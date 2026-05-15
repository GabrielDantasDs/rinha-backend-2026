/* ============================================================
 * Runtime search over the compact-format HNSW index.
 *
 * Reads:  hnsw_header_t (pointers into mmap of hnsw_index.bin)
 * Writes: top-K neighbor indices + squared distances
 *
 * Both build and search use uint8 quantized vectors and uint32
 * squared distances, so query-time ordering matches what the
 * builder optimized for.
 * ============================================================ */

#include "hnsw_search.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define TOP_K 5

/* ============================================================
 * Quantization — public, used both here and by the request
 * pipeline that converts a query payload into the search input.
 * ============================================================ */

void hnsw_quantize(const float *v, uint8_t *out, float qscale)
{
    for (int i = 0; i < 14; i++)
    {
        float q = (v[i] + 1.0f) * qscale;
        if (q < 0.0f)
            q = 0.0f;
        if (q > 255.0f)
            q = 255.0f;
        out[i] = (uint8_t)(q + 0.5f);
    }
}

/* ============================================================
 * Distance — uint8 inputs, uint32 output.
 * Max value: 14 * 255^2 = 910,350 — fits comfortably in uint32.
 * ============================================================ */

static inline uint32_t dist2(const uint8_t *a, const uint8_t *b)
{
    uint32_t s = 0;
    int valid = 0;

    for (int i = 0; i < 14; i++)
    {

        if (i == 11 && (a[i] == 1 && b[i] == 1))
            continue;
        if (a[i] == 0 || b[i] == 0)
            continue;

        int d = (int)a[i] - (int)b[i];

        s += (uint32_t)(d * d);

        valid++;
    }

    if (valid == 0)
        return UINT32_MAX;

    return (s * 14) / valid;
}

/* ============================================================
 * Visited tracking — epoch-stamped buffer, one allocation at startup.
 * ============================================================ */

static uint32_t *g_visited = NULL;
static int g_visited_size = 0;
static uint32_t g_visited_epoch = 0;

int hnsw_search_init(int size)
{
    free(g_visited);
    g_visited = calloc((size_t)size, sizeof(uint32_t));
    if (!g_visited)
        return -1;
    g_visited_size = size;
    g_visited_epoch = 0;
    return 0;
}

static inline void visited_clear(void)
{
    if (++g_visited_epoch == 0)
    {
        memset(g_visited, 0, (size_t)g_visited_size * sizeof(uint32_t));
        g_visited_epoch = 1;
    }
}

static inline int visited_test_and_set(int i)
{
    if (g_visited[i] == g_visited_epoch)
        return 1;
    g_visited[i] = g_visited_epoch;
    return 0;
}

/* ============================================================
 * Neighbor accessor for the compact format.
 *
 * Layer 0 is a flat slice of `h->l0_blob` of packed uint24 entries.
 * Higher layers are packed into a per-node variable-length record
 * in `h->high_blob`:
 *
 *   record layout at byte offset n->high_offset:
 *     uint8_t  count[level];           // count for levels 1..level
 *     uint8_t  neighbors[3 * total];   // packed uint24, flat:
 *                                      //   count[0] entries for layer 1,
 *                                      //   count[1] entries for layer 2, ...
 *
 * Returns a byte pointer to the first uint24 entry; use unpack_u24 to
 * decode each. Returns (NULL, 0) if `layer` is invalid for this node.
 * ============================================================ */

static inline void get_neighbors(const hnsw_header_t *h,
                                 const node_t *n, int layer,
                                 const uint8_t **out_arr, int *out_count)
{
    if (layer == 0)
    {
        *out_arr = h->l0_blob + (size_t)n->l0_offset * 3u;
        *out_count = (int)n->l0_count;
        return;
    }

    if (n->high_offset == UINT32_MAX || layer > (int)n->level)
    {
        *out_arr = NULL;
        *out_count = 0;
        return;
    }

    const uint8_t *rec = h->high_blob + n->high_offset;
    int L = (int)n->level;
    int counts_bytes = L;

    /* skip past the counts to reach the packed uint24 neighbor array */
    const uint8_t *neighbors_base = rec + counts_bytes;

    /* index within the flat array: sum of counts for layers 1..layer-1 */
    int idx = 0;
    for (int l = 1; l < layer; l++)
    {
        idx += (int)rec[l - 1];
    }

    *out_arr = neighbors_base + (size_t)idx * 3u;
    *out_count = (int)rec[layer - 1];
}

/* ============================================================
 * Beam search on one layer
 * ============================================================ */

#define CAND_CAP (EF_SEARCH * 4)

