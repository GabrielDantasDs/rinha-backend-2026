#ifndef HNSW_SEARCH_H
#define HNSW_SEARCH_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * HNSW graph parameters
 * ============================================================ */

#define MAX_LEVEL 6       /* max number of hierarchical layers       */
#define M         16     /* max neighbors per node at layers 1..max */
#define M0        32      /* max neighbors per node at layer 0       */
#define EF_SEARCH 80     /* query-time beam width                   */

/* ============================================================
 * Quantization
 *
 * Spec range for vector components is [-1, 1]. We map linearly to
 * uint8 [0, 255]:
 *   q = round((v + 1.0) * qscale)         qscale = 127.5
 * Inverse (for diagnostics): v = q/qscale - 1.0
 *
 * Distance computation works on the quantized values directly:
 *   dist2 = sum((qa[i] - qb[i])^2)        uint32 accumulator
 * Since both sides are shifted by the same constant, the squared
 * difference is preserved up to a global scale factor — top-K
 * ordering is identical to the float ground truth.
 * ============================================================ */

#define QSCALE_DEFAULT 127.5f

/* ============================================================
 * Index file sanity
 *
 * If the file is corrupted, truncated, or built with a different
 * struct layout, magic+version mismatch causes startup to panic
 * instead of reading garbage (which is how the label=186 bug
 * happened before).
 * ============================================================ */

#define INDEX_MAGIC   0x48534E56u   /* "VNsH" little-endian */
#define INDEX_VERSION 1u

/* ============================================================
 * Compact on-disk node (28 bytes)
 *
 * One per reference vector. Stored contiguously right after the
 * disk_header_t at the start of hnsw_index.bin.
 *
 * Neighbor IDs are NOT stored inline — they live in separate blobs:
 *   level 0       : l0_blob[node.l0_offset .. + node.l0_count)
 *   level >= 1    : packed record in high_blob starting at
 *                   byte offset node.high_offset
 * ============================================================ */

typedef struct {
    uint8_t  qvec[14];      /*  14 B — quantized vector                  */
    uint8_t  label;         /*   1 B — 0=legit, 1=fraud                  */
    uint8_t  level;         /*   1 B — top level this node lives at      */
    uint8_t  l0_count;      /*   1 B — actual # neighbors at layer 0     */
    uint8_t  _pad[3];       /*   3 B — keep l0_offset 4-byte aligned     */
    uint32_t l0_offset;     /*   4 B — index into l0_blob[] (uint32 idx) */
    uint32_t high_offset;   /*   4 B — byte offset into high_blob,
                                       UINT32_MAX iff level == 0          */
} node_t;                   /*  28 B total                                */

/* ============================================================
 * Disk header (32 bytes) — first thing in hnsw_index.bin
 * ============================================================ */

typedef struct {
    uint32_t magic;             /*  must equal INDEX_MAGIC                */
    uint32_t version;           /*  must equal INDEX_VERSION              */
    uint32_t size;              /*  number of nodes (e.g. 3,000,000)      */
    uint32_t entry_point;       /*  node id where searches begin          */
    uint8_t  max_level;         /*  highest layer any node reaches        */
    uint8_t  m;                 /*  must equal the M macro at runtime     */
    uint8_t  m0;                /*  must equal the M0 macro at runtime    */
    uint8_t  _pad;
    float    qscale;            /*  quantization scale used at build      */
    uint32_t l0_blob_count;     /*  total uint32 entries in level-0 blob  */
    uint32_t high_blob_size;    /*  bytes in the high-level blob          */
} disk_header_t;                /*  32 B total                            */

/* File layout (little-endian):
 *
 *   [ disk_header_t                       (32 B) ]
 *   [ node_t × size                       (28 B each) ]
 *   [ l0_blob: uint32_t × l0_blob_count   (flat, indexed by node.l0_offset) ]
 *   [ high_blob: bytes × high_blob_size   (variable-length per-node records) ]
 *
 * A node N with top level = L > 0 has a record in high_blob at byte
 * offset N.high_offset:
 *
 *   uint8_t  count[L];                 // one per level 1..L
 *   // (implicit alignment padding so the next field is 4-aligned)
 *   uint32_t neighbors[];              // flat: count[0] for layer 1,
 *                                      //       count[1] for layer 2, ...
 *
 * Nodes with level == 0 have high_offset == UINT32_MAX and consume zero
 * bytes in high_blob. Since ~94% of nodes are level-0 only (M=16), this
 * saves the bulk of what the old fixed [MAX_LEVEL][M] matrix wasted.
 */

/* ============================================================
 * Runtime header — pointers into the mmap, never allocated
 * ============================================================ */

typedef struct {
    node_t   *nodes;        /* size entries, contiguous                  */
    uint32_t *l0_blob;      /* level-0 neighbor IDs, flat                */
    uint8_t  *high_blob;    /* level 1+ neighbor records, variable-len   */
    uint32_t  size;
    uint32_t  entry_point;
    uint8_t   max_level;
    float     qscale;
} hnsw_header_t;

/* ============================================================
 * API
 * ============================================================ */

/* Allocate the per-search "visited" buffer once at startup.
 * Pass the size from disk_header_t.size. Returns 0 on success. */
int hnsw_search_init(int size);

/* Quantize a 14-D float vector into uint8 using the given scale.
 * Clamps each output to [0, 255]. */
void hnsw_quantize(const float *v, uint8_t *out, float qscale);

/* Top-5 nearest neighbors of q in the index.
 *   q        : raw float query vector (quantized internally)
 *   idx_out  : 5 ints, filled with node IDs (-1 if fewer than 5 found)
 *   dist_out : 5 floats, squared distances (cast from uint32 accumulator)
 * Returns 0 on success, -1 on error. */
int search(hnsw_header_t *h, float *q, int *idx_out, float *dist_out);

/* ============================================================
 * Compile-time sanity — fails to compile if padding assumptions
 * above are wrong. Catches struct-mismatch bugs at build time
 * instead of runtime (which is how label=186 happened before).
 * ============================================================ */

_Static_assert(sizeof(node_t)        == 28, "node_t must be 28 bytes");
_Static_assert(sizeof(disk_header_t) == 32, "disk_header_t must be 32 bytes");

#endif /* HNSW_SEARCH_H */
