#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>      /* strncasecmp */
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include "validation.h"
#include "normalization.h"
#include "hnsw_search.h"
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>

#define PORT 8080
#define BUFFER_SIZE 4096

static hnsw_header_t g_hnsw;
static hnsw_node_t *g_nodes;

struct connection
{
    int fd;
    char buffer[BUFFER_SIZE];
    size_t buffer_len;

    const char *write_ptr;
    size_t      write_pos;
    size_t      write_len;
};

void load_index(const char *filename, hnsw_header_t *h, hnsw_node_t **nodes)
{
    int fd = open(filename, O_RDONLY);
    if (fd < 0)
    {
        perror("open");
        exit(EXIT_FAILURE);
    }

    struct stat st;
    if (fstat(fd, &st) < 0)
    {
        perror("fstat");
        close(fd);
        exit(EXIT_FAILURE);
    }

    /* MAP_POPULATE: read every page into RAM during mmap, instead of
     * faulting them in lazily on first access. Kills cold-start spikes
     * in search() that were showing up as 1-5 ms outliers in p99. */
    void *mapped = mmap(
        NULL,
        st.st_size,
        PROT_READ,
        MAP_SHARED | MAP_POPULATE,
        fd,
        0);

    if (mapped == MAP_FAILED)
    {
        perror("mmap");
        close(fd);
        exit(EXIT_FAILURE);
    }

    close(fd);

    /* Tell the kernel: we'll need this whole region (drives readahead),
     * and our access pattern is random (HNSW jumps around the graph),
     * so don't waste cycles on sequential read-ahead heuristics. */
    if (madvise(mapped, st.st_size, MADV_WILLNEED) != 0)
    {
        perror("madvise WILLNEED");
        /* non-fatal — keep going */
    }
    if (madvise(mapped, st.st_size, MADV_RANDOM) != 0)
    {
        perror("madvise RANDOM");
        /* non-fatal — keep going */
    }

    /* Belt-and-suspenders: if MAP_POPULATE was honored partially or
     * the kernel skipped some pages, this loop forces every page into
     * the resident set. Reads one byte per page so it's effectively
     * memcpy-bound (~0.5 GB/s), takes ~300 ms for a 168 MB index — only
     * paid once, at startup. */
    {
        volatile unsigned char sink = 0;
        const unsigned char *p = (const unsigned char *)mapped;
        size_t pagesize = (size_t)sysconf(_SC_PAGESIZE);
        for (size_t off = 0; off < (size_t)st.st_size; off += pagesize)
        {
            sink ^= p[off];
        }
        (void)sink;
    }

    hnsw_disk_header_t *disk = mapped;

    h->size = disk->size;
    h->entry_point = disk->entry_point;
    h->max_level = disk->max_level;

    *nodes = (hnsw_node_t *)((char *)mapped + sizeof(hnsw_disk_header_t));
}

void server_init(void)
{
    /* Load constants + MCC risk table into memory once.
     * Hot path (lookup_mcc_risk) becomes a few pointer compares instead
     * of fopen/fread/fclose per request. */
    if (normalization_init("./dataset/normalization.json",
                           "./dataset/mcc_risk.json") != 0)
    {
        fprintf(stderr,
                "warning: normalization_init partial - using built-in defaults\n");
    }

    load_index("hnsw_index.bin", &g_hnsw, &g_nodes);
    g_hnsw.nodes = g_nodes;

    /* Allocate the per-search "visited" buffer once. Replaces a
     * calloc(3M) that used to happen on every request. */
    if (hnsw_search_init(g_hnsw.size) != 0)
    {
        fprintf(stderr, "hnsw_search_init: failed to allocate visited buffer\n");
        exit(EXIT_FAILURE);
    }
}

/* ============================================================
 * HTTP read-side state machine
 * ============================================================ */

/* Forward declaration: the per-request handler that picks routes,
 * runs validation/search, and queues a response into c->write_*.
 * Defined in the next step. */
