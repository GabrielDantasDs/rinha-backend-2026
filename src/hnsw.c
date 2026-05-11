#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <time.h>

#define MAX_LEVEL 6
#define M 12
#define EF_SEARCH 80
#define EF_CONSTRUCTION 400 /* quantos candidatos explorar ao inserir */
#define DIM 14

typedef struct node
{
    float vector[DIM];
    uint8_t label;

    int neighbors[MAX_LEVEL][M];
    int neighbor_count[MAX_LEVEL];

    int level;
} hnsw_node_t;

typedef struct
{
    hnsw_node_t *nodes;
    int size;
    int entry_point;
    int max_level;
} hnsw_header_t;

typedef struct
{
    int size;
    int entry_point;
    int max_level;
} hnsw_disk_header_t;

typedef struct
{
    float vector[DIM];
    unsigned char label;
} item_t;

typedef struct
{
    int idx;
    float dist;
} candidate_t;

static int compare_candidates_asc(const void *a, const void *b)
{
    float diff = ((candidate_t *)a)->dist - ((candidate_t *)b)->dist;
    return (diff < 0) ? -1 : (diff > 0) ? 1
                                        : 0;
}

static float dist2(const float *a, const float *b)
{
    float s = 0;
    for (int i = 0; i < DIM; i++)
    {
        float d = a[i] - b[i];
        s += d * d;
    }
    return s;
}

/* ------------------------------------------------------------------ *
 * search_layer                                                         *
 *   Beam search em uma camada. Retorna os `ef` vizinhos mais próximos  *
 *   de `query` a partir de `entry`, ordenados por distância crescente. *
 * ------------------------------------------------------------------ */
static int search_layer(hnsw_header_t *h, const float *query,
                        int entry, int layer,
                        candidate_t *out, int ef,
                        int max_nodes) /* para alocar visited */
{
    bool *visited = calloc(max_nodes, sizeof(bool));
    if (!visited)
        return 0;

    /* candidatos a explorar (usamos array simples – tamanho conservador) */
    int cap = h->size;
    candidate_t *cands = malloc(cap * sizeof(candidate_t));
    candidate_t *found = malloc(cap * sizeof(candidate_t));
    int cand_n = 0, found_n = 0;

    float d0 = dist2(query, h->nodes[entry].vector);
    cands[cand_n++] = (candidate_t){entry, d0};
    found[found_n++] = (candidate_t){entry, d0};
    visited[entry] = true;

    while (cand_n > 0)
    {
        /* menor distância entre os candidatos */
        int best_i = 0;
        for (int i = 1; i < cand_n; i++)
            if (cands[i].dist < cands[best_i].dist)
                best_i = i;
        candidate_t c = cands[best_i];
        cands[best_i] = cands[--cand_n];

        /* maior distância entre os achados */
        float worst = 0;
        for (int i = 0; i < found_n; i++)
            if (found[i].dist > worst)
                worst = found[i].dist;

        if (c.dist > worst && found_n >= ef)
            break; /* poda */

        hnsw_node_t *node = &h->nodes[c.idx];
        for (int j = 0; j < node->neighbor_count[layer]; j++)
        {
            int nb = node->neighbors[layer][j];
            if (nb < 0 || nb >= max_nodes || visited[nb])
                continue;
            visited[nb] = true;

            float nd = dist2(query, h->nodes[nb].vector);

            /* recalcula pior achado */
            float worst2 = 0;
            int worst_pos = 0;
            for (int i = 0; i < found_n; i++)
                if (found[i].dist > worst2)
                {
                    worst2 = found[i].dist;
                    worst_pos = i;
                }

            if (found_n < ef || nd < worst2)
            {
                /* adiciona ao heap de candidatos */

                if (found_n < ef)
                    found[found_n++] = (candidate_t){nb, nd};
                else
                    found[worst_pos] = (candidate_t){nb, nd};
            }
        }
    }

    qsort(found, found_n, sizeof(candidate_t), compare_candidates_asc);
    int out_n = (found_n < ef) ? found_n : ef;
    memcpy(out, found, out_n * sizeof(candidate_t));

    free(visited);
    free(cands);
    free(found);
    return out_n;
}

/* ------------------------------------------------------------------ *
 * random_level                                                         *
 *   Distribuição exponencial conforme o paper original de HNSW.        *
 *   mL = 1/ln(M) → P(level >= l) decresce geometricamente.            *
 * ------------------------------------------------------------------ */

int random_level(void)
{
    // mL = 1/ln(M) is the normalization factor from the paper
    // floor(-ln(uniform(0,1)) * mL) gives the correct geometric distribution
    double mL = 1.0 / log((double)M);
    double r = (double)(rand() + 1) / ((double)RAND_MAX + 2); // avoid log(0)
    int level = (int)(-log(r) * mL);
    return level < MAX_LEVEL - 1 ? level : MAX_LEVEL - 1;
}

