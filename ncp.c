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


static void Usage(int argc, char *argv[]);
static void Print_help(void);

/* Global configuration parameters (from command line) */
static int Loss_rate;
static int Mode;
static char *Port_Str;
static char *Src_filename;
static char *Dst_filename;
static char *Hostname;
static void run_sender(const char* src,
                       const char* dst_name,
                       const char* ip,
                       const char* port_str);

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

static void die(const char* msg){ perror(msg); exit(1); }

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

    // 2) 循环 DATA
    uint8_t  buf[MAX_PAYLOAD];
    hdr_t    h;
    memset(&h, 0, sizeof(h));
    h.type = PKT_DATA;
    h.seq  = 0;
    h.file_size = fsz;

    size_t n;
    while ((n = fread(buf, 1, MAX_PAYLOAD, fp)) > 0) {
        h.len = (uint32_t)n;

        // 组帧：头+负载
        uint8_t frame[sizeof(hdr_t) + MAX_PAYLOAD];
        memcpy(frame, &h, sizeof(hdr_t));
        memcpy(frame + sizeof(hdr_t), buf, n);

        // 发送（注意必须用 sendto_dbg）
        sendto_dbg(s, (const char*)frame, sizeof(hdr_t) + (int)n, 0,
                   servinfo->ai_addr, servinfo->ai_addrlen);

        h.seq++; // 下一片序号
    }

    // 3) FIN
    hdr_t fin = {0};
    fin.type = PKT_FIN;
    fin.seq  = h.seq;     // 下一期望序号（总包数）
    fin.len  = 0;
    fin.file_size = fsz;

    sendto_dbg(s, (const char*)&fin, sizeof(fin), 0,
               servinfo->ai_addr, servinfo->ai_addrlen);

    fclose(fp);
    freeaddrinfo(servinfo);
    close(s);
    printf("Sender done: %s (%lu bytes) → %s:%s\n", src, (unsigned long)fsz, ip, port_str);
}