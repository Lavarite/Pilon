#define _GNU_SOURCE
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

enum { maximum_request = 65536 };

static int transfer(int fd, unsigned char *data, size_t size, int writing)
{
    while (size) {
        const ssize_t count = writing ? send(fd, data, size, MSG_NOSIGNAL) : recv(fd, data, size, 0);
        if (count <= 0) return 0;
        data += count;
        size -= count;
    }
    return 1;
}

static void wipe(unsigned char *data, size_t size)
{
    volatile unsigned char *bytes = data;
    while (size--) *bytes++ = 0;
}

int main(int argc, char **argv)
{
    unsigned char request[maximum_request + 16], response[maximum_request];
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    const char *path = getenv("PILON_GIT_CREDENTIAL_SOCKET");
    int result = 1, fd = -1;
    if (argc != 2 || (strcmp(argv[1], "get") && strcmp(argv[1], "store") && strcmp(argv[1], "erase")))
        goto done;
    if (!path || *path != '/' || strlen(path) >= sizeof(address.sun_path)) goto done;
    const size_t prefix = strlen(argv[1]) + 1;
    memcpy(request, argv[1], prefix - 1);
    request[prefix - 1] = '\n';
    const size_t size = fread(request + prefix, 1, maximum_request + 1, stdin);
    if (size > maximum_request || ferror(stdin)) goto done;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) goto done;
    const struct timeval timeout = {.tv_sec = 5};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout))
        || setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) goto done;
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address))) goto done;
    struct ucred peer;
    socklen_t peer_size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) || peer.uid != getuid()) goto done;
    uint32_t length = htonl((uint32_t)(size + prefix));
    if (!transfer(fd, (unsigned char *)&length, sizeof(length), 1)
        || !transfer(fd, request, size + prefix, 1)) goto done;
    unsigned char ok;
    if (!transfer(fd, &ok, 1, 0) || !transfer(fd, (unsigned char *)&length, sizeof(length), 0)) goto done;
    length = ntohl(length);
    if (ok != 1 || length > maximum_request || !transfer(fd, response, length, 0)) goto done;
    if (length && fwrite(response, 1, length, stdout) != length) goto done;
    result = 0;
done:
    if (fd >= 0) close(fd);
    wipe(request, sizeof(request));
    wipe(response, sizeof(response));
    if (result) fputs("The private Git credential service could not complete this request.\n", stderr);
    return result;
}
