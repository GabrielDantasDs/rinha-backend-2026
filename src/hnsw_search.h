#ifndef HNSW_SEARCH_H
#define HNSW_SEARCH_H

#include <stdint.h>

#define MAX_LEVEL 6
#define M 12
#define EF_SEARCH 16

typedef struct node
{
    float vector[14];
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


int hnsw_search_init(int size);

int search(hnsw_header_t *h, float *q, int *idx_out, float *dist_out);

#endif
