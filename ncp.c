#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "sendto_dbg.h"
#include "net_include.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>     // ← 这里提供 struct addrinfo, getaddrinfo
#include <arpa/inet.h> // ← 提供 htons, inet_pton 等
#include <unistd.h>  
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>

static void Usage(int argc, char *argv[]);
static void Print_help(void);
static void die(const char* msg){ perror(msg); exit(1); }
/* Global configuration parameters (from command line) */
static int Loss_rate;
static int Mode;
static char *Port_Str;
static char *Src_filename;
static char *Dst_filename;
static char *Hostname;

// 窗口与超时参数（可按 LAN/WAN 调整）
enum { W_LAN = 512, W_WAN = 1500 };
static const uint32_t RTO_LAN_MS = 60;
static const uint32_t RTO_WAN_MS = 300;

typedef struct {
    uint32_t len;          // 该分片长度
    uint64_t last_tx_ms;   // 最近一次(重)发时间戳
    int      acked;        // 是否已被累积 ACK 覆盖
    long     file_off;     // 源文件偏移（便于重传时重新读取）
} seg_t;
// 分片元数据

// 函数原型（声明）
static void send_one_segment(int s,
                             const struct sockaddr *to, socklen_t tolen,
                             FILE* fp, uint32_t seq,
                             seg_t *segs,               // ← 把 segs 作为参数传入
                             uint64_t *sent_bytes_counter);

static void run_sender(const char* src,
                       const char* dst_name,
                       const char* ip,
                       const char* port_str);
                       static void send_one_segment(int s,
                             const struct sockaddr *to, socklen_t tolen,
                             FILE* fp, uint32_t seq,
                             seg_t *segs,
                             uint64_t *sent_bytes_counter)
{
    hdr_t h = {0};
    h.type = PKT_DATA;
    h.seq  = seq;
    h.len  = segs[seq].len;

    uint8_t frame[sizeof(hdr_t) + MAX_PAYLOAD];
    memcpy(frame, &h, sizeof(hdr_t));

    // 读该分片数据
    if (fseek(fp, segs[seq].file_off, SEEK_SET) != 0) die("fseek");
    size_t n = fread(frame + sizeof(hdr_t), 1, segs[seq].len, fp);
    if (n != segs[seq].len) die("fread");

    sendto_dbg(s, (const char*)frame, sizeof(hdr_t) + (int)segs[seq].len, 0, to, tolen);
    segs[seq].last_tx_ms = now_ms();
    if (sent_bytes_counter) *sent_bytes_counter += segs[seq].len;
}

int main(int argc, char *argv[]) {

    /* Initialize */
    Usage(argc, argv);
    sendto_dbg_init(Loss_rate);
    printf("Successfully initialized with:\n");
    printf("\tLoss rate = %d\n", Loss_rate);
    printf("\tSource filename = %s\n", Src_filename);
    printf("\tDestination filename = %s\n", Dst_filename);
    printf("\tHostname = %s\n", Hostname);
    printf("\tPort = %s\n", Port_Str);
    if (Mode == MODE_LAN) {
        printf("\tMode = LAN\n");
    } else { /*(Mode == WAN)*/
        printf("\tMode = WAN\n");
    }


    // slice
    run_sender(Src_filename, Dst_filename, Hostname, Port_Str);
}

/* Read commandline arguments */
static void Usage(int argc, char *argv[]) {

    if (argc != 5) {
        Print_help();
    }

    if (sscanf(argv[1], "%d", &Loss_rate) != 1) {
        Print_help();
    }

    if (!strncmp(argv[2], "WAN", 4)) {
        Mode = MODE_WAN;
    } else if (!strncmp(argv[2], "LAN", 4)) {
        Mode = MODE_LAN;
    } else {
        Print_help();
    }

    Src_filename = argv[3];
    Dst_filename = strtok(argv[4], "@");
    Hostname = strtok(NULL, ":");
    if (Hostname == NULL) {
        printf("Error: no hostname provided\n");
        Print_help();
    }
    Port_Str = strtok(NULL, ":");
    if (Port_Str == NULL) {
        printf("Error: no port provided\n");
        Print_help();
    }
}

static void Print_help(void) {
    printf("Usage: ncp <loss_rate_percent> <env> <source_file_name> <dest_file_name>@<ip_addr>:<port>\n");
    exit(0);
}



