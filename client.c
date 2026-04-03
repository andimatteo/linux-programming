#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 4242
#define BUF_SIZE 4096
static int send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}
static int recv_all(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n == 0) return -1; // server closed
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}
int main(void) {
    int fd;
    struct sockaddr_in addr;
    char buf[BUF_SIZE];
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &addr.sin_addr) != 1) {
        fprintf(stderr, "inet_pton failed\n");
        close(fd);
        return 1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }
    printf("Connected to %s:%d\n", SERVER_IP, SERVER_PORT);
    printf("Type text and press ENTER (Ctrl+D to quit)\n");
    while (fgets(buf, sizeof(buf), stdin)) {
        size_t len = strlen(buf);
        if (len == 0) continue;
        if (send_all(fd, buf, len) < 0) {
            perror("send");
            break;
        }
        if (recv_all(fd, buf, len) < 0) {
            fprintf(stderr, "recv failed or server closed connection\n");
            break;
        }
        fwrite(buf, 1, len, stdout);
        fflush(stdout);
    }
    close(fd);
    return 0;
}
