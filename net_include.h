#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h> 
#include <netdb.h>

#define MAX_MESS_LEN 1400

#define MODE_LAN 1
#define MODE_WAN 2

#define MAX_PAYLOAD    1360           // 1400 - 头部约40字节(对齐安全)
#define PKT_START      1
#define PKT_DATA       2
#define PKT_FIN        3
#define PKT_ACK        4              // 预留，本文先不用

#pragma pack(push,1)
typedef struct {
    uint8_t  type;        // START/DATA/FIN
    uint32_t seq;         // DATA 包序号：从 0 开始，每包 +1
    uint32_t len;         // DATA 负载长度
    uint64_t file_size;   // START/FIN 携带；DATA 可不管
} hdr_t;
#pragma pack(pop)
typedef struct dummy_ncp_msg {
    /* Fill in header information needed for your protocol */
    char payload[MAX_MESS_LEN];
} ncp_msg;

/* Uncomment and fill in fields for your protocol */
/*
typedef struct dummy_rcv_msg {
} rcv_msg;
*/
