// t_ncp.c - TCP sender baseline for Project 1
// Usage: ./t_ncp <src_file> <dest_file>@<ip>:<port>

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define BUF_SIZE (1<<16) // 64KB

static void die(const char* msg) { perror(msg); exit(1); }

static inline uint64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000ULL + ts.tv_nsec/1000000ULL;
}

static inline uint64_t htonll(uint64_t x){
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (((uint64_t)htonl(x & 0xFFFFFFFFULL)) << 32) | htonl((uint32_t)(x >> 32));
#else
    return x;
#endif
}

// writen: send all bytes
static ssize_t writen(int fd, const void* buf, size_t n) {
    size_t left = n; const char* p = (const char*)buf;
    while (left > 0) {
        ssize_t w = send(fd, p, left, 0);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        left -= (size_t)w; p += w;
    }
    return (ssize_t)n;
}

// parse "<dest>@<ip>:<port>"
static int parse_dst(const char* s, char* out_name, size_t on,
                     char* out_host, size_t oh, char* out_port, size_t op)
{
    const char* at = strchr(s, '@');
    const char* colon = at ? strrchr(at+1, ':') : NULL;
    if (!at || !colon) return -1;
    size_t nlen = (size_t)(at - s);
    size_t hlen = (size_t)(colon - (at + 1));
    size_t plen = strlen(colon + 1);
    if (nlen == 0 || nlen >= on || hlen == 0 || hlen >= oh || plen == 0 || plen >= op) return -1;

    memcpy(out_name, s, nlen); out_name[nlen] = '\0';
    memcpy(out_host, at + 1, hlen); out_host[hlen] = '\0';
    memcpy(out_port, colon + 1, plen); out_port[plen] = '\0';
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <src_file> <dest_file>@<ip>:<port>\n", argv[0]);
        return 2;
    }
    const char* src_path = argv[1];
    char dst_name[1024], host[256], port[64];
    if (parse_dst(argv[2], dst_name, sizeof(dst_name), host, sizeof(host), port, sizeof(port)) != 0) {
        fprintf(stderr, "Bad destination format. Expect: <dest>@<ip>:<port>\n");
        return 2;
    }

    struct stat st;
    if (stat(src_path, &st) != 0) die("stat src");
    uint64_t file_size = (uint64_t)st.st_size;

    printf("Successfully initialized with:\n");
    printf("\tSource filename = %s\n\tDestination filename = %s\n", src_path, dst_name);
    printf("\tHostname = %s\n\tPort = %s\n", host, port);
    fflush(stdout);

    // connect
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai) != 0) die("getaddrinfo");
    int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s < 0) die("socket");
    // 可选：关闭 Nagle 将更“及时”，但对吞吐影响不大
    // int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (connect(s, ai->ai_addr, ai->ai_addrlen) < 0) die("connect");
    freeaddrinfo(ai);

    // header: [name_len u16 net][file_size u64 net][name bytes]
    uint16_t name_len = (uint16_t)strlen(dst_name);
    uint16_t name_len_net = htons(name_len);
    uint64_t fsz_net = htonll(file_size);
    if (writen(s, &name_len_net, sizeof(name_len_net)) < 0) die("send name_len");
    if (writen(s, &fsz_net, sizeof(fsz_net)) < 0) die("send file_size");
    if (writen(s, dst_name, name_len) < 0) die("send dest_name");

    // send body
    FILE* fp = fopen(src_path, "rb");
    if (!fp) die("fopen src");
    char* buf = (char*)malloc(BUF_SIZE);
    if (!buf) die("malloc");

    uint64_t start_ms = now_ms();
    uint64_t bytes_sent = 0;

    for (;;) {
        size_t n = fread(buf, 1, BUF_SIZE, fp);
        if (n == 0) { if (feof(fp)) break; die("fread"); }
        if (writen(s, buf, n) < 0) die("send body");
        bytes_sent += (uint64_t)n;
    }
    free(buf);
    uint64_t end_ms = now_ms();

    fclose(fp);
    close(s);

    // final stats (TCP baseline不需要“含重传”的总发送量)
    double elapsed_s = (end_ms - start_ms) / 1000.0;
    double goodput_mbps = (bytes_sent * 8.0) / (elapsed_s * 1e6);
    printf("[TCP-SND] DONE: %.2f MB in %.2f s, avg goodput: %.2f Mb/s\n",
           bytes_sent / 1e6, elapsed_s, goodput_mbps);
    fflush(stdout);

    return 0;
}
