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
#define TEN_MB (10u * 1024u * 1024u)

static void die(const char* msg) { perror(msg); exit(1); }  // ← 新增

static void Usage(int argc, char *argv[]);
static void Print_help(void);

/* Global configuration parameters (from command line) */
static int Loss_rate;
static int Mode;
static char *Port_Str;

static void send_start_ok(int s, const struct sockaddr *to, socklen_t tolen){
    hdr_t h = {0};
    h.type = PKT_START_OK;
    sendto_dbg(s, (const char*)&h, sizeof(h), 0, to, tolen);
}
static int busy = 0; // 当前是否在接收一个会话
static struct sockaddr_storage cur_peer;
static socklen_t               cur_plen = 0;

// 会话活跃时间（用于超时清理）
static uint64_t last_activity_ms = 0;
static const uint64_t SESSION_IDLE_TIMEOUT_MS = 5000; // 5s，可按需调

static int same_peer(const struct sockaddr_storage* a, socklen_t alen,
                     const struct sockaddr_storage* b, socklen_t blen)
{
    if (a->ss_family != b->ss_family || alen != blen) return 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *pa=(const struct sockaddr_in*)a, *pb=(const struct sockaddr_in*)b;
        return pa->sin_port==pb->sin_port &&
               memcmp(&pa->sin_addr,&pb->sin_addr,sizeof(struct in_addr))==0;
    } else if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *pa=(const struct sockaddr_in6*)a, *pb=(const struct sockaddr_in6*)b;
        return pa->sin6_port==pb->sin6_port &&
               memcmp(&pa->sin6_addr,&pb->sin6_addr,sizeof(struct in6_addr))==0;
    }
    return 0;
}

