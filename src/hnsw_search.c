#include "hnsw_search.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#define TOP_K       5
#define DIM         14
#define EF_SEARCH 80


static uint32_t *g_visited       = NULL;
static int       g_visited_size  = 0;
static uint32_t  g_visited_epoch = 0;

int hnsw_search_init(int size)
{
    free(g_visited);
    g_visited = calloc((size_t)size, sizeof(uint32_t));
    if (!g_visited) return -1;
    g_visited_size  = size;
    g_visited_epoch = 0;
    return 0;
}

static float dist2(const float *a, const float *b)
{
    float s0=0, s1=0, s2=0, s3=0;

    for (int i = 0; i < 12; i += 4) {
        float d0 = a[i]-b[i], d1 = a[i+1]-b[i+1],
              d2 = a[i+2]-b[i+2], d3 = a[i+3]-b[i+3];
        s0+=d0*d0; s1+=d1*d1; s2+=d2*d2; s3+=d3*d3;
    }

    float d12 = a[12]-b[12], d13 = a[13]-b[13];
    return s0+s1+s2+s3 + d12*d12 + d13*d13;
}



static int search_layer(hnsw_header_t *h, const float *q,
                        int entry, int layer,
                        int *out_idx, float *out_dist, int ef)
{
    if (++g_visited_epoch == 0) {
        if (g_visited && g_visited_size > 0) {
            memset(g_visited, 0,
                   (size_t)g_visited_size * sizeof(uint32_t));
        }
        g_visited_epoch = 1;
    }

    enum { CAP = EF_SEARCH * 4 };
    int   cand_idx[CAP];
    float cand_dist[CAP];
    int   found_idx[EF_SEARCH];
    float found_dist[EF_SEARCH];

    int cand_n = 0, found_n = 0;

    float d0 = dist2(q, h->nodes[entry].vector);
    cand_idx[cand_n]   = entry; cand_dist[cand_n++]  = d0;
    found_idx[found_n] = entry; found_dist[found_n++] = d0;
    g_visited[entry] = g_visited_epoch;

    while (cand_n > 0) {
        int best_i = 0;
        for (int i = 1; i < cand_n; i++)
            if (cand_dist[i] < cand_dist[best_i]) best_i = i;

        int   c_idx  = cand_idx[best_i];
        float c_dist = cand_dist[best_i];

        cand_idx[best_i]  = cand_idx[--cand_n];
        cand_dist[best_i] = cand_dist[cand_n];

        float worst = 0;
        for (int i = 0; i < found_n; i++)
            if (found_dist[i] > worst) worst = found_dist[i];

        if (c_dist > worst && found_n >= ef) break;

        hnsw_node_t *node = &h->nodes[c_idx];
        int nb_count = node->neighbor_count[layer];
        if (nb_count < 0 || nb_count > M) continue; 

        for (int j = 0; j < nb_count; j++) {
            int nb = node->neighbors[layer][j];
            if (nb < 0 || nb >= h->size) continue;
            if (g_visited[nb] == g_visited_epoch) continue;
            g_visited[nb] = g_visited_epoch;

            float nd = dist2(q, h->nodes[nb].vector);

            float worst2 = 0; int worst_pos = 0;
            for (int i = 0; i < found_n; i++)
                if (found_dist[i] > worst2) { worst2 = found_dist[i]; worst_pos = i; }

            if (found_n < ef || nd < worst2) {
                if (cand_n < CAP) {
                    cand_idx[cand_n]    = nb;
                    cand_dist[cand_n++] = nd;
                }
                if (found_n < ef) {
                    found_idx[found_n]    = nb;
                    found_dist[found_n++] = nd;
                } else {
                    found_idx[worst_pos]  = nb;
                    found_dist[worst_pos] = nd;
                }
            }
        }
    }

    for (int i = 1; i < found_n; i++) {
        float kd = found_dist[i]; int ki = found_idx[i];
        int j = i - 1;
        while (j >= 0 && found_dist[j] > kd) {
            found_dist[j+1] = found_dist[j];
            found_idx[j+1]  = found_idx[j];
            j--;
        }
        found_dist[j+1] = kd;
        found_idx[j+1]  = ki;
    }

    int out_n = (found_n < ef) ? found_n : ef;
    memcpy(out_idx,  found_idx,  out_n * sizeof(int));
    memcpy(out_dist, found_dist, out_n * sizeof(float));
    return out_n;
}


int greedy_search_layer(hnsw_header_t *h, float *q, int entry, int level)
{   
    if (!h || !q) return -1;
    if (entry < 0 || entry >= h->size) return -1;
    if (level < 0 || level >= MAX_LEVEL) return -1;

    int current = entry;
    int changed;
    do {
        changed = 0;
        hnsw_node_t *n = &h->nodes[current];
        int nb_count = n->neighbor_count[level];
        if (nb_count < 0 || nb_count > M) return -1;

        float d_cur = dist2(q, n->vector);
        for (int i = 0; i < nb_count; i++) {
            int nb = n->neighbors[level][i];
            if (nb < 0 || nb >= h->size) continue;
            float d_nb = dist2(q, h->nodes[nb].vector);
            if (d_nb < d_cur) {
                current = nb;
                d_cur   = d_nb; 
                changed = 1;
                break;
            }
        }
    } while (changed);

    return current;
}

int search(hnsw_header_t *h, float *q, int *idx_out, float *dist_out)
{
    if (!h || !q || !idx_out || !dist_out) {
        printf("search: invalid arguments (null)\n");
        return -1;
    }
    if (h->size <= 0 || !h->nodes) {
        printf("search: empty hnsw structure\n");
        return -1;
    }

    for (int i = 0; i < TOP_K; i++) {
        idx_out[i]  = -1;
        dist_out[i] = 1.0f / 0.0f;
    }

    int entry = h->entry_point;
    if (entry < 0 || entry >= h->size) {
        printf("search: invalid entry_point %d\n", entry);
        return -1;
    }

    for (int l = h->max_level; l > 0; l--) {
        int next = greedy_search_layer(h, q, entry, l);
        entry = next;
    }
    int   ef_idx[EF_SEARCH];
    float ef_dist[EF_SEARCH];
    int cnt = search_layer(h, q, entry, 0, ef_idx, ef_dist, EF_SEARCH);

    int take = (cnt < TOP_K) ? cnt : TOP_K;
    for (int i = 0; i < take; i++) {
        idx_out[i]  = ef_idx[i];
        dist_out[i] = ef_dist[i];
    }

    return 0;
}
