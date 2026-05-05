#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 8080
#define BUFFER_SIZE 4096

void handle_request(int client_socket)
{
    char buffer[BUFFER_SIZE];
    char method[16], path[256];
    int bytes_read = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_read < 0)
    {
        perror("recv");
        close(client_socket);
        return;
    }
    buffer[bytes_read] = '\0';
    printf("Received request:\n%s\n", buffer);
    printf("Request method: %s, path: %s\n", method, path);

     sscanf(buffer, "%s %s", method, path);
    if (strcmp(path, "/") == 0)
    {
        char *response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n\r\n"
            "Hello, World!";
        write(client_socket, response, strlen(response));
    }
    else if (strcmp(path, "/health") == 0)
    {
        char *response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n\r\n"
            "{\"status\": \"ok\"}";
        write(client_socket, response, strlen(response));
    }
    else
    {
        char *response =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain\r\n\r\n"
            "Not Found";
        write(client_socket, response, strlen(response));
    }

    close(client_socket);
}

int main()
{
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, 5) < 0)
    {
        perror("listen");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    printf("Server is listening on port %d...\n", PORT);

    while (1)
    {
        int client_socket = accept(server_socket, NULL, NULL);
        if (client_socket < 0)
        {
            perror("accept");
            continue;
        }
        handle_request(client_socket);
    }

    close(server_socket);
    return 0;
}