void insert(hnsw_header_t *h, int idx)
{
    hnsw_node_t *n = &h->nodes[idx];
    int node_level = n->level;

    /* Caso 1: grafo vazio */
    if (h->entry_point == -1)
    {
        h->entry_point = idx;
        h->max_level = node_level;
        return;
    }

    int current = h->entry_point;

    candidate_t best1[1];
    for (int l = h->max_level; l > node_level; l--)
    {
        if (search_layer(h, n->vector, current, l, best1, 1, h->size) > 0)
            current = best1[0].idx;
    }

    candidate_t candidates[EF_CONSTRUCTION];

    for (int l = node_level; l >= 0; l--)
    {
        int cnt = search_layer(h, n->vector, current, l,
                               candidates, EF_CONSTRUCTION, h->size);
        if (cnt == 0)
            continue;

        current = candidates[0].idx;

        int connect = (cnt < M) ? cnt : M;
        for (int i = 0; i < connect; i++)
        {
            int nb_idx = candidates[i].idx;
            hnsw_node_t *nb = &h->nodes[nb_idx];

            if (n->neighbor_count[l] < M)
                n->neighbors[l][n->neighbor_count[l]++] = nb_idx;

            if (nb->neighbor_count[l] < M)
            {
                nb->neighbors[l][nb->neighbor_count[l]++] = idx;
            }
            else
            {
                int worst_pos = 0;
                float worst_dist = 0;
                for (int j = 0; j < nb->neighbor_count[l]; j++)
                {
                    float d = dist2(nb->vector,
                                    h->nodes[nb->neighbors[l][j]].vector);
                    if (d > worst_dist)
                    {
                        worst_dist = d;
                        worst_pos = j;
                    }
                }
                if (dist2(nb->vector, n->vector) < worst_dist)
                    nb->neighbors[l][worst_pos] = idx;
            }
        }
    }

    if (node_level > h->max_level)
    {
        h->entry_point = idx;
        h->max_level = node_level;
    }
}

int search(hnsw_header_t *h, float *q)
{
    if (h->entry_point == -1)
        return -1;

    int current = h->entry_point;

    candidate_t best1[1];
    for (int l = h->max_level; l > 0; l--)
    {
        if (search_layer(h, q, current, l, best1, 1, h->size) > 0)
            current = best1[0].idx;
    }

    candidate_t results[EF_SEARCH];
    int cnt = search_layer(h, q, current, 0, results, EF_SEARCH, h->size);

    return (cnt > 0) ? results[0].idx : current;
}

void init(hnsw_header_t *h, hnsw_node_t *nodes, int size)
{
    h->nodes = nodes;
    h->size = size;
    h->entry_point = -1;
    h->max_level = 0;
}

int build_hnsw(const char *data_file, const char *index_file)
{
    srand(1001);
    int fd = open(data_file, O_RDONLY);
    if (fd < 0)
    {
        perror("open");
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0)
    {
        perror("fstat");
        close(fd);
        return -1;
    }

    item_t *items = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (items == MAP_FAILED)
    {
        perror("mmap");
        close(fd);
        return -1;
    }

    int size = st.st_size / sizeof(item_t);

    hnsw_node_t *nodes = calloc(size, sizeof(hnsw_node_t));
    if (!nodes)
    {
        fprintf(stderr, "Memory allocation failed\n");
        munmap(items, st.st_size);
        close(fd);
        return -1;
    }

    for (int i = 0; i < size; i++)
    {
        memcpy(nodes[i].vector, items[i].vector, DIM * sizeof(float));
        nodes[i].label = items[i].label;
        nodes[i].level = random_level();
        for (int l = 0; l < MAX_LEVEL; l++)
            nodes[i].neighbor_count[l] = 0;
    }

    hnsw_header_t h;
    init(&h, nodes, size);

    for (int i = 0; i < size; i++)
    {
        fprintf(stderr, "Inserting item %d/%d (level %d)\n", i + 1, size, nodes[i].level);
        insert(&h, i);
    }
    int hist[MAX_LEVEL] = {0};
    for (int i = 0; i < size; i++)
        hist[nodes[i].level]++;
    for (int l = 0; l < MAX_LEVEL; l++)
        if (hist[l] > 0)
            fprintf(stderr, "Level %d: %d nodes (%.3f%%)\n",
                    l, hist[l], 100.0 * hist[l] / size);

    hnsw_disk_header_t disk = {h.size, h.entry_point, h.max_level};

    FILE *f = fopen(index_file, "wb");
    if (!f)
    {
        fprintf(stderr, "File opening failed\n");
        munmap(items, st.st_size);
        close(fd);
        free(nodes);
        return -1;
    }

    fwrite(&disk, sizeof(disk), 1, f);
    fwrite(nodes, sizeof(hnsw_node_t), h.size, f);
    fclose(f);

    munmap(items, st.st_size);
    close(fd);
    free(nodes);
    return 0;
}

int main(void)
{
    return build_hnsw("preprocessed_data.bin", "hnsw_index.bin");
}