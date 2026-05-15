/* ============================================================
 * Builder for the compact-format HNSW index.
 *
 * Reads:  preprocessed_data.bin       (float vectors + uint8 labels)
 * Writes: hnsw_index.bin              (compact format defined in hnsw_search.h)
 *
 * Graph is built in memory using quantized uint8 vectors and uint32
 * squared distances — same metric the runtime search uses, so the
 * resulting graph is optimized for the exact distance ordering the
 * server will see.
 *
 * Build-time peak RAM: ~2 GB (608 B per build_node_t × 3M).
 * Output file size:    ~400 MB.
 * ============================================================ */

#include "hnsw_search.h"      /* macros: M, M0, MAX_LEVEL — disk types */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DIM             14
#define EF_CONSTRUCTION 80      /* candidates explored per insert */

/* ============================================================
 * Build-time types — fat in-memory representation
 * ============================================================ */

typedef struct {
    uint8_t  qvec[DIM];           /* quantized at load time, reused everywhere */
    uint8_t  label;
    uint8_t  level;
    int      neighbors_l0[M0];
    int      neighbors_upper[MAX_LEVEL - 1][M];
    uint8_t  neighbor_count[MAX_LEVEL];
} build_node_t;

typedef struct {
    build_node_t *nodes;
    int           size;
    int           entry_point;
    int           max_level;
} build_header_t;

/* Input file format. */
typedef struct {
    float         vector[DIM];
    unsigned char label;
} item_t;

/* Candidate during a beam search. dist is uint32 (max 14*255^2 = 910350). */
typedef struct {
    int      idx;
    uint32_t dist;
} candidate_t;

/* ============================================================
 * Quantization
 * ============================================================ */

static void quantize_vector(const float *v, uint8_t *out)
{
    for (int i = 0; i < DIM; i++) {
        float q = (v[i] + 1.0f) * QSCALE_DEFAULT;
        if (q < 0.0f)   q = 0.0f;
        if (q > 255.0f) q = 255.0f;
        out[i] = (uint8_t)(q + 0.5f);   /* round to nearest */
    }
}

/* ============================================================
 * Distance — uint8 inputs, uint32 squared distance.
 * Same ordering as the float version (sum of squared differences),
 * so top-K is identical up to quantization-induced ties.
 * ============================================================ */

