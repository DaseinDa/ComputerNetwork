#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sendto_dbg.h"
#include "net_include.h"


#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>


#define RECV_WINDOW 4096   // 简易缓冲上限（可调）
typedef struct {
    int      present;      // 是否已收到
    uint32_t seq;          // 分片序号
    uint32_t len;          // 该分片长度
    uint8_t  data[MAX_PAYLOAD];
} slot_t;
static void die(const char* msg) { perror(msg); exit(1); }  // ← 新增

static void Usage(int argc, char *argv[]);
static void Print_help(void);

/* Global configuration parameters (from command line) */
static int Loss_rate;
static int Mode;
static char *Port_Str;



// 将 [base, base+RECV_WINDOW) 映射到缓冲槽 idx
static inline int slot_index(uint32_t base, uint32_t seq) {
    if (seq < base) return -1;
    uint32_t d = seq - base;
    if (d >= RECV_WINDOW) return -1;
    return (int)d;
}

static void send_ack(int s, const struct sockaddr *peer, socklen_t plen, uint32_t ack_seq) {
    hdr_t ack = {0};
    ack.type = PKT_ACK;
    ack.seq  = ack_seq;   // 最后一个已按序提交的分片号
    ack.len  = 0;
    // ACK 也必须走 sendto_dbg（项目要求）
    sendto_dbg(s, (const char*)&ack, sizeof(ack), 0, peer, plen);
}

static void run_receiver(const char* port_str, int expect_loss_sim_env_is_lan_or_wan_unused)
{
    struct sockaddr_storage sender_addr;
    socklen_t sender_len = 0;
    int sender_known = 0;

    int s;
    struct addrinfo hints, *res;
    memset(&hints,0,sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags    = AI_PASSIVE;

    if (getaddrinfo(NULL, port_str, &hints, &res)!=0) die("getaddrinfo");
    s = socket(res->ai_family, res->ai_socktype, 0);
    if (s<0) die("socket");
    if (bind(s, res->ai_addr, res->ai_addrlen)<0) die("bind");
    freeaddrinfo(res);

    printf("rcv listening on %s/UDP ...\n", port_str);

    // 会话状态
    FILE*    fp = NULL;
    char     dst_name[256] = {0};
    uint64_t file_size = 0;
    uint32_t next_write_seq = 0;
    uint64_t bytes_in_order = 0;
    int      fin_seen = 0;            // 收到 FIN
    uint32_t fin_seq  = 0;            // 期望的总包数（来自 FIN）

    // 简易滑动窗口缓冲
    slot_t*  buf = (slot_t*)calloc(RECV_WINDOW, sizeof(slot_t));
    if (!buf) die("calloc");

    // 接收 loop
    for (;;) {
        uint8_t frame[sizeof(hdr_t) + MAX_PAYLOAD + 300]; // 预留
        struct sockaddr_storage peer; socklen_t plen = sizeof(peer);
        ssize_t r = recvfrom(s, frame, sizeof(frame), 0, (struct sockaddr*)&peer, &plen);
        if (r < (ssize_t)sizeof(hdr_t)) continue;

        hdr_t* h = (hdr_t*)frame;
        uint8_t* payload = frame + sizeof(hdr_t);

        if (h->type == PKT_START) {
            // 只接受空闲状态下的 START；简化：单会话
            if (fp != NULL) {
                // 忙的话这里应回 BUSY（等你后续做握手/并发控制）
                continue;
            }
            file_size = h->file_size;
            // 取文件名（h->len 为 name 长度）
            size_t name_len = (size_t)h->len;
            if (name_len > sizeof(dst_name)-1) name_len = sizeof(dst_name)-1;
            memcpy(dst_name, payload, name_len);
            dst_name[name_len] = '\0';

            fp = fopen(dst_name, "wb");
            if (!fp) die("fopen");
            next_write_seq = 0;
            bytes_in_order = 0;
            fin_seen = 0;
            printf("START: recv -> %s (size=%lu)\n", dst_name, (unsigned long)file_size);
            memcpy(&sender_addr, &peer, sizeof(peer));
            sender_len = plen;
            sender_known = 1;
        }
        else if (h->type == PKT_DATA) {
            if (!fp) continue; // 未 START，忽略
            // 放入窗口缓冲
            int idx = slot_index(next_write_seq, h->seq);
            if (idx < 0) {
                // 超出窗口太远或重复在窗口左边，先忽略（后续加NACK/重传）
                continue;
            }
            if (!buf[idx].present) {
                buf[idx].present = 1;
                buf[idx].seq = h->seq;
                buf[idx].len = h->len;
                memcpy(buf[idx].data, payload, h->len);
            }

            // 尝试按序 flush
            while (buf[0].present) {
                // 写盘
                fwrite(buf[0].data, 1, buf[0].len, fp);
                bytes_in_order += buf[0].len;
                next_write_seq++;

                // 窗口整体左移一格（O(W) 简单实现；后续可用环形缓冲优化成 O(1)）
                memmove(&buf[0], &buf[1], sizeof(slot_t) * (RECV_WINDOW - 1));
                memset(&buf[RECV_WINDOW - 1], 0, sizeof(slot_t));
            }
            // 只要推进了按序进度，就发一次 ACK
            if (sender_known && next_write_seq > 0) {
                send_ack(s, (struct sockaddr*)&sender_addr, sender_len, next_write_seq - 1);
            }

            // 这里也可以做“每 10MB 输出一次中间速率”的计数（后续再加时间戳/速率计算）
        }
        else if (h->type == PKT_FIN) {
            fin_seen = 1; fin_seq = h->seq; file_size = h->file_size;
            printf("FIN seen: total_seq=%u, size=%lu\n", fin_seq, (unsigned long)file_size);

            // 如果已经全部按序写完（简单判断：bytes_in_order == file_size）
            if (fp && bytes_in_order == file_size) {
                fclose(fp); fp = NULL;
                printf("RECV DONE: %s (%lu bytes)\n", dst_name, (unsigned long)bytes_in_order);
                // 简化：单次会话就退出；你也可以 while 等下一次 START
                break;
            }
            // 否则继续等前面洞补齐（后续会用重传/超时推动）
        }
    }

    free(buf);
    close(s);
}

int main(int argc, char *argv[]) {
    /* Initialize */
    Usage(argc, argv);
    sendto_dbg_init(Loss_rate);
    printf("Successfully initialized with:\n");
    printf("\tLoss rate = %d\n", Loss_rate);
    printf("\tPort = %s\n", Port_Str);
    if (Mode == MODE_LAN) {
        printf("\tMode = LAN\n");
    } else { /*(Mode == WAN)*/
        printf("\tMode = WAN\n");
    }
    run_receiver(Port_Str, Mode);
}

/* Read commandline arguments */
static void Usage(int argc, char *argv[]) {
    if (argc != 4) {
        Print_help();
    }

    if (sscanf(argv[1], "%d", &Loss_rate) != 1) {
        Print_help();
    }

    Port_Str = argv[2];

    if (!strncmp(argv[3], "WAN", 4)) {
        Mode = MODE_WAN;
    } else if (!strncmp(argv[3], "LAN", 4)) {
        Mode = MODE_LAN;
    } else {
        Print_help();
    }
}

static void Print_help(void) {
    printf("Usage: rcv <loss_rate_percent> <port> <env>\n");
    exit(0);
}
