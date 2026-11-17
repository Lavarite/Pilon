#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct address {
    int family;
    unsigned char bytes[16];
    uint32_t scope;
};

int main(int argc, char **argv)
{
    if (argc != 2 || !argv[1][0] || strlen(argv[1]) > 253)
        return 64;
    for (const unsigned char *p = (const unsigned char *)argv[1]; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
              || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
            return 64;

    // Bionic delegates to Android's per-network resolver, including VPN and Private DNS policy.
    alarm(15);
    struct addrinfo hints = {0}, *result = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(argv[1], NULL, &hints, &result) != 0)
        return 69;

    struct address addresses[64] = {0};
    size_t count = 0;
    int status = 0;
    for (const struct addrinfo *p = result; p; p = p->ai_next) {
        struct address address = {0};
        address.family = p->ai_family;
        size_t length;
        if (p->ai_family == AF_INET && p->ai_addrlen >= sizeof(struct sockaddr_in)) {
            const struct sockaddr_in *v4 = (const struct sockaddr_in *)p->ai_addr;
            length = 4;
            memcpy(address.bytes, &v4->sin_addr, length);
        } else if (p->ai_family == AF_INET6 && p->ai_addrlen >= sizeof(struct sockaddr_in6)) {
            const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)p->ai_addr;
            length = 16;
            memcpy(address.bytes, &v6->sin6_addr, length);
            address.scope = v6->sin6_scope_id;
        } else {
            continue;
        }
        size_t index = 0;
        while (index < count && !(addresses[index].family == address.family
               && addresses[index].scope == address.scope
               && memcmp(addresses[index].bytes, address.bytes, length) == 0))
            ++index;
        if (index < count)
            continue;
        if (count == 64) { status = 70; break; }
        addresses[count++] = address;
    }
    freeaddrinfo(result);
    if (status || !count)
        return status ? status : 69;

    char output[8192] = "PILON_DNS_V1\n";
    size_t used = strlen(output);
    for (size_t index = 0; index < count; ++index) {
        char text[INET6_ADDRSTRLEN];
        const struct address *address = &addresses[index];
        if (!inet_ntop(address->family, address->bytes, text, sizeof(text)))
            return 70;
        int written = address->family == AF_INET
            ? snprintf(output + used, sizeof(output) - used, "4 %s\n", text)
            : snprintf(output + used, sizeof(output) - used, "6 %s %u\n", text, address->scope);
        if (written < 0 || (size_t)written >= sizeof(output) - used)
            return 70;
        used += (size_t)written;
    }
    return fwrite(output, 1, used, stdout) == used && fflush(stdout) == 0 ? 0 : 70;
}