//slice
static void run_sender(const char* src, const char* dst_name,
                       const char* ip, const char* port_str)
{
    
    int s;
    struct addrinfo hints, *servinfo;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(ip, port_str, &hints, &servinfo) != 0) die("getaddrinfo");
    s = socket(servinfo->ai_family, servinfo->ai_socktype, 0);
    if (s < 0) die("socket");

    // 打开文件并取大小
    FILE* fp = fopen(src, "rb");
    if (!fp) die("fopen");
    struct stat st; if (stat(src, &st) != 0) die("stat");
    uint64_t fsz = (uint64_t)st.st_size;


    //读文件时顺便记录每个分片元数据 在 run_sender() 内，打开文件、得到 fsz 后，先算分片总数并建表：
    uint32_t total_segs = (uint32_t)((fsz + MAX_PAYLOAD - 1) / MAX_PAYLOAD);
    seg_t *segs = (seg_t*)calloc(total_segs, sizeof(seg_t));
    if (!segs) die("calloc");

    for (uint32_t i = 0; i < total_segs; ++i) {
        long off = (long)i * MAX_PAYLOAD;
        uint32_t this_len = (uint32_t)((off + MAX_PAYLOAD <= fsz) ? MAX_PAYLOAD : (fsz - off));
        segs[i].len = this_len;
        segs[i].file_off = off;
    }

    // 1) START
    struct {
        hdr_t h;
        char  name[256];   // 简单起见，携带目标文件名（不超 255）
    } start_pkt;
    memset(&start_pkt, 0, sizeof(start_pkt));
    start_pkt.h.type = PKT_START;
    start_pkt.h.seq  = 0;
    start_pkt.h.len  = (uint32_t)snprintf(start_pkt.name, sizeof(start_pkt.name), "%s", dst_name);
    start_pkt.h.file_size = fsz;

    sendto_dbg(s, (char*)&start_pkt, sizeof(hdr_t)+start_pkt.h.len, 0,
               servinfo->ai_addr, servinfo->ai_addrlen);

    // 窗口大小与RTO
    uint32_t W   = (Mode == MODE_LAN) ? W_LAN : W_WAN;
    uint32_t RTO = (Mode == MODE_LAN) ? RTO_LAN_MS : RTO_WAN_MS;

    uint32_t send_base = 0;      // 最早未确认的分片号
    uint32_t next_seq  = 0;      // 下一个可发分片号
    uint64_t total_sent_bytes = 0; // 包含重传的计数(用于报告)

    fd_set rfds;
    struct timeval tv;
    int nfds = 0;

    // 把 socket 记下（用于 select）
    if (servinfo->ai_family == AF_INET || servinfo->ai_family == AF_INET6) {
        nfds = s + 1;
    }

    while (send_base < total_segs) {
        // 1) 尽量填满窗口
        while (next_seq < total_segs && next_seq < send_base + W) {
            if (!segs[next_seq].acked) {
                send_one_segment(s, servinfo->ai_addr, servinfo->ai_addrlen, fp, next_seq, segs,&total_sent_bytes);
            }
            next_seq++;
        }

        // 2) 等待 ACK（带超时，用于定时器）
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        tv.tv_sec  = 0;
        tv.tv_usec = 10000; // 10ms tick
        int rv = select(nfds, &rfds, NULL, NULL, &tv);

        uint64_t now = now_ms();

        if (rv > 0 && FD_ISSET(s, &rfds)) {
            // 收包（ACK 或其他控制）
            uint8_t rbuf[sizeof(hdr_t) + 64];
            struct sockaddr_storage from; socklen_t flen = sizeof(from);
            ssize_t rcvd = recvfrom(s, rbuf, sizeof(rbuf), 0, (struct sockaddr*)&from, &flen);
            if (rcvd >= (ssize_t)sizeof(hdr_t)) {
                hdr_t *rh = (hdr_t*)rbuf;
                if (rh->type == PKT_ACK) {
                    // 累积 ACK：确认 [send_base .. rh->seq]
                    if (rh->seq + 1 > send_base) {
                        uint32_t old_base = send_base;
                        send_base = rh->seq + 1;
                        for (uint32_t i = old_base; i < send_base && i < total_segs; ++i) {
                            segs[i].acked = 1;
                        }
                    }
                }
                // （后续可处理 BUSY/START_OK/NACK 等）
            }
        }

        // 3) 超时重传（Selective Repeat：对窗口内未确认的分片检查 RTO）
        for (uint32_t i = send_base; i < next_seq; ++i) {
            if (!segs[i].acked) {
                if (now - segs[i].last_tx_ms > RTO) {
                    send_one_segment(s, servinfo->ai_addr, servinfo->ai_addrlen, fp, i, segs,&total_sent_bytes);
                }
            }
        }
    }
    // 窗口内全确认 → 发 FIN



    // // 2) 循环 DATA

    // while ((n = fread(buf, 1, MAX_PAYLOAD, fp)) > 0) {
    //     h.len = (uint32_t)n;

    //     // 组帧：头+负载
    //     uint8_t frame[sizeof(hdr_t) + MAX_PAYLOAD];
    //     memcpy(frame, &h, sizeof(hdr_t));
    //     memcpy(frame + sizeof(hdr_t), buf, n);

    //     // 发送（注意必须用 sendto_dbg）
    //     sendto_dbg(s, (const char*)frame, sizeof(hdr_t) + (int)n, 0,
    //                servinfo->ai_addr, servinfo->ai_addrlen);

    //     h.seq++; // 下一片序号
    // }

    // 3) FIN
    uint8_t  buf[MAX_PAYLOAD];
    hdr_t    h;
    memset(&h, 0, sizeof(h));
    h.type = PKT_DATA;
    h.seq  = 0;
    h.file_size = fsz;

    size_t n;
    hdr_t fin = {0};
    fin.type = PKT_FIN;
    fin.seq  = (total_segs > 0) ? (total_segs - 1) : 0;
    fin.len  = 0;
    fin.file_size = fsz;

    sendto_dbg(s, (const char*)&fin, sizeof(fin), 0,
               servinfo->ai_addr, servinfo->ai_addrlen);

    fclose(fp);
    freeaddrinfo(servinfo);
    close(s);
    printf("Sender done: %s (%lu bytes) → %s:%s\n", src, (unsigned long)fsz, ip, port_str);
}