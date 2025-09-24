// t_rcv.c - TCP receiver baseline for Project 1
// Usage: ./t_rcv <port>

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define TEN_MB 10000000ULL
#define BUF_SIZE (1<<16) // 64KB

static void die(const char* msg) { perror(msg); exit(1); }

static inline uint64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000ULL + ts.tv_nsec/1000000ULL;
}

// 64-bit hton/ntoh helpers
static inline uint64_t htonll(uint64_t x){
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (((uint64_t)htonl(x & 0xFFFFFFFFULL)) << 32) | htonl((uint32_t)(x >> 32));
#else
    return x;
#endif
}
static inline uint64_t ntohll(uint64_t x){ return htonll(x); }

// read exactly n bytes
static ssize_t readn(int fd, void* buf, size_t n) {
    size_t left = n; char* p = (char*)buf;
    while (left > 0) {
        ssize_t r = recv(fd, p, left, 0);
        if (r == 0) return n - left;      // EOF
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        left -= (size_t)r; p += r;
    }
    return (ssize_t)n;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 2;
    }
    const char* port = argv[1];

    // listen
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(NULL, port, &hints, &ai) != 0) die("getaddrinfo");
    int ls = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (ls < 0) die("socket");
    int yes = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (bind(ls, ai->ai_addr, ai->ai_addrlen) < 0) die("bind");
    if (listen(ls, 1) < 0) die("listen");

    printf("Successfully initialized with:\n\tPort = %s\n", port);
    fflush(stdout);

    struct sockaddr_storage peer; socklen_t plen = sizeof(peer);
    int s = accept(ls, (struct sockaddr*)&peer, &plen);
    if (s < 0) die("accept");
    freeaddrinfo(ai); close(ls);

    // ---- read header ----
    // [name_len: u16 net-order][file_size: u64 net-order][dest_name: name_len bytes]
    uint16_t name_len_net;
    uint64_t fsz_net;
    if (readn(s, &name_len_net, sizeof(name_len_net)) != sizeof(name_len_net)) die("read name_len");
    if (readn(s, &fsz_net, sizeof(fsz_net)) != sizeof(fsz_net)) die("read file_size");
    uint16_t name_len = ntohs(name_len_net);
    uint64_t file_size = ntohll(fsz_net);
    if (name_len == 0 || name_len > 1024) { fprintf(stderr, "bad name_len\n"); exit(1); }

    char dst_name[1100] = {0};
    if (readn(s, dst_name, name_len) != name_len) die("read dest_name");
    dst_name[name_len] = '\0';

    FILE* fp = fopen(dst_name, "wb");
    if (!fp) die("fopen dst");

    uint64_t start_ms = now_ms();
    uint64_t last_mark_ms = start_ms;
    uint64_t last_mark_bytes = 0;
    uint64_t bytes_in_order = 0;

    // ---- receive body ----
    char* buf = (char*)malloc(BUF_SIZE);
    if (!buf) die("malloc");
    for (;;) {
        ssize_t r = recv(s, buf, BUF_SIZE, 0);
        if (r < 0) { if (errno == EINTR) continue; die("recv"); }
        if (r == 0) break; // EOF
        size_t w = fwrite(buf, 1, (size_t)r, fp);
        if (w != (size_t)r) die("fwrite");
        bytes_in_order += (uint64_t)r;

        // 10 MB progress (decimal)
        uint64_t delta_bytes = bytes_in_order - last_mark_bytes;
        if (delta_bytes >= TEN_MB) {
            uint64_t now = now_ms();
            uint64_t delta_ms = (now - last_mark_ms) ? (now - last_mark_ms) : 1;
            double recent_mbps = (delta_bytes * 8.0) / (double)delta_ms / 1000.0;
            printf("[TCP-RCV] Progress: %.2f MB total, recent 10MB avg rate: %.2f Mb/s\n",
                   bytes_in_order / 1e6, recent_mbps);
            fflush(stdout);
            last_mark_ms = now;
            last_mark_bytes += TEN_MB; // step one bucket
        }
    }
    free(buf);

    // done stats
    uint64_t end_ms = now_ms();
    double elapsed_s = (end_ms - start_ms) / 1000.0;
    double avg_mbps = (bytes_in_order * 8.0) / (elapsed_s * 1e6);

    printf("[TCP-RCV] DONE: %.2f MB in %.2f s, avg goodput: %.2f Mb/s\n",
           bytes_in_order / 1e6, elapsed_s, avg_mbps);
    fflush(stdout);

    fclose(fp);
    close(s);
    return 0;
}