static void send_busy(int s, const struct sockaddr *to, socklen_t tolen)
{
    hdr_t h = {0};
    h.type = PKT_BUSY;            // 需要你在 net_include.h 里定义 PKT_BUSY
    sendto_dbg(s, (const char*)&h, sizeof(h), 0, to, tolen);
}

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
    ack.seq  = ack_seq;   // Last in-order sequence number received
    ack.len  = 0;
    // ACK has to go sendto_dbg(required by project)
    sendto_dbg(s, (const char*)&ack, sizeof(ack), 0, peer, plen);
}
static void send_nack(int s, const struct sockaddr *peer, socklen_t plen, uint32_t want_seq) {
    hdr_t nack = (hdr_t){0};
    nack.type = PKT_NACK;
    nack.seq  = want_seq;   // 告诉发送端：我现在需要这个分片（next_write_seq）
    nack.len  = 0;
    sendto_dbg(s, (const char*)&nack, sizeof(nack), 0, peer, plen);
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
    //NACK
    uint64_t last_nack_ms = 0;     // 上次发NACK时间（节流）
    const   uint64_t nack_gap_ms = 50; // NACK最小间隔


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
    //Statistics
    uint64_t start_ms = 0;        // 本次会话开始时间（收到 START 后）
    uint64_t last_mark_ms = 0;    // 上一个 10MB 报告时间
    uint64_t last_mark_bytes = 0; // 上一个 10MB 报告时的有序字节数


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

        uint64_t now = now_ms();
        if (busy && last_activity_ms > 0 && now - last_activity_ms > SESSION_IDLE_TIMEOUT_MS) {
            if (fp) { fclose(fp); fp = NULL; }
            busy = 0; sender_known = 0; cur_plen = 0;
            next_write_seq = 0; bytes_in_order = 0; fin_seen = 0; fin_seq = 0;
            memset(&cur_peer, 0, sizeof(cur_peer));
            memset(buf, 0, sizeof(slot_t) * RECV_WINDOW);
            printf("[RCV] session idle timeout, back to IDLE.\n");
        }


        if (h->type == PKT_START) {

            if (busy) {
                if (!same_peer(&peer, plen, &cur_peer, cur_plen)) {
                    send_busy(s, (struct sockaddr*)&peer, plen);
                    continue;
                }
                // ✅ 同一 sender 的重复 START：只重发 START_OK，不重置会话/不重开文件
                send_start_ok(s, (struct sockaddr*)&peer, plen);
                last_activity_ms = now_ms();
                continue;
            } else {
                // 空闲：登记 sender，一次性初始化
                busy = 1;
                memcpy(&cur_peer, &peer, sizeof(peer));
                cur_plen = plen;
            }
            // 同一个 sender 的新 START：重启会话（关闭旧文件，清状态）
            if (fp) { fclose(fp); fp = NULL; }
            next_write_seq = 0; bytes_in_order = 0; fin_seen = 0; fin_seq = 0;
            memset(buf, 0, sizeof(slot_t) * RECV_WINDOW);


            file_size = h->file_size;
            // 取文件名（h->len 为 name 长度）
            size_t name_len = (size_t)h->len;
            if (name_len > sizeof(dst_name)-1) name_len = sizeof(dst_name)-1;
            memcpy(dst_name, payload, name_len);
            dst_name[name_len] = '\0';

            fp = fopen(dst_name, "wb");

            //Statistics
            start_ms = now_ms();
            last_mark_ms = start_ms;
            last_mark_bytes = 0;


            if (!fp) die("fopen");
            next_write_seq = 0;
            bytes_in_order = 0;
            fin_seen = 0;
            printf("START: recv -> %s (size=%lu)\n", dst_name, (unsigned long)file_size);
            memcpy(&sender_addr, &peer, sizeof(peer));
            sender_len = plen;
            sender_known = 1;
            send_start_ok(s, (struct sockaddr*)&peer, plen);
            last_activity_ms = now_ms();

        }else if (h->type == PKT_DATA) {
            if (!busy) {
                // 还没会话就来了 FIN（可能 START/数据都丢了）——忽略
                continue;
            }
            // 仅接受当前 sender 的 FIN
            if (!same_peer(&peer, plen, &cur_peer, cur_plen)) {
                send_busy(s, (struct sockaddr*)&peer, plen);
                continue;
            }

            last_activity_ms = now_ms();
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
            // ---- NACK 触发逻辑（有缺口时主动催促 next_write_seq）----
        if (sender_known) {
            // 条件1：最左槽 buf[0] 仍缺（说明按序缺口存在）
            // 条件2：但右侧已有至少一个乱序片（说明不是发得太慢，而是确实丢了一个洞）
            int has_right = 0;
            for (int i = 1; i < RECV_WINDOW; ++i) {
                if (buf[i].present) { has_right = 1; break; }
            }
            if (!buf[0].present && has_right) {
                uint64_t nowms = now_ms();
                if (nowms - last_nack_ms >= nack_gap_ms) {
                    // 向发送端请求 next_write_seq 这个分片
                    send_nack(s, (struct sockaddr*)&sender_addr, sender_len, next_write_seq);
                    last_nack_ms = nowms;
                    // 可选日志：printf("[RCV] NACK want=%u\n", next_write_seq);
                }
            }
        }


            // 10MB 打点：每当有序累计写入增加了 >=10MB，就打印一次
            uint64_t delta_bytes = bytes_in_order - last_mark_bytes;
            if (delta_bytes >= TEN_MB) {
                uint64_t now = now_ms();
                uint64_t delta_ms = (now - last_mark_ms) ? (now - last_mark_ms) : 1; // 防除零
                double recent_mbps = (delta_bytes * 8.0) / (double)delta_ms / 1000.0; // Mb/s
                printf("[RCV] Progress: %.2f MB total, recent 10MB avg rate: %.2f Mb/s\n",
                    bytes_in_order / (1024.0*1024.0), recent_mbps);
                fflush(stdout);
                last_mark_ms = now;
                // 如果增量 >10MB（一次推进很多），也只前进一个 10MB 档位
                last_mark_bytes += TEN_MB;
            }

            // 只要推进了按序进度，就发一次 ACK
            if (sender_known && next_write_seq > 0) {
                send_ack(s, (struct sockaddr*)&sender_addr, sender_len, next_write_seq - 1);
            }
            // 更新会话活跃时间
            last_activity_ms = now_ms();
            
            // 这里也可以做“每 10MB 输出一次中间速率”的计数（后续再加时间戳/速率计算）
        }
        else if (h->type == PKT_FIN) {

            if (!busy) {
                // 还没会话就来了 FIN（可能 START/数据都丢了）——忽略
                continue;
            }
            last_activity_ms = now_ms();
            // 仅接受当前 sender 的 FIN
            if (!same_peer(&peer, plen, &cur_peer, cur_plen)) {
                send_busy(s, (struct sockaddr*)&peer, plen);
                continue;
            }
            fin_seen = 1; fin_seq = h->seq; file_size = h->file_size;
            printf("FIN seen: total_seq=%u, size=%lu\n", fin_seq, (unsigned long)file_size);
            uint64_t end_ms = now_ms();


            // 如果已经全部按序写完（简单判断：bytes_in_order == file_size）
            if (fp && bytes_in_order == file_size) {
                //When finished, print statistics result
                double elapsed_s = (end_ms - start_ms) / 1000.0;
                double avg_mbps = (bytes_in_order * 8.0) / (elapsed_s * 1e6);
                printf("[RCV] DONE: %.2f MB in %.2f s, avg goodput: %.2f Mb/s\n",
                    bytes_in_order / (1024.0*1024.0), elapsed_s, avg_mbps);
                fflush(stdout);
                
                fclose(fp); fp = NULL;
                printf("RECV DONE: %s (%lu bytes)\n", dst_name, (unsigned long)bytes_in_order);
                // 简化：单次会话就退出；你也可以 while 等下一次 START
                // ✅ 关键：不退出进程，复位为“空闲”，接受下一次 START
                busy = 0;
                sender_known = 0;
                memset(&cur_peer, 0, sizeof(cur_peer));
                cur_plen = 0;

                // 复位流水线状态
                next_write_seq  = 0;
                bytes_in_order  = 0;
                fin_seen        = 0;
                fin_seq         = 0;
                memset(buf, 0, sizeof(slot_t) * RECV_WINDOW);

                printf("Receiver is ready for the next session.\n");
                continue;   // 回到 for(;;) 顶部，等待下一次 START
            }
                last_activity_ms = now_ms();   // 否则继续等前面洞补齐（后续会用重传/超时推动）
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