static inline uint32_t dist2(const uint8_t *a, const uint8_t *b)
{
    uint32_t s = 0;
    int valid = 0;

    for (int i = 0; i < 14; i++) {

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
 * Visited tracking — epoch-stamped, allocated once at build start.
 *
 * Replaces the calloc(3M) + free that used to happen in every
 * search_layer call. ~21M calls during a full build, so this is
 * a major perf win.
 * ============================================================ */

static uint32_t *g_visited       = NULL;
static int       g_visited_size  = 0;
static uint32_t  g_visited_epoch = 0;

static void visited_init(int size)
{
    g_visited = calloc((size_t)size, sizeof(uint32_t));
    if (!g_visited) {
        fprintf(stderr, "visited_init: out of memory\n");
        exit(1);
    }
    g_visited_size  = size;
    g_visited_epoch = 0;
}

static inline void visited_clear(void)
{
    if (++g_visited_epoch == 0) {
        memset(g_visited, 0, (size_t)g_visited_size * sizeof(uint32_t));
        g_visited_epoch = 1;
    }
}

static inline int visited_test_and_set(int i)
{
    if (g_visited[i] == g_visited_epoch) return 1;
    g_visited[i] = g_visited_epoch;
    return 0;
}

/* ============================================================
 * Beam search on one layer of the build graph.
 *
 * Returns up to `ef` nearest neighbors of `query` (in sorted order
 * by ascending distance), starting from `entry`. Uses stack-bounded
 * work arrays — no malloc.
 * ============================================================ */

static int cmp_candidates_asc(const void *a, const void *b)
{
    uint32_t da = ((const candidate_t *)a)->dist;
    uint32_t db = ((const candidate_t *)b)->dist;
    return (da < db) ? -1 : (da > db) ? 1 : 0;
}

/* Bound for the candidate frontier. EF_CONSTRUCTION=400 → CAP=1600 max. */
#define CAND_CAP (EF_CONSTRUCTION * 4)

static int search_layer(build_header_t *h, const uint8_t *query,
                        int entry, int layer,
                        candidate_t *out, int ef)
{
    visited_clear();

    /* stack-allocated work arrays */
    int      cand_idx[CAND_CAP];
    uint32_t cand_dist[CAND_CAP];
    int      found_idx[EF_CONSTRUCTION];
    uint32_t found_dist[EF_CONSTRUCTION];

    int cand_n = 0, found_n = 0;

    uint32_t d0 = dist2(query, h->nodes[entry].qvec);
    cand_idx[cand_n]   = entry; cand_dist[cand_n++]   = d0;
    found_idx[found_n] = entry; found_dist[found_n++] = d0;
    visited_test_and_set(entry);

    int cap_for_layer = (layer == 0) ? M0 : M;
    (void)cap_for_layer; /* used in the guard below */

    while (cand_n > 0) {
        /* pop the candidate with the smallest distance */
        int best_i = 0;
        for (int i = 1; i < cand_n; i++)
            if (cand_dist[i] < cand_dist[best_i]) best_i = i;
        int      c_idx  = cand_idx[best_i];
        uint32_t c_dist = cand_dist[best_i];
        cand_idx[best_i]  = cand_idx[--cand_n];
        cand_dist[best_i] = cand_dist[cand_n];

        /* worst distance among current found */
        uint32_t worst = 0;
        for (int i = 0; i < found_n; i++)
            if (found_dist[i] > worst) worst = found_dist[i];

        /* if the best candidate is already worse than our worst keeper, stop */
        if (c_dist > worst && found_n >= ef) break;

        build_node_t *node = &h->nodes[c_idx];
        int          nb_count = node->neighbor_count[layer];
        const int   *nb_arr   = (layer == 0)
                                ? node->neighbors_l0
                                : node->neighbors_upper[layer - 1];

        for (int j = 0; j < nb_count; j++) {
            int nb = nb_arr[j];
            if (nb < 0 || nb >= h->size) continue;
            if (visited_test_and_set(nb)) continue;

            uint32_t nd = dist2(query, h->nodes[nb].qvec);

            /* re-find worst-of-found (its position may have changed) */
            uint32_t worst2  = 0;
            int      worst_p = 0;
            for (int i = 0; i < found_n; i++)
                if (found_dist[i] > worst2) { worst2 = found_dist[i]; worst_p = i; }

            if (found_n < ef || nd < worst2) {
                if (cand_n < CAND_CAP) {
                    cand_idx[cand_n]    = nb;
                    cand_dist[cand_n++] = nd;
                }
                if (found_n < ef) {
                    found_idx[found_n]    = nb;
                    found_dist[found_n++] = nd;
                } else {
                    found_idx[worst_p]  = nb;
                    found_dist[worst_p] = nd;
                }
            }
        }
    }

    /* sort the found list ascending by distance, copy to out[] */
    candidate_t tmp[EF_CONSTRUCTION];
    for (int i = 0; i < found_n; i++) {
        tmp[i].idx  = found_idx[i];
        tmp[i].dist = found_dist[i];
    }
    qsort(tmp, found_n, sizeof(candidate_t), cmp_candidates_asc);

    int out_n = (found_n < ef) ? found_n : ef;
    memcpy(out, tmp, out_n * sizeof(candidate_t));
    return out_n;
}

/* ============================================================
 * Random level — geometric distribution per the HNSW paper.
 *   P(level >= L) = (1/M)^L
 * ============================================================ */

static int random_level(void)
{
    double mL = 1.0 / log((double)M);
    double r  = (double)(rand() + 1) / ((double)RAND_MAX + 2);  /* avoid log(0) */
    int    L  = (int)(-log(r) * mL);
    if (L < 0)              L = 0;
    if (L >= MAX_LEVEL)     L = MAX_LEVEL - 1;
    return L;
}

/* ============================================================
 * Insert one node into the graph.
 * ============================================================ */

static void insert(build_header_t *h, int idx)
{
    build_node_t *n          = &h->nodes[idx];
    int           node_level = n->level;

    /* first node: just becomes the entry point */
    if (h->entry_point == -1) {
        h->entry_point = idx;
        h->max_level   = node_level;
        return;
    }

    int current = h->entry_point;

    /* Phase 1: greedy descent from top level down to node_level + 1.
     * Goal is to find a good entry point for the dense-search phase. */
    candidate_t best1[1];
    for (int l = h->max_level; l > node_level; l--) {
        if (search_layer(h, n->qvec, current, l, best1, 1) > 0)
            current = best1[0].idx;
    }

    /* Phase 2: dense beam search at each layer from node_level down to 0,
     * connecting the new node to the M/M0 best candidates at each level. */
    candidate_t candidates[EF_CONSTRUCTION];

    for (int l = node_level; l >= 0; l--) {
        int cnt = search_layer(h, n->qvec, current, l,
                               candidates, EF_CONSTRUCTION);
        if (cnt == 0) continue;
        current = candidates[0].idx;     /* best entry for the next layer down */

        int cap_l = (l == 0) ? M0 : M;
        int *n_arr = (l == 0) ? n->neighbors_l0
                              : n->neighbors_upper[l - 1];

        int connect = (cnt < cap_l) ? cnt : cap_l;
        for (int i = 0; i < connect; i++) {
            int           nb_idx = candidates[i].idx;
            build_node_t *nb     = &h->nodes[nb_idx];
            int          *nb_arr = (l == 0) ? nb->neighbors_l0
                                            : nb->neighbors_upper[l - 1];

            /* forward edge: new node → neighbor */
            if (n->neighbor_count[l] < cap_l)
                n_arr[n->neighbor_count[l]++] = nb_idx;

            /* backward edge: neighbor → new node (bidirectional) */
            if (nb->neighbor_count[l] < cap_l) {
                nb_arr[nb->neighbor_count[l]++] = idx;
            } else {
                /* prune: replace nb's worst neighbor if n is closer */
                int      worst_pos  = 0;
                uint32_t worst_dist = 0;
                for (int j = 0; j < nb->neighbor_count[l]; j++) {
                    uint32_t d = dist2(nb->qvec, h->nodes[nb_arr[j]].qvec);
                    if (d > worst_dist) { worst_dist = d; worst_pos = j; }
                }
                int dist = dist2(nb->qvec, n->qvec);
                if (dist < worst_dist) {  /* sanity check: don't connect if it's way worse */
                    nb_arr[worst_pos] = idx;
                }
            }
        }
    }

    /* promote entry point if this node climbed higher than any existing */
    if (node_level > h->max_level) {
        h->entry_point = idx;
        h->max_level   = node_level;
    }
}

/* ============================================================
 * Serialize the in-memory build graph to the compact disk format.
 *
 * Layout (already documented in hnsw_search.h):
 *   [ disk_header_t ]
 *   [ node_t × size ]
 *   [ l0_blob: uint32_t × l0_blob_count ]
 *   [ high_blob: variable per-node records ]
 * ============================================================ */

/* Bytes used by the high_blob record for one node at top level L.
 *   counts[L] uint8_t, then padding to 4-byte alignment, then 4*sum(counts) bytes.
 * Total is always a multiple of 4, keeping subsequent records aligned. */
static uint32_t high_record_bytes(const build_node_t *n)
{
    int L = (int)n->level;
    if (L == 0) return 0;

    uint32_t counts_bytes = (uint32_t)L;
    uint32_t pad_to_4     = (4 - (counts_bytes & 3)) & 3;
    uint32_t nb_total     = 0;
    for (int l = 1; l <= L; l++) {
        nb_total += (uint32_t)n->neighbor_count[l];
    }
    return counts_bytes + pad_to_4 + nb_total * (uint32_t)sizeof(uint32_t);
}

static int serialize_compact(const build_header_t *h, const char *path)
{
    int N = h->size;

    /* ── Pass 1: compute per-node offsets and total blob sizes ── */
    uint32_t *l0_offsets   = malloc((size_t)N * sizeof(uint32_t));
    uint32_t *high_offsets = malloc((size_t)N * sizeof(uint32_t));
    if (!l0_offsets || !high_offsets) {
        fprintf(stderr, "serialize: out of memory\n");
        free(l0_offsets); free(high_offsets);
        return -1;
    }

    uint32_t l0_total   = 0;
    uint32_t high_total = 0;
    for (int i = 0; i < N; i++) {
        const build_node_t *n = &h->nodes[i];
        l0_offsets[i] = l0_total;
        l0_total += (uint32_t)n->neighbor_count[0];

        if (n->level == 0) {
            high_offsets[i] = UINT32_MAX;
        } else {
            high_offsets[i] = high_total;
            high_total += high_record_bytes(n);
        }
    }

    /* ── Pass 2: materialize blobs ── */
    uint32_t *l0_blob = malloc((size_t)l0_total * sizeof(uint32_t));
    uint8_t  *high_blob = (high_total > 0) ? malloc(high_total) : NULL;
    if ((!l0_blob && l0_total > 0) || (!high_blob && high_total > 0)) {
        fprintf(stderr, "serialize: out of memory for blobs\n");
        free(l0_offsets); free(high_offsets);
        free(l0_blob); free(high_blob);
        return -1;
    }

    {
        uint32_t l0_cursor   = 0;
        uint32_t high_cursor = 0;
        for (int i = 0; i < N; i++) {
            const build_node_t *n = &h->nodes[i];

            /* level-0 neighbors → l0_blob */
            int l0c = n->neighbor_count[0];
            for (int j = 0; j < l0c; j++) {
                l0_blob[l0_cursor++] = (uint32_t)n->neighbors_l0[j];
            }

            /* level 1+ neighbors → high_blob (variable-length record) */
            int L = n->level;
            if (L == 0) continue;

            uint8_t *rec = high_blob + high_cursor;
            /* counts[L] one byte each */
            for (int l = 1; l <= L; l++) {
                rec[l - 1] = n->neighbor_count[l];
            }
            /* zero the alignment padding */
            uint32_t counts_bytes = (uint32_t)L;
            uint32_t pad_to_4     = (4 - (counts_bytes & 3)) & 3;
            for (uint32_t p = 0; p < pad_to_4; p++) {
                rec[counts_bytes + p] = 0;
            }
            /* neighbors[] uint32_t, flat */
            uint32_t off_into_rec = counts_bytes + pad_to_4;
            for (int l = 1; l <= L; l++) {
                int nbc = n->neighbor_count[l];
                for (int j = 0; j < nbc; j++) {
                    uint32_t nbid = (uint32_t)n->neighbors_upper[l - 1][j];
                    memcpy(rec + off_into_rec, &nbid, sizeof(uint32_t));
                    off_into_rec += sizeof(uint32_t);
                }
            }

            high_cursor += high_record_bytes(n);
        }
    }

    /* ── Write the file ── */
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror("fopen");
        free(l0_offsets); free(high_offsets);
        free(l0_blob); free(high_blob);
        return -1;
    }

    disk_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic           = INDEX_MAGIC;
    hdr.version         = INDEX_VERSION;
    hdr.size            = (uint32_t)N;
    hdr.entry_point     = (uint32_t)h->entry_point;
    hdr.max_level       = (uint8_t)h->max_level;
    hdr.m               = (uint8_t)M;
    hdr.m0             = (uint8_t)M0;
    hdr.qscale          = QSCALE_DEFAULT;
    hdr.l0_blob_count   = l0_total;
    hdr.high_blob_size  = high_total;

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) goto wrerr;

    /* compact node_t records, in node-id order */
    for (int i = 0; i < N; i++) {
        const build_node_t *bn = &h->nodes[i];
        node_t out;
        memset(&out, 0, sizeof(out));
        memcpy(out.qvec, bn->qvec, DIM);
        out.label       = bn->label;
        out.level       = bn->level;
        out.l0_count    = bn->neighbor_count[0];
        out.l0_offset   = l0_offsets[i];
        out.high_offset = high_offsets[i];
        if (fwrite(&out, sizeof(out), 1, f) != 1) goto wrerr;
    }

    /* l0 blob */
    if (l0_total > 0) {
        if (fwrite(l0_blob, sizeof(uint32_t), l0_total, f) != l0_total) goto wrerr;
    }

    /* high blob */
    if (high_total > 0) {
        if (fwrite(high_blob, 1, high_total, f) != high_total) goto wrerr;
    }

    fclose(f);
    free(l0_offsets); free(high_offsets);
    free(l0_blob); free(high_blob);
    return 0;

wrerr:
    perror("fwrite");
    fclose(f);
    free(l0_offsets); free(high_offsets);
    free(l0_blob); free(high_blob);
    return -1;
}

