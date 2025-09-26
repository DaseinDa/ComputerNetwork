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
enum { W_LAN = 512, W_WAN = 2000 };
static const uint32_t RTO_LAN_MS = 60;
static const uint32_t RTO_WAN_MS = 200;

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
    // >>> 在这里记录发送起始时间 <<<
    uint64_t snd_start_ms = now_ms();

    /* ---- Handshake: wait START_OK or BUSY (queue if busy) ---- */
    int start_ok = 0;
    uint32_t backoff_ms = 100;            // 初始退避
    const uint32_t backoff_max = 2000;    // 最大退避 2s
    uint64_t last_start_tx = 0;
    /* 新增：记录本次 START 的发送时间，供超时重发用 */
    last_start_tx = now_ms();
        
    for (;;) {
        // 等待接收端响应（START_OK / BUSY），最多 300ms
        fd_set hs_rfds; FD_ZERO(&hs_rfds); FD_SET(s, &hs_rfds);
        struct timeval hs_tv = {.tv_sec=0, .tv_usec=300*1000};
        int hs_rv = select(s+1, &hs_rfds, NULL, NULL, &hs_tv);
        if (hs_rv > 0 && FD_ISSET(s, &hs_rfds)) {
            uint8_t rbuf[sizeof(hdr_t) + 64];
            struct sockaddr_storage from; socklen_t flen = sizeof(from);
            ssize_t rcvd = recvfrom(s, rbuf, sizeof(rbuf), 0, (struct sockaddr*)&from, &flen);
            if (rcvd >= (ssize_t)sizeof(hdr_t)) {
                hdr_t *rh = (hdr_t*)rbuf;
                if (rh->type == PKT_START_OK) {    // 接收端就绪
                    start_ok = 1;
                    break;
                } else if (rh->type == PKT_BUSY) { // 排队：指数退避 + 重发 START
                    // 等 backoff
                    usleep(backoff_ms * 1000);
                    // 重发 START（复用你已有的 START 发送逻辑/缓冲）
                    // 如果你有封装函数，直接调用；否则把你发 START 的两行黏贴到这里
                    // 例：sendto_dbg(s, (char*)&start_pkt, sizeof(hdr_t)+start_pkt.h.len, 0, servinfo->ai_addr, servinfo->ai_addrlen);
                    sendto_dbg(s, (char*)&start_pkt, sizeof(hdr_t)+start_pkt.h.len, 0,
                            servinfo->ai_addr, servinfo->ai_addrlen);
                    // 指数退避（上限 2s）
                    backoff_ms = (backoff_ms < backoff_max) ? (backoff_ms * 2) : backoff_max;
                    last_start_tx = now_ms();
                    continue; // 继续排队
                }
            }
        } else {
       // 超时：保活重发 START（不放行，继续排队等待 START_OK）
               // 超时：保活重发 START（不放行，继续排队等待 START_OK）
            if (now_ms() - last_start_tx > 500) {
            sendto_dbg(s, (char*)&start_pkt, sizeof(hdr_t)+start_pkt.h.len, 0,
                        servinfo->ai_addr, servinfo->ai_addrlen);
                last_start_tx = now_ms();
            }
            continue;
        }
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
                }else if (rh->type == PKT_NACK) {
                        uint32_t want = rh->seq;  // 接收端告诉我们缺这个分片
                        if (want < total_segs && !segs[want].acked) {
                            // 立即重传这个分片
                            send_one_segment(s, servinfo->ai_addr, servinfo->ai_addrlen,
                                            fp, want, segs, &total_sent_bytes);
                            // 可选：记录一下 NACK 命中次数/日志
                            // printf("[SND] NACK->rexmit %u\n", want);
                        }
                    }else if (rh->type == PKT_BUSY) {
                        // 接收端忙，说明它正服务别人——我也得排队
                            // 正在服务别人 → 暂停发送，进入排队：退避 + 重发 START，直到放行
                        uint32_t backoff_ms2 = 100;
                        const uint32_t backoff_max2 = 2000;
                        for (;;) {
                            usleep(backoff_ms2 * 1000);
                            // 重发 START（同上）
                            sendto_dbg(s, (char*)&start_pkt, sizeof(hdr_t)+start_pkt.h.len, 0,
                                    servinfo->ai_addr, servinfo->ai_addrlen);

                            // 等一小会看看是否仍 BUSY 或已就绪
                            fd_set q_rfds; FD_ZERO(&q_rfds); FD_SET(s, &q_rfds);
                            struct timeval q_tv = {.tv_sec=0, .tv_usec=300*1000};
                            int q_rv = select(s+1, &q_rfds, NULL, NULL, &q_tv);
                            if (q_rv > 0 && FD_ISSET(s, &q_rfds)) {
                                uint8_t qbuf[sizeof(hdr_t) + 64];
                                struct sockaddr_storage qfrom; socklen_t qlen = sizeof(qfrom);
                                ssize_t qn = recvfrom(s, qbuf, sizeof(qbuf), 0, (struct sockaddr*)&qfrom, &qlen);
                                if (qn >= (ssize_t)sizeof(hdr_t)) {
                                    hdr_t *qh = (hdr_t*)qbuf;
                                    if (qh->type == PKT_START_OK) {
                                        // 放行，退出排队循环，继续数据阶段
                                        break;
                                    } else if (qh->type == PKT_BUSY) {
                                        // 继续排队（指数退避）
                                        backoff_ms2 = (backoff_ms2 < backoff_max2) ? (backoff_ms2 * 2) : backoff_max2;
                                        continue;
                                    }
                                }
                            } else {
                                // 没有响应：也视为放行（兼容 rcv 不发 START_OK 的情况）
                                break;
                            }
                        }

                            printf("[SND] Receiver is busy, I was blocked.\n");
                            // 可以选择重试，或者直接退出
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
    //    在这里记录结束时间
    uint64_t snd_end_ms = now_ms();
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

    //发完 FIN 后打印统计
    double snd_elapsed_s = (snd_end_ms - snd_start_ms) / 1000.0;
    double over_wire_MB  = total_sent_bytes / (1024.0*1024.0);
    double over_wire_mbps = (total_sent_bytes * 8.0) / (snd_elapsed_s * 1e6);

    // 若你有文件大小 fsz，可计算冗余度（含重传的发送量 / 实际文件大小）
    double redundancy = (fsz > 0) ? ((double)total_sent_bytes / (double)fsz) : 0.0;

    printf("[SND] SENT(total incl. retrans): %.2f MB in %.2f s, avg send rate: %.2f Mb/s\n",
        over_wire_MB, snd_elapsed_s, over_wire_mbps);
    printf("[SND] Redundancy (bytes_sent/file_size): %.2fx\n", redundancy);
    fflush(stdout);


    fclose(fp);
    freeaddrinfo(servinfo);
    close(s);
    printf("Sender done: %s (%lu bytes) → %s:%s\n", src, (unsigned long)fsz, ip, port_str);
}