static int handle_one_request(struct connection *c,
                              const char *headers, size_t header_len,
                              const char *body,    size_t body_len);

/* Locate the end of the HTTP header block.
 * Returns a pointer to the first byte after "\r\n\r\n", or NULL if
 * the buffer doesn't yet contain a complete header section. */
static const char *find_header_end(const char *buf, size_t len)
{
    if (len < 4) return NULL;
    for (size_t i = 0; i + 3 < len; i++)
    {
        if (buf[i]   == '\r' && buf[i + 1] == '\n'
         && buf[i + 2] == '\r' && buf[i + 3] == '\n')
        {
            return buf + i + 4;
        }
    }
    return NULL;
}

/* Walk the header block, find a "Content-Length:" line (case-insensitive),
 * and return the integer value. Returns 0 if the header is absent — that's
 * the right default for GET requests with no body. */
static size_t parse_content_length(const char *headers, size_t header_len)
{
    const char *end = headers + header_len;

    /* Skip the request line (first line). */
    const char *line = memchr(headers, '\n', header_len);
    if (line == NULL) return 0;
    line++;

    static const char key[] = "content-length:";
    const size_t klen = sizeof(key) - 1;

    while (line < end)
    {
        const char *next = memchr(line, '\n', (size_t)(end - line));
        if (next == NULL) next = end;

        size_t line_len = (size_t)(next - line);
        /* trim trailing \r */
        while (line_len > 0 && (line[line_len - 1] == '\r' ||
                                line[line_len - 1] == '\n'))
        {
            line_len--;
        }

        if (line_len >= klen && strncasecmp(line, key, klen) == 0)
        {
            const char *p = line + klen;
            const char *line_end = line + line_len;
            while (p < line_end && (*p == ' ' || *p == '\t')) p++;

            size_t value = 0;
            while (p < line_end && *p >= '0' && *p <= '9')
            {
                value = value * 10 + (size_t)(*p - '0');
                p++;
            }
            return value;
        }

        line = next + 1;
    }
    return 0;
}

/* Pull bytes off the socket into c->buffer, appending after whatever's
 * already there. Returns 1 to keep the connection alive, 0 to close. */
static int drain_socket(struct connection *c)
{
    for (;;)
    {
        if (c->buffer_len == BUFFER_SIZE)
        {
            /* Buffer full — let the parser try to consume what's there.
             * If even that's not a complete request, the request is too
             * big and process_buffered_requests will return 0. */
            return 1;
        }

        ssize_t n = recv(c->fd,
                         c->buffer + c->buffer_len,
                         BUFFER_SIZE - c->buffer_len,
                         0);

        if (n > 0)
        {
            c->buffer_len += (size_t)n;
            continue;
        }
        if (n == 0)
        {
            return 0;                /* peer closed cleanly */
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return 1;                /* no more data right now — normal */
        }
        if (errno == EINTR)
        {
            continue;                /* signal — retry */
        }
        return 0;                    /* real error */
    }
}

/* Consume as many complete requests from c->buffer as we can. After
 * each, slide leftover bytes (a pipelined next request) down to
 * position 0. Returns 1 on success, 0 if the request is malformed or
 * too big. */
static int process_buffered_requests(struct connection *c)
{
    for (;;)
    {
        const char *body = find_header_end(c->buffer, c->buffer_len);
        if (body == NULL)
        {
            return 1;                /* headers still arriving */
        }

        size_t header_len = (size_t)(body - c->buffer);
        size_t body_len   = parse_content_length(c->buffer, header_len);

        if (header_len + body_len >= BUFFER_SIZE)
        {
            return 0;                /* doesn't fit — bail */
        }
        if (c->buffer_len < header_len + body_len)
        {
            return 1;                /* body still arriving */
        }

        if (!handle_one_request(c, c->buffer, header_len, body, body_len))
        {
            return 0;
        }

        /* Slide any leftover bytes (pipelined next request) to the front. */
        size_t consumed = header_len + body_len;
        size_t leftover = c->buffer_len - consumed;
        if (leftover > 0)
        {
            memmove(c->buffer, c->buffer + consumed, leftover);
        }
        c->buffer_len = leftover;

        /* If the previous request queued a response that didn't drain in
         * one shot, stop — we can't process the next pipelined request
         * until the write side empties. The dispatcher will resume us
         * when EPOLLOUT fires. */
        if (c->write_pos < c->write_len)
        {
            return 1;
        }
    }
}

