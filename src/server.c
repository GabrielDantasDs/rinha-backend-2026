#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PORT 8080
#define BACKLOG 1024
#define BUFSIZE 4096

static void *handle(void *arg) {
    int fd = *(int *)arg;
    free(arg);

    char buf[BUFSIZE];
    int n = recv(fd, buf, BUFSIZE - 1, 0);
    if (n <= 0) { close(fd); return NULL; }
    buf[n] = '\0';

    char method[8], path[256];
    sscanf(buf, "%7s %255s", method, path);

    const char *body    = "{\"status\":\"ok\"}";
    char resp[256];
    int  blen = strlen(body);
    int  rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s", blen, body);

    send(fd, resp, rlen, 0);
    close(fd);
    return NULL;
}

int main(void) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(PORT),
    };
    bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv, BACKLOG);

    while (1) {
        int *cfd = malloc(sizeof(int));
        *cfd = accept(srv, NULL, NULL);
        pthread_t t;
        pthread_create(&t, NULL, handle, cfd);
        pthread_detach(t);
    }
}