/* ============================================================
 * Top-level build entry point
 * ============================================================ */

static void init_header(build_header_t *h, build_node_t *nodes, int size)
{
    h->nodes       = nodes;
    h->size        = size;
    h->entry_point = -1;
    h->max_level   = 0;
}

static int build_hnsw(const char *data_file, const char *index_file)
{
    /* Deterministic seed so rebuilds are reproducible during debugging.
     * Change once we're committed to a final build. */
    srand(1001);

    int fd = open(data_file, O_RDONLY);
    if (fd < 0) { perror("open"); return -1; }

    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return -1; }

    item_t *items = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (items == MAP_FAILED) { perror("mmap"); close(fd); return -1; }

    int size = (int)(st.st_size / sizeof(item_t));
    fprintf(stderr, "[build] %d items in %s (sizeof item_t=%zu)\n",
            size, data_file, sizeof(item_t));

    /* allocate fat build nodes */
    build_node_t *nodes = calloc((size_t)size, sizeof(build_node_t));
    if (!nodes) {
        fprintf(stderr, "calloc failed\n");
        munmap(items, st.st_size); close(fd);
        return -1;
    }

    /* quantize each vector once; pick a random level */
    for (int i = 0; i < size; i++) {
        quantize_vector(items[i].vector, nodes[i].qvec);
        nodes[i].label = items[i].label;
        nodes[i].level = (uint8_t)random_level();
        for (int l = 0; l < MAX_LEVEL; l++) nodes[i].neighbor_count[l] = 0;
    }

    build_header_t h;
    init_header(&h, nodes, size);

    /* visited buffer for all search_layer calls */
    visited_init(size);

    /* insert one by one */
    for (int i = 0; i < size; i++) {
        insert(&h, i);
        if (i > 0 && (i % 100000) == 0) {
            fprintf(stderr, "[build] %d / %d inserted (max_level=%d)\n",
                    i, size, h.max_level);
        }
    }

    /* level histogram — helpful sanity check */
    int hist[MAX_LEVEL] = {0};
    for (int i = 0; i < size; i++) hist[nodes[i].level]++;
    for (int l = 0; l < MAX_LEVEL; l++) {
        if (hist[l] > 0) {
            fprintf(stderr, "[build] level %d: %d nodes (%.3f%%)\n",
                    l, hist[l], 100.0 * hist[l] / size);
        }
    }
    fprintf(stderr, "[build] entry_point=%d max_level=%d\n",
            h.entry_point, h.max_level);

    int rc = serialize_compact(&h, index_file);
    if (rc == 0) {
        fprintf(stderr, "[build] wrote %s\n", index_file);
    }

    free(g_visited);
    free(nodes);
    munmap(items, st.st_size);
    close(fd);
    return rc;
}

int main(void)
{
    return build_hnsw("preprocessed_data.bin", "hnsw_index.bin");
}