/* Top-level read handler — called by the dispatcher when EPOLLIN fires.
 * Pulls bytes off the wire, then chews through any complete requests
 * that have accumulated. */
static int handle_read(struct connection *c)
{
    if (!drain_socket(c)) return 0;
    return process_buffered_requests(c);
}

/* ============================================================
 * Pre-rendered HTTP responses
 *
 * Every response this server sends is one of a small fixed set,
 * so we precompute them as static bytes with Content-Length already
 * baked in. The hot path then sends a response by picking a pointer
 * out of a 6-element table — no formatting, no strlen, no malloc.
 * ============================================================ */

static const char RESPONSE_READY[] =
    "HTTP/1.1 204 No Content\r\n"
    "Connection: keep-alive\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

static const char RESPONSE_400[] =
    "HTTP/1.1 400 Bad Request\r\n"
    "Connection: keep-alive\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

static const char RESPONSE_404[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Connection: keep-alive\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

/* /fraud-score outcomes — bodies are 35 chars (true) or 36 chars (false). */
static const char RESPONSE_SCORE_0[] =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 35\r\nConnection: keep-alive\r\n\r\n"
    "{\"approved\":true,\"fraud_score\":0.0}";
static const char RESPONSE_SCORE_1[] =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 35\r\nConnection: keep-alive\r\n\r\n"
    "{\"approved\":true,\"fraud_score\":0.2}";
static const char RESPONSE_SCORE_2[] =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 35\r\nConnection: keep-alive\r\n\r\n"
    "{\"approved\":true,\"fraud_score\":0.4}";
static const char RESPONSE_SCORE_3[] =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\nConnection: keep-alive\r\n\r\n"
    "{\"approved\":false,\"fraud_score\":0.6}";
static const char RESPONSE_SCORE_4[] =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\nConnection: keep-alive\r\n\r\n"
    "{\"approved\":false,\"fraud_score\":0.8}";
static const char RESPONSE_SCORE_5[] =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\nConnection: keep-alive\r\n\r\n"
    "{\"approved\":false,\"fraud_score\":1.0}";

static const char *const SCORE_RESPONSES[6] = {
    RESPONSE_SCORE_0, RESPONSE_SCORE_1, RESPONSE_SCORE_2,
    RESPONSE_SCORE_3, RESPONSE_SCORE_4, RESPONSE_SCORE_5,
};
static const size_t SCORE_RESPONSE_LENS[6] = {
    sizeof(RESPONSE_SCORE_0) - 1,
    sizeof(RESPONSE_SCORE_1) - 1,
    sizeof(RESPONSE_SCORE_2) - 1,
    sizeof(RESPONSE_SCORE_3) - 1,
    sizeof(RESPONSE_SCORE_4) - 1,
    sizeof(RESPONSE_SCORE_5) - 1,
};

/* Stash a response on the connection. Doesn't actually send anything;
 * handle_write does that. Splitting "decide" from "send" is what makes
 * partial writes resumable. */
static void queue_response(struct connection *c,
                           const char *resp, size_t len)
{
    c->write_ptr = resp;
    c->write_pos = 0;
    c->write_len = len;
}

/* ============================================================
 * Per-request handler
 * ============================================================ */

static int handle_one_request(struct connection *c,
                              const char *headers, size_t header_len,
                              const char *body, size_t body_len)
{
    (void)body;  /* we use c->buffer + header_len for non-const access */

    /* GET /ready — health check, no body. */
    if (header_len >= 11 && memcmp(headers, "GET /ready ", 11) == 0)
    {
        queue_response(c, RESPONSE_READY, sizeof(RESPONSE_READY) - 1);
        return 1;
    }

    /* Anything that isn't POST /fraud-score → 404. */
    if (header_len < 18 || memcmp(headers, "POST /fraud-score ", 18) != 0)
    {
        queue_response(c, RESPONSE_404, sizeof(RESPONSE_404) - 1);
        return 1;
    }

    /* validate_request and create_vector_from_request use strstr/strchr,
     * which need a null-terminated string. The buffer is raw bytes —
     * we save the byte at position (header_len + body_len), overwrite
     * it with '\0', then restore it. The check in
     * process_buffered_requests guarantees that byte exists in-buffer. */
    char *body_str = c->buffer + header_len;
    char saved = c->buffer[header_len + body_len];
    c->buffer[header_len + body_len] = '\0';

    /* === TIMING INSTRUMENTATION (temporary) === */
    struct timespec t0, t1, t2, t3;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    if (!validate_request(body_str))
    {
        c->buffer[header_len + body_len] = saved;
        queue_response(c, RESPONSE_400, sizeof(RESPONSE_400) - 1);
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);

    float vector[14];
    create_vector_from_request(body_str, vector);
    c->buffer[header_len + body_len] = saved;

    clock_gettime(CLOCK_MONOTONIC, &t2);

    /* KNN search for the 5 nearest reference vectors. */
    int   idx_out[5];
    float dist_out[5];
    for (int i = 0; i < 5; i++)
    {
        idx_out[i]  = -1;
        dist_out[i] = 1e30f;
    }
    search(&g_hnsw, vector, idx_out, dist_out);

    clock_gettime(CLOCK_MONOTONIC, &t3);

    long us_validate = (t1.tv_sec - t0.tv_sec) * 1000000L
                     + (t1.tv_nsec - t0.tv_nsec) / 1000;
    long us_vector   = (t2.tv_sec - t1.tv_sec) * 1000000L
                     + (t2.tv_nsec - t1.tv_nsec) / 1000;
    long us_search   = (t3.tv_sec - t2.tv_sec) * 1000000L
                     + (t3.tv_nsec - t2.tv_nsec) / 1000;
    fprintf(stderr, "TIMING validate=%ldus vector=%ldus search=%ldus\n",
            us_validate, us_vector, us_search);
    /* === END TIMING === */

    /* Count how many of the 5 neighbors are labeled fraud. */
    int fraud_count = 0;
    for (int i = 0; i < 5; i++)
    {
        if (idx_out[i] >= 0 && g_hnsw.nodes[idx_out[i]].label == 1)
        {
            fraud_count++;
        }
    }
    if (fraud_count < 0) fraud_count = 0;
    if (fraud_count > 5) fraud_count = 5;

    /* Pick one of the 6 pre-rendered JSON responses. */
    queue_response(c, SCORE_RESPONSES[fraud_count],
                      SCORE_RESPONSE_LENS[fraud_count]);
    return 1;
}

/* ============================================================
 * HTTP write-side state machine
 * ============================================================ */

/* Drain c->write_* onto the wire. Returns 1 if the connection should
 * stay open (everything sent OR partial — caller will wait for EPOLLOUT),
 * 0 if the connection should be closed. */
static int handle_write(struct connection *c)
{
    while (c->write_pos < c->write_len)
    {
        ssize_t n = write(c->fd,
                          c->write_ptr + c->write_pos,
                          c->write_len - c->write_pos);
        if (n > 0)
        {
            c->write_pos += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return 1;            /* kernel buffer full — resume on EPOLLOUT */
        }
        if (n < 0 && errno == EINTR)
        {
            continue;            /* signal — retry */
        }
        return 0;                /* EPIPE / ECONNRESET / other — close */
    }

    /* Fully drained. Clear write state so the dispatcher won't ask
     * for EPOLLOUT next time around. */
    c->write_ptr = NULL;
    c->write_pos = 0;
    c->write_len = 0;
    return 1;
}

/* ============================================================
 * Connection lifecycle helpers
 * ============================================================ */

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static struct connection *new_connection(int fd)
{
    struct connection *c = malloc(sizeof(struct connection));
    if (c == NULL) return NULL;
    c->fd         = fd;
    c->buffer_len = 0;
    c->write_ptr  = NULL;
    c->write_pos  = 0;
    c->write_len  = 0;
    return c;
}

static void close_connection(int epoll_fd, struct connection *c)
{
    if (c == NULL) return;
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    free(c);
}

/* Re-arm the connection's epoll interest set: always EPOLLIN, plus
 * EPOLLOUT iff there are unsent bytes pending. Without this, a
 * connection with nothing to write would get spammed with "writable"
 * wake-ups doing nothing. */
static int update_events(int epoll_fd, struct connection *c)
{
    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLRDHUP;
    if (c->write_pos < c->write_len) ev.events |= EPOLLOUT;
    ev.data.ptr = c;
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c->fd, &ev);
}

