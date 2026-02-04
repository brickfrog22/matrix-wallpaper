#define _GNU_SOURCE
#include "capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <netinet/if_ether.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <signal.h>

/* Globals */
ring_buffer_t ring_buffer;
pcap_t *pcap_handle = NULL;
atomic_ulong packets_captured = 0;
atomic_ulong bytes_per_sec = 0;
char *net_interface = NULL;
struct in_addr local_ips[MAX_LOCAL_IPS];
int local_ip_count = 0;

/* Private state for network rate tracking */
static unsigned long last_bytes = 0;
static time_t last_time = 0;

extern volatile sig_atomic_t running;

/* Check if a port is associated with encrypted traffic */
static int is_encrypted_port(uint16_t port) {
    switch (port) {
        case 443:   /* HTTPS */
        case 22:    /* SSH */
        case 993:   /* IMAPS */
        case 995:   /* POP3S */
        case 465:   /* SMTPS */
        case 587:   /* SMTP STARTTLS */
        case 853:   /* DNS over TLS */
        case 636:   /* LDAPS */
        case 989:   /* FTPS data */
        case 990:   /* FTPS control */
        case 8443:  /* HTTPS alt */
            return 1;
        default:
            return 0;
    }
}

static int is_encrypted_traffic(uint16_t src_port, uint16_t dst_port) {
    return is_encrypted_port(src_port) || is_encrypted_port(dst_port);
}

/* Initialize ring buffer */
void ring_buffer_init(ring_buffer_t *rb) {
    memset(rb, 0, sizeof(*rb));
    pthread_mutex_init(&rb->lock, NULL);
}

/* Push packet to ring buffer (thread-safe) */
int ring_buffer_push(ring_buffer_t *rb, packet_t *pkt) {
    pthread_mutex_lock(&rb->lock);

    if (rb->count >= RING_BUFFER_SIZE) {
        rb->tail = (rb->tail + 1) % RING_BUFFER_SIZE;
        rb->count--;
    }

    memcpy(&rb->packets[rb->head], pkt, sizeof(packet_t));
    rb->head = (rb->head + 1) % RING_BUFFER_SIZE;
    rb->count++;

    pthread_mutex_unlock(&rb->lock);
    return 0;
}

/* Pop packet from ring buffer (thread-safe) */
int ring_buffer_pop(ring_buffer_t *rb, packet_t *pkt) {
    pthread_mutex_lock(&rb->lock);

    if (rb->count == 0) {
        pthread_mutex_unlock(&rb->lock);
        return -1;
    }

    memcpy(pkt, &rb->packets[rb->tail], sizeof(*pkt));
    rb->tail = (rb->tail + 1) % RING_BUFFER_SIZE;
    rb->count--;

    pthread_mutex_unlock(&rb->lock);
    return 0;
}