static int search_layer(hnsw_header_t *h, const uint8_t *q,
                        int entry, int layer,
                        int *out_idx, uint32_t *out_dist, int ef)
{
    visited_clear();

    /* stack-allocated work arrays — small at EF_SEARCH=16 */
    int cand_idx[CAND_CAP];
    uint32_t cand_dist[CAND_CAP];
    int found_idx[EF_SEARCH];
    uint32_t found_dist[EF_SEARCH];

    int cand_n = 0, found_n = 0;

    uint32_t d0 = dist2(q, h->nodes[entry].qvec);
    cand_idx[cand_n] = entry;
    cand_dist[cand_n++] = d0;
    found_idx[found_n] = entry;
    found_dist[found_n++] = d0;
    visited_test_and_set(entry);

    while (cand_n > 0)
    {
        /* pop the candidate with the smallest distance */
        int best_i = 0;
        for (int i = 1; i < cand_n; i++)
            if (cand_dist[i] < cand_dist[best_i])
                best_i = i;
        int c_idx = cand_idx[best_i];
        uint32_t c_dist = cand_dist[best_i];
        cand_idx[best_i] = cand_idx[--cand_n];
        cand_dist[best_i] = cand_dist[cand_n];

        /* worst-of-found pruning check */
        uint32_t worst = 0;
        for (int i = 0; i < found_n; i++)
            if (found_dist[i] > worst)
                worst = found_dist[i];
        if (c_dist > worst && found_n >= ef)
            break;

        const node_t *node = &h->nodes[c_idx];
        const uint8_t *nb_arr;
        int nb_count;
        get_neighbors(h, node, layer, &nb_arr, &nb_count);

        for (int j = 0; j < nb_count; j++)
        {
            int nb = (int)unpack_u24(nb_arr + (size_t)j * 3u);
            if (nb < 0 || nb >= (int)h->size)
                continue;
            if (visited_test_and_set(nb))
                continue;

            uint32_t nd = dist2(q, h->nodes[nb].qvec);

            uint32_t worst2 = 0;
            int worst_p = 0;
            for (int i = 0; i < found_n; i++)
                if (found_dist[i] > worst2)
                {
                    worst2 = found_dist[i];
                    worst_p = i;
                }

            if (found_n < ef || nd < worst2)
            {
                if (cand_n < CAND_CAP)
                {
                    cand_idx[cand_n] = nb;
                    cand_dist[cand_n++] = nd;
                }
                if (found_n < ef)
                {
                    found_idx[found_n] = nb;
                    found_dist[found_n++] = nd;
                }
                else
                {
                    found_idx[worst_p] = nb;
                    found_dist[worst_p] = nd;
                }
            }
        }
    }

    /* insertion sort by ascending distance — ef is small */
    for (int i = 1; i < found_n; i++)
    {
        uint32_t kd = found_dist[i];
        int ki = found_idx[i];
        int j = i - 1;
        while (j >= 0 && found_dist[j] > kd)
        {
            found_dist[j + 1] = found_dist[j];
            found_idx[j + 1] = found_idx[j];
            j--;
        }
        found_dist[j + 1] = kd;
        found_idx[j + 1] = ki;
    }

    int out_n = (found_n < ef) ? found_n : ef;
    memcpy(out_idx, found_idx, out_n * sizeof(int));
    memcpy(out_dist, found_dist, out_n * sizeof(uint32_t));
    return out_n;
}

/* ============================================================
 * Greedy descent on one upper layer (used during top-down search).
 * Returns the node id with smallest distance to q reachable from
 * `entry` by repeatedly moving to a closer neighbor.
 * ============================================================ */

static int greedy_search_layer(hnsw_header_t *h, const uint8_t *q,
                               int entry, int level)
{
    if (entry < 0 || entry >= (int)h->size)
        return entry;
    if (level < 1 || level >= MAX_LEVEL)
        return entry;

    int current = entry;
    int changed;
    do
    {
        changed = 0;
        const node_t *n = &h->nodes[current];
        const uint8_t *nb_arr;
        int nb_count;
        get_neighbors(h, n, level, &nb_arr, &nb_count);

        uint32_t d_cur = dist2(q, n->qvec);
        // fprintf(stderr, "greedy_search_layer: current=%d d_cur=%u nb_count=%d\n",
        //         current, d_cur, nb_count);
        for (int i = 0; i < nb_count; i++)
        {
            int nb = (int)unpack_u24(nb_arr + (size_t)i * 3u);
            if (nb < 0 || nb >= (int)h->size)
                continue;
            uint32_t d_nb = dist2(q, h->nodes[nb].qvec);
            if (d_nb < d_cur)
            {
                current = nb;
                d_cur = d_nb;
                changed = 1;
                break;
            }
        }
    } while (changed);

    return current;
}

/* ============================================================
 * Public search entry point.
 * ============================================================ */

int search(hnsw_header_t *h, float *q, int *idx_out, float *dist_out)
{
    if (!h || !q || !idx_out || !dist_out)
        return -1;
    if (h->size == 0 || !h->nodes || !h->l0_blob)
        return -1;

    /* sentinels */
    for (int i = 0; i < TOP_K; i++)
    {
        idx_out[i] = -1;
        dist_out[i] = 1e30f;
    }

    int entry = (int)h->entry_point;
    if (entry < 0 || entry >= (int)h->size)
        return -1;

    /* quantize the float query into uint8 using the index's saved scale */
    uint8_t qq[14];
    hnsw_quantize(q, qq, h->qscale);

    /* Phase 1: greedy descent from max_level down to 1 (ef=1, fast) */
    for (int l = (int)h->max_level; l > 0; l--)
    {
        entry = greedy_search_layer(h, qq, entry, l);
    }

    /* Phase 2: beam search at layer 0 with EF_SEARCH candidates */
    int ef_idx[EF_SEARCH];
    uint32_t ef_dist[EF_SEARCH];
    int cnt = search_layer(h, qq, entry, 0, ef_idx, ef_dist, EF_SEARCH);

    /* fill the top-K outputs (already sorted ascending by distance) */
    int take = (cnt < TOP_K) ? cnt : TOP_K;
    for (int i = 0; i < take; i++)
    {
        idx_out[i] = ef_idx[i];
        dist_out[i] = (float)ef_dist[i];
    }
    return 0;
}
