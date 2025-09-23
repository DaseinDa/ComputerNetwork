#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h> 
#include <netdb.h>
#include <stdint.h>
#include <time.h>

#define MAX_MESS_LEN 1400

#define MODE_LAN 1
#define MODE_WAN 2


#define MAX_PAYLOAD    1360          // 保证总长<=1400B
#define PKT_START      1
#define PKT_DATA       2
#define PKT_FIN        3
#define PKT_ACK        4             // 新增：累积 ACK

#pragma pack(push,1)
typedef struct {
    uint8_t  type;        // START/DATA/FIN/ACK
    uint32_t seq;         // DATA: 分片号; ACK: 累积确认号(最后一个已按序提交的分片号)
    uint32_t len;         // 负载长度（DATA）或附带信息长度
    uint64_t file_size;   // START/FIN 携带；ACK 可不管
} hdr_t;
#pragma pack(pop)

static inline uint64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000ULL + ts.tv_nsec/1000000ULL;
}