/* Format packet info with per-character colors */
static void format_packet_info(packet_t *pkt, const struct ip *ip_hdr, int protocol,
                               uint16_t src_port, uint16_t dst_port) {
    char src_ip[INET_ADDRSTRLEN];
    char dst_ip[INET_ADDRSTRLEN];
    const char *proto_str;
    int pos = 0;

    inet_ntop(AF_INET, &ip_hdr->ip_src, src_ip, sizeof(src_ip));
    inet_ntop(AF_INET, &ip_hdr->ip_dst, dst_ip, sizeof(dst_ip));

    int is_inbound = is_local_ip(ip_hdr->ip_dst);
    pkt->is_inbound = is_inbound;

    int stream_color = is_inbound ? COLOR_INBOUND : COLOR_OUTBOUND;

    switch (protocol) {
        case IPPROTO_TCP: proto_str = "TCP"; break;
        case IPPROTO_UDP: proto_str = "UDP"; break;
        case IPPROTO_ICMP: proto_str = "ICMP"; break;
        default: proto_str = "IP"; break;
    }

    /* Protocol */
    int proto_len = strlen(proto_str);
    for (int i = 0; i < proto_len && pos < MAX_INFO_LEN - 1; i++) {
        pkt->text[pos] = proto_str[i];
        pkt->colors[pos] = stream_color;
        pos++;
    }

    if (pos < MAX_INFO_LEN - 1) {
        pkt->text[pos] = ' ';
        pkt->colors[pos] = stream_color;
        pos++;
    }

    /* Source IP */
    int src_len = strlen(src_ip);
    for (int i = 0; i < src_len && pos < MAX_INFO_LEN - 1; i++) {
        pkt->text[pos] = src_ip[i];
        pkt->colors[pos] = stream_color;
        pos++;
    }

    /* Source port */
    if (src_port > 0) {
        if (pos < MAX_INFO_LEN - 1) {
            pkt->text[pos] = ':';
            pkt->colors[pos] = stream_color;
            pos++;
        }
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%u", src_port);
        int port_len = strlen(port_str);
        for (int i = 0; i < port_len && pos < MAX_INFO_LEN - 1; i++) {
            pkt->text[pos] = port_str[i];
            pkt->colors[pos] = stream_color;
            pos++;
        }
    }

    /* Arrow */
    const char *arrow = " > ";
    for (int i = 0; arrow[i] && pos < MAX_INFO_LEN - 1; i++) {
        pkt->text[pos] = arrow[i];
        pkt->colors[pos] = stream_color;
        pos++;
    }

    /* Destination IP */
    int dst_len = strlen(dst_ip);
    for (int i = 0; i < dst_len && pos < MAX_INFO_LEN - 1; i++) {
        pkt->text[pos] = dst_ip[i];
        pkt->colors[pos] = stream_color;
        pos++;
    }

    /* Destination port */
    if (dst_port > 0) {
        if (pos < MAX_INFO_LEN - 1) {
            pkt->text[pos] = ':';
            pkt->colors[pos] = stream_color;
            pos++;
        }
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%u", dst_port);
        int port_len = strlen(port_str);
        for (int i = 0; i < port_len && pos < MAX_INFO_LEN - 1; i++) {
            pkt->text[pos] = port_str[i];
            pkt->colors[pos] = stream_color;
            pos++;
        }
    }

    pkt->is_encrypted = is_encrypted_traffic(src_port, dst_port);
    pkt->column_zone = pkt->is_encrypted ? ZONE_ENCRYPTED_META : ZONE_CLEARTEXT;

    pkt->text[pos] = '\0';
    pkt->length = pos;
}

/* pcap callback */
static void packet_handler(u_char *user, const struct pcap_pkthdr *header, const u_char *packet) {
    (void)user;

    packets_captured++;

    if (header->caplen < sizeof(struct ether_header)) return;

    const struct ether_header *eth = (struct ether_header *)packet;
    if (ntohs(eth->ether_type) != ETHERTYPE_IP) return;

    if (header->caplen < sizeof(struct ether_header) + 20) return;

    const struct ip *ip_hdr = (struct ip *)(packet + sizeof(struct ether_header));
    int ip_header_len = ip_hdr->ip_hl * 4;

    if (ip_header_len < 20) return;
    if (header->caplen < sizeof(struct ether_header) + (unsigned)ip_header_len) return;

    int protocol = ip_hdr->ip_p;

    uint16_t src_port = 0, dst_port = 0;
    int transport_header_len = 0;

    if (protocol == IPPROTO_TCP) {
        if (header->caplen >= sizeof(struct ether_header) + (unsigned)ip_header_len + 20) {
            const struct tcphdr *tcp = (struct tcphdr *)((unsigned char *)ip_hdr + ip_header_len);
            src_port = ntohs(tcp->th_sport);
            dst_port = ntohs(tcp->th_dport);
            transport_header_len = tcp->th_off * 4;
            if (transport_header_len < 20) transport_header_len = 20;
        }
    } else if (protocol == IPPROTO_UDP) {
        if (header->caplen >= sizeof(struct ether_header) + (unsigned)ip_header_len + 8) {
            const struct udphdr *udp = (struct udphdr *)((unsigned char *)ip_hdr + ip_header_len);
            src_port = ntohs(udp->uh_sport);
            dst_port = ntohs(udp->uh_dport);
            transport_header_len = 8;
        }
    }

    const u_char *payload = NULL;
    int payload_len = 0;
    int headers_len = sizeof(struct ether_header) + ip_header_len + transport_header_len;
    if (transport_header_len > 0 && (int)header->caplen > headers_len) {
        payload = packet + headers_len;
        payload_len = header->caplen - headers_len;
    }

    packet_t pkt;
    pkt.length = 0;
    pkt.is_encrypted = 0;
    pkt.is_inbound = 0;
    pkt.column_zone = 0;
    format_packet_info(&pkt, ip_hdr, protocol, src_port, dst_port);

    if (pkt.length > 0) {
        ring_buffer_push(&ring_buffer, &pkt);
    }

    /* For encrypted traffic with payload, push a separate hex-only packet */
    if (pkt.is_encrypted && payload && payload_len >= MIN_PACKET_DISPLAY) {
        packet_t hex_pkt;
        hex_pkt.is_encrypted = 1;
        hex_pkt.is_inbound = pkt.is_inbound;
        hex_pkt.column_zone = ZONE_ENCRYPTED_HEX;

        int pos = 0;
        static const char hexchars[] = "0123456789abcdef";
        int max_pos = MAX_INFO_LEN - 2;

        int hex_color = pkt.is_inbound ? COLOR_INBOUND : COLOR_OUTBOUND;
        for (int i = 0; i < payload_len && pos < max_pos; i++) {
            if (i > 0 && pos < max_pos) {
                hex_pkt.text[pos] = ' ';
                hex_pkt.colors[pos] = hex_color;
                pos++;
            }
            if (pos + 1 < max_pos) {
                hex_pkt.text[pos] = hexchars[(payload[i] >> 4) & 0x0f];
                hex_pkt.colors[pos] = hex_color;
                pos++;
                hex_pkt.text[pos] = hexchars[payload[i] & 0x0f];
                hex_pkt.colors[pos] = hex_color;
                pos++;
            }
        }

        hex_pkt.text[pos] = '\0';
        hex_pkt.length = pos;

        if (hex_pkt.length > 0) {
            ring_buffer_push(&ring_buffer, &hex_pkt);
        }
    }
}