/* Drain pending clients off the listener's accept queue. The listener
 * is non-blocking, so EAGAIN is the normal exit condition. */
static int accept_loop(int server_socket, int epoll_fd)
{
    for (;;)
    {
        int fd = accept4(server_socket, NULL, NULL, SOCK_NONBLOCK);
        if (fd >= 0)
        {
            struct connection *c = new_connection(fd);
            if (c == NULL)
            {
                close(fd);
                continue;
            }

            struct epoll_event ev;
            ev.events  = EPOLLIN | EPOLLRDHUP;
            ev.data.ptr = c;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0)
            {
                close(fd);
                free(c);
            }
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EINTR) continue;
        perror("accept4");
        return -1;
    }
}

/* ============================================================
 * Main event loop
 * ============================================================ */

int main(void)
{
    /* Without this, write() to a closed peer raises SIGPIPE which kills
     * the process. We want it to just return EPIPE so handle_write can
     * close the connection normally. */
    signal(SIGPIPE, SIG_IGN);

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) { perror("socket"); exit(EXIT_FAILURE); }

    int yes = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(PORT);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    /* Big backlog: under load, the kernel queues pending connections
     * until accept_loop drains them. 4096 is a comfortable margin. */
    if (listen(server_socket, 4096) < 0)
    {
        perror("listen");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    if (set_nonblocking(server_socket) < 0)
    {
        perror("fcntl O_NONBLOCK");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    server_init();

    printf("Server is listening on port %d...\n", PORT);

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        perror("epoll_create1");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    /* Tag the listener with NULL — the dispatcher uses that to recognize
     * "this is the listener" without an extra fd-comparison. */
    struct epoll_event listen_ev;
    listen_ev.events  = EPOLLIN;
    listen_ev.data.ptr = NULL;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &listen_ev) < 0)
    {
        perror("epoll_ctl listen");
        close(epoll_fd);
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    struct epoll_event events[256];

    for (;;)
    {
        int nfds = epoll_wait(epoll_fd, events, 256, -1);
        if (nfds < 0)
        {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            /* Listener event → accept all pending clients. */
            if (events[i].data.ptr == NULL)
            {
                accept_loop(server_socket, epoll_fd);
                continue;
            }

            struct connection *c = events[i].data.ptr;
            int keep = 1;

            if (events[i].events & (EPOLLERR | EPOLLHUP))
                keep = 0;

            if (keep && (events[i].events & (EPOLLIN | EPOLLRDHUP)))
                keep = handle_read(c);

            if (keep && (events[i].events & EPOLLOUT))
                keep = handle_write(c);

            /* If handle_read queued a response, try to send it now to
             * skip an extra epoll wake-up round-trip. */
            if (keep && c->write_pos < c->write_len)
                keep = handle_write(c);

            if (keep)
            {
                if (update_events(epoll_fd, c) != 0)
                    keep = 0;
            }

            if (!keep)
                close_connection(epoll_fd, c);
        }
    }

    close(server_socket);
    close(epoll_fd);
    return 0;
}
