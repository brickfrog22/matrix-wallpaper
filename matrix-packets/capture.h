#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdint.h>
#include <pthread.h>
#include <pcap.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <stdatomic.h>

/* Configuration */
#define MAX_PACKET_SIZE  1500
#define RING_BUFFER_SIZE 2048
#define MIN_PACKET_DISPLAY 20
#define PCAP_BATCH_SIZE  64
#define CAPTURE_IDLE_US  1000
#define PCAP_TIMEOUT_MS  100
#define MAX_LOCAL_IPS    8

/* Formatted packet info stored in ring buffer */
#define MAX_INFO_LEN 256

/* Column zone assignments */
#define ZONE_ENCRYPTED_META  0
#define ZONE_ENCRYPTED_HEX   1
#define ZONE_CLEARTEXT       2

/* Color pair IDs (shared with renderer) */
#define COLOR_SRC_IP     1
#define COLOR_DST_IP     2
#define COLOR_PORT       3
#define COLOR_PROTO      4
#define COLOR_ARROW      5
#define COLOR_HEAD       6
#define COLOR_FADING     7
#define COLOR_HEX        8
#define COLOR_INBOUND    9
#define COLOR_OUTBOUND   10

typedef struct {
    char text[MAX_INFO_LEN];
    int colors[MAX_INFO_LEN];
    int length;
    int is_encrypted;
    int is_inbound;
    int column_zone;
} packet_t;

typedef struct {
    packet_t packets[RING_BUFFER_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t lock;
} ring_buffer_t;

/* Globals (defined in capture.c) */
extern ring_buffer_t ring_buffer;
extern pcap_t *pcap_handle;
extern atomic_ulong packets_captured;
extern atomic_ulong bytes_per_sec;
extern char *net_interface;
extern struct in_addr local_ips[MAX_LOCAL_IPS];
extern int local_ip_count;

/* Ring buffer operations */
void ring_buffer_init(ring_buffer_t *rb);
int ring_buffer_push(ring_buffer_t *rb, packet_t *pkt);
int ring_buffer_pop(ring_buffer_t *rb, packet_t *pkt);

/* Packet capture */
void *capture_thread(void *arg);

/* Network helpers */
char *detect_interface(void);
void get_local_ips(const char *interface);
int is_local_ip(struct in_addr addr);
void update_network_rate(unsigned long frame_count);

#endif /* CAPTURE_H */