/* Packet capture thread */
void *capture_thread(void *arg) {
    (void)arg;

    while (running) {
        int n = pcap_dispatch(pcap_handle, PCAP_BATCH_SIZE, packet_handler, NULL);
        if (n == 0) {
            usleep(CAPTURE_IDLE_US);
        }
    }

    return NULL;
}

/* Auto-detect network interface */
char *detect_interface(void) {
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) {
        char *fallback = strdup("eth0");
        if (!fallback) { perror("strdup"); exit(1); }
        return fallback;
    }

    char line[512];
    char best_iface[32] = "lo";
    unsigned long max_bytes = 0;

    if (!fgets(line, sizeof(line), f) || !fgets(line, sizeof(line), f)) {
        fclose(f);
        char *fallback = strdup("eth0");
        if (!fallback) { perror("strdup"); exit(1); }
        return fallback;
    }

    while (fgets(line, sizeof(line), f)) {
        char iface[32];
        unsigned long rx, tx;

        if (sscanf(line, " %31[^:]: %lu %*u %*u %*u %*u %*u %*u %*u %lu",
                   iface, &rx, &tx) == 3) {
            if (strcmp(iface, "lo") != 0) {
                if (rx + tx > max_bytes) {
                    max_bytes = rx + tx;
                    snprintf(best_iface, sizeof(best_iface), "%s", iface);
                }
            }
        }
    }
    fclose(f);

    char *result = strdup(best_iface);
    if (!result) { perror("strdup"); exit(1); }
    return result;
}

/* Get local IP addresses for the given interface */
void get_local_ips(const char *interface) {
    struct ifaddrs *ifaddr, *ifa;
    local_ip_count = 0;

    if (getifaddrs(&ifaddr) == -1) {
        return;
    }

    for (ifa = ifaddr; ifa != NULL && local_ip_count < MAX_LOCAL_IPS; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;

        if (interface == NULL || strcmp(ifa->ifa_name, interface) == 0) {
            struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
            local_ips[local_ip_count++] = addr->sin_addr;
        }
    }

    freeifaddrs(ifaddr);
}

/* Check if an IP address is local */
int is_local_ip(struct in_addr addr) {
    for (int i = 0; i < local_ip_count; i++) {
        if (local_ips[i].s_addr == addr.s_addr) {
            return 1;
        }
    }
    return 0;
}

/* Update network rate from /proc/net/dev */
void update_network_rate(unsigned long frame_count) {
    if (frame_count % 20 != 0) return;

    time_t now = time(NULL);
    if (now == last_time) return;

    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;

    char line[512];
    unsigned long rx_bytes = 0, tx_bytes = 0;

    while (fgets(line, sizeof(line), f)) {
        char iface[32];
        if (sscanf(line, " %31[^:]: %lu %*u %*u %*u %*u %*u %*u %*u %lu",
                   iface, &rx_bytes, &tx_bytes) == 3) {
            if (net_interface && strcmp(iface, net_interface) == 0) {
                break;
            }
        }
    }
    fclose(f);

    unsigned long total = rx_bytes + tx_bytes;
    if (last_bytes > 0) {
        bytes_per_sec = (total - last_bytes) / (now - last_time);
    }
    last_bytes = total;
    last_time = now;
}
