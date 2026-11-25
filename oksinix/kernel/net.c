/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - BSD-Style Sockets & IPv4/ARP/UDP/ICMP Stack
 */

#include "net.h"
#include "xhci.h"
#include "pit.h"
#include "io.h"
#include "serial.h"
#include "string.h"

/* Linux negative errno constants */
#define NET_EBADF         9
#define NET_EAGAIN        11
#define NET_EFAULT        14
#define NET_EINVAL        22
#define NET_EMFILE        24
#define NET_EPIPE         32
#define NET_EDESTADDRREQ  89
#define NET_EPROTONOSUPP  93
#define NET_EAFNOSUPPORT  97
#define NET_EADDRINUSE    98
#define NET_ENETUNREACH   101
#define NET_ENOTCONN      107
#define NET_ETIMEDOUT     110
#define NET_ECONNREFUSED  111

#define NET_SOCK_RX_SLOTS 16U
#define NET_SOCK_RX_MAX   1500U
#define NET_ARP_CACHE_SZ  8U
#define NET_ARP_TTL_TICKS 60000U /* 60s at 1000 Hz PIT */
#define NET_REASM_SLOTS   2U
#define NET_REASM_MAX     2048U  /* 256 * 8-byte fragment blocks */
#define NET_REASM_TTL     5000U  /* 5s fragment reassembly timeout */
#define TCP_RCV_WIN       4096U

#define TCP_STATE_CLOSED      0U
#define TCP_STATE_SYN_SENT    1U
#define TCP_STATE_ESTABLISHED 2U
#define TCP_STATE_CLOSE_WAIT  3U
#define TCP_STATE_FIN_WAIT    4U

#define TCP_FIN 0x01U
#define TCP_SYN 0x02U
#define TCP_RST 0x04U
#define TCP_PSH 0x08U
#define TCP_ACK 0x10U

struct net_rx_pkt {
    uint32_t src_ip;        /* network byte order */
    uint16_t src_port;      /* network byte order */
    uint16_t len;
    uint16_t off;
    uint8_t  data[NET_SOCK_RX_MAX];
};

struct net_sock {
    bool_t            used;
    int               domain;
    int               type;
    int               protocol;
    bool_t            bound;
    bool_t            connected;
    uint32_t          local_ip;     /* network byte order */
    uint16_t          local_port;   /* network byte order */
    uint32_t          peer_ip;      /* network byte order */
    uint16_t          peer_port;    /* network byte order */
    uint8_t           tcp_state;
    bool_t            tcp_eof;
    bool_t            tcp_rst;
    uint32_t          tcp_snd_nxt;
    uint32_t          tcp_snd_una;
    uint32_t          tcp_rcv_nxt;
    uint32_t          rx_head;
    uint32_t          rx_tail;
    struct net_rx_pkt rx_q[NET_SOCK_RX_SLOTS];
};

struct net_arp_entry {
    bool_t   valid;
    uint32_t ip;            /* network byte order */
    uint8_t  mac[6];
    uint32_t expires_tick;
    uint32_t last_used_tick;
};

struct net_ipv4_reasm {
    bool_t   used;
    bool_t   have_last;
    uint8_t  proto;
    uint16_t ip_id;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint32_t expires_tick;
    uint32_t total_len;
    uint8_t  block_map[NET_REASM_MAX / 64U]; /* 1 bit per 8-byte fragment block */
    uint8_t  data[NET_REASM_MAX];
};

static struct net_sock       sock_table[NET_MAX_SOCKETS];
static struct net_arp_entry  arp_cache[NET_ARP_CACHE_SZ];
static struct net_ipv4_reasm reasm_table[NET_REASM_SLOTS];
static uint8_t               reasm_deliver_buf[NET_REASM_MAX];
static uint32_t             arp_pending_ip = 0UL;
static uint32_t             local_ip_be    = 0;
static uint32_t             netmask_be     = 0;
static uint32_t             gateway_ip_be  = 0;
static uint32_t             dns_ip_be      = 0;
static bool_t               dhcp_zero_src  = FALSE;
static bool_t               dhcp_active    = FALSE;
static uint32_t             tcp_isn_secret = 0x9E3779B9UL;
static uint16_t             next_ephemeral_port = 49152U;
static uint16_t             ipv4_ident = 1U;
static uint16_t             dns_txid   = 1U;
static uint8_t              net_rx_frame_buf[1600];
static uint8_t              net_tx_frame_buf[1600];
static uint8_t              tcp_tx_buf[1520];

static int net_send_ipv4(uint32_t dst_ip_be, uint8_t proto,
                         const uint8_t *payload, uint32_t payload_len);

uint16_t net_htons(uint16_t v)
{
    return (uint16_t)((uint16_t)((v & 0x00FFU) << 8) | (uint16_t)((v >> 8) & 0x00FFU));
}

uint16_t net_ntohs(uint16_t v)
{
    return net_htons(v);
}

uint32_t net_htonl(uint32_t v)
{
    return ((v & 0x000000FFUL) << 24) |
           ((v & 0x0000FF00UL) << 8)  |
           ((v & 0x00FF0000UL) >> 8)  |
           ((v & 0xFF000000UL) >> 24);
}

uint32_t net_ntohl(uint32_t v)
{
    return net_htonl(v);
}

uint32_t net_make_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    uint32_t host = ((uint32_t)a << 24) | ((uint32_t)b << 16) |
                    ((uint32_t)c << 8)  | (uint32_t)d;
    return net_htonl(host);
}

static uint16_t net_checksum16(const uint8_t *data, uint32_t len)
{
    uint32_t sum = 0;
    uint32_t i;

    for (i = 0; i + 1U < len; i += 2U) {
        sum += ((uint32_t)data[i] << 8) | (uint32_t)data[i + 1U];
    }
    if ((len & 1U) != 0U) {
        sum += (uint32_t)data[len - 1U] << 8;
    }
    while ((sum >> 16) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFFU);
}

static uint32_t read_be32(const uint8_t *p)
{
    uint32_t val;
    memcpy(&val, p, 4U);
    return val;
}

static void write_be32(uint8_t *p, uint32_t val)
{
    memcpy(p, &val, 4U);
}

static bool_t is_loopback_ip(uint32_t ip_be)
{
    uint8_t first_octet = (uint8_t)(ip_be & 0xFFU);
    return (first_octet == 127U || ip_be == local_ip_be) ? TRUE : FALSE;
}

static struct net_sock *get_sock(int sockfd)
{
    int idx = sockfd - NET_SOCK_FD_BASE;
    if (idx < 0 || idx >= NET_MAX_SOCKETS || !sock_table[idx].used) {
        return NULL;
    }
    return &sock_table[idx];
}

static void sock_enqueue_rx(struct net_sock *sk, uint32_t src_ip, uint16_t src_port,
                            const uint8_t *data, uint32_t len)
{
    uint32_t next_head;

    if (sk == NULL || data == NULL || len == 0U) {
        return;
    }
    if (len > NET_SOCK_RX_MAX) {
        len = NET_SOCK_RX_MAX;
    }
    next_head = (sk->rx_head + 1U) % NET_SOCK_RX_SLOTS;
    if (next_head == sk->rx_tail) {
        sk->rx_tail = (sk->rx_tail + 1U) % NET_SOCK_RX_SLOTS;
    }
    sk->rx_q[sk->rx_head].src_ip   = src_ip;
    sk->rx_q[sk->rx_head].src_port = src_port;
    sk->rx_q[sk->rx_head].len      = (uint16_t)len;
    sk->rx_q[sk->rx_head].off      = 0U;
    memcpy(sk->rx_q[sk->rx_head].data, data, len);
    sk->rx_head = next_head;
}

static uint16_t net_tcp_checksum(uint32_t src_ip_be, uint32_t dst_ip_be,
                                 const uint8_t *seg, uint32_t seg_len)
{
    uint32_t sum = 0;
    uint32_t i;
    uint8_t pseudo[12];

    write_be32(&pseudo[0], src_ip_be);
    write_be32(&pseudo[4], dst_ip_be);
    pseudo[8]  = 0U;
    pseudo[9]  = IPPROTO_TCP;
    pseudo[10] = (uint8_t)((seg_len >> 8) & 0xFFU);
    pseudo[11] = (uint8_t)(seg_len & 0xFFU);

    for (i = 0; i < 12U; i += 2U) {
        sum += ((uint32_t)pseudo[i] << 8) | (uint32_t)pseudo[i + 1U];
    }
    for (i = 0; i + 1U < seg_len; i += 2U) {
        sum += ((uint32_t)seg[i] << 8) | (uint32_t)seg[i + 1U];
    }
    if ((seg_len & 1U) != 0U) {
        sum += (uint32_t)seg[seg_len - 1U] << 8;
    }
    while ((sum >> 16) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFFU);
}

static int tcp_send_segment(struct net_sock *sk, uint8_t flags,
                            uint32_t seq, uint32_t ack,
                            const uint8_t *data, uint32_t len)
{
    uint32_t hdr_len = ((flags & TCP_SYN) != 0U) ? 24U : 20U;
    uint32_t seg_len;
    uint32_t seq_be;
    uint32_t ack_be;
    uint16_t ck;

    if (len > 1400U) {
        len = 1400U;
    }
    seg_len = hdr_len + len;
    memset(tcp_tx_buf, 0, hdr_len);
    memcpy(&tcp_tx_buf[0], &sk->local_port, 2U);
    memcpy(&tcp_tx_buf[2], &sk->peer_port, 2U);
    seq_be = net_htonl(seq);
    ack_be = net_htonl(ack);
    write_be32(&tcp_tx_buf[4], seq_be);
    write_be32(&tcp_tx_buf[8], ack_be);
    tcp_tx_buf[12] = (uint8_t)((hdr_len / 4U) << 4);
    tcp_tx_buf[13] = flags;
    tcp_tx_buf[14] = 0x10U; /* 4096-byte receive window */
    tcp_tx_buf[15] = 0x00U;
    if ((flags & TCP_SYN) != 0U) {
        /* MSS option = 1400 bytes (0x0578) */
        tcp_tx_buf[20] = 2U;
        tcp_tx_buf[21] = 4U;
        tcp_tx_buf[22] = 0x05U;
        tcp_tx_buf[23] = 0x78U;
    }
    if (data != NULL && len > 0U) {
        memcpy(&tcp_tx_buf[hdr_len], data, len);
    }
    ck = net_tcp_checksum(local_ip_be, sk->peer_ip, tcp_tx_buf, seg_len);
    tcp_tx_buf[16] = (uint8_t)((ck >> 8) & 0xFFU);
    tcp_tx_buf[17] = (uint8_t)(ck & 0xFFU);

    return net_send_ipv4(sk->peer_ip, IPPROTO_TCP, tcp_tx_buf, seg_len);
}

static uint32_t tcp_mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x85EBCA6BUL;
    x ^= x >> 13;
    x *= 0xC2B2AE35UL;
    x ^= x >> 16;
    return x;
}

/* RFC 6528-style hardened ISN: monotonic clock + keyed 4-tuple hash + PIT jitter */
static uint32_t tcp_generate_isn(uint32_t loc_ip, uint16_t loc_port,
                                 uint32_t rem_ip, uint16_t rem_port)
{
    struct xhci_net_info ni;
    uint32_t ticks = pit_get_ticks();
    uint32_t pit_cnt;
    uint32_t h;
    uint8_t lo, hi;

    outb(0x43U, 0x00U); /* Latch PIT channel 0 current count for sub-ms jitter */
    lo = inb(0x40U);
    hi = inb(0x40U);
    pit_cnt = ((uint32_t)hi << 8) | (uint32_t)lo;

    tcp_isn_secret = tcp_mix32(tcp_isn_secret ^ ticks ^ (pit_cnt << 16) ^ 0xA5A55A5AUL);
    if (xhci_has_usb_net()) {
        xhci_get_net_info(&ni);
        tcp_isn_secret = tcp_mix32(tcp_isn_secret ^ read_be32(&ni.mac[2]));
    }

    h = tcp_isn_secret;
    h = tcp_mix32(h ^ loc_ip);
    h = tcp_mix32(h ^ rem_ip);
    h = tcp_mix32(h ^ (((uint32_t)loc_port << 16) | (uint32_t)rem_port));
    return h + (ticks * 250UL);
}

static bool_t arp_valid_unicast_mac(const uint8_t mac[6])
{
    int i;
    uint8_t or_all = 0U;

    /* Reject multicast/broadcast MACs (bit 0 of first octet set) */
    if ((mac[0] & 0x01U) != 0U) {
        return FALSE;
    }
    /* Reject all-zero MAC */
    for (i = 0; i < 6; i++) {
        or_all |= mac[i];
    }
    return (or_all != 0U) ? TRUE : FALSE;
}

static bool_t arp_valid_sender_ip(uint32_t ip_be)
{
    uint8_t first = (uint8_t)(ip_be & 0xFFU);
    if (ip_be == 0UL || ip_be == 0xFFFFFFFFUL || ip_be == local_ip_be) {
        return FALSE;
    }
    /* Reject loopback (127/8), multicast (224..239), and reserved (240..255) */
    if (first == 0U || first == 127U || first >= 224U) {
        return FALSE;
    }
    return TRUE;
}

static bool_t arp_cache_has_ip(uint32_t ip_be)
{
    uint32_t now = pit_get_ticks();
    uint32_t i;
    for (i = 0; i < NET_ARP_CACHE_SZ; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip_be &&
            (int32_t)(now - arp_cache[i].expires_tick) < 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static void arp_cache_put(uint32_t ip_be, const uint8_t mac[6])
{
    uint32_t now = pit_get_ticks();
    uint32_t i;
    uint32_t victim = 0U;
    bool_t found_free = FALSE;

    if (!arp_valid_sender_ip(ip_be) || !arp_valid_unicast_mac(mac)) {
        return;
    }

    for (i = 0; i < NET_ARP_CACHE_SZ; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip_be) {
            memcpy(arp_cache[i].mac, mac, 6U);
            arp_cache[i].expires_tick   = now + NET_ARP_TTL_TICKS;
            arp_cache[i].last_used_tick = now;
            return;
        }
    }
    for (i = 0; i < NET_ARP_CACHE_SZ; i++) {
        if (!arp_cache[i].valid || (int32_t)(now - arp_cache[i].expires_tick) >= 0) {
            victim = i;
            found_free = TRUE;
            break;
        }
    }
    if (!found_free) {
        /* LRU eviction: replace least-recently-used entry */
        uint32_t oldest_age = 0U;
        for (i = 0; i < NET_ARP_CACHE_SZ; i++) {
            uint32_t age = now - arp_cache[i].last_used_tick;
            if (age >= oldest_age) {
                oldest_age = age;
                victim = i;
            }
        }
    }
    arp_cache[victim].valid          = TRUE;
    arp_cache[victim].ip             = ip_be;
    arp_cache[victim].expires_tick   = now + NET_ARP_TTL_TICKS;
    arp_cache[victim].last_used_tick = now;
    memcpy(arp_cache[victim].mac, mac, 6U);
}

static bool_t arp_cache_lookup(uint32_t ip_be, uint8_t out_mac[6])
{
    uint32_t now = pit_get_ticks();
    uint32_t i;
    for (i = 0; i < NET_ARP_CACHE_SZ; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip_be) {
            if ((int32_t)(now - arp_cache[i].expires_tick) >= 0) {
                arp_cache[i].valid = FALSE;
                return FALSE;
            }
            arp_cache[i].last_used_tick = now;
            memcpy(out_mac, arp_cache[i].mac, 6U);
            return TRUE;
        }
    }
    return FALSE;
}

static void arp_send_packet(uint16_t oper, const uint8_t dst_mac[6],
                            const uint8_t target_mac[6], uint32_t target_ip_be)
{
    struct xhci_net_info ni;
    uint8_t pkt[42];

    if (!xhci_has_usb_net()) {
        return;
    }
    xhci_get_net_info(&ni);

    memset(pkt, 0, sizeof(pkt));
    memcpy(&pkt[0], dst_mac, 6U);
    memcpy(&pkt[6], ni.mac, 6U);
    pkt[12] = 0x08U;
    pkt[13] = 0x06U;
    pkt[14] = 0x00U;
    pkt[15] = 0x01U;
    pkt[16] = 0x08U;
    pkt[17] = 0x00U;
    pkt[18] = 6U;
    pkt[19] = 4U;
    pkt[20] = (uint8_t)((oper >> 8) & 0xFFU);
    pkt[21] = (uint8_t)(oper & 0xFFU);
    memcpy(&pkt[22], ni.mac, 6U);
    write_be32(&pkt[28], local_ip_be);
    memcpy(&pkt[32], target_mac, 6U);
    write_be32(&pkt[38], target_ip_be);

    (void)xhci_net_send(pkt, 42U);
}

static void net_process_ipv4_payload(uint32_t src_ip, uint32_t dst_ip,
                                     bool_t is_local_dst, bool_t is_bcast_dst,
                                     uint8_t proto, const uint8_t *payload,
                                     uint32_t payload_len)
{
    int i;

    if (proto == IPPROTO_UDP && payload_len >= 8U) {
        const uint8_t *udp = payload;
        uint16_t src_port, dst_port, udp_len;
        memcpy(&src_port, &udp[0], 2U);
        memcpy(&dst_port, &udp[2], 2U);
        udp_len = (uint16_t)(((uint16_t)udp[4] << 8) | (uint16_t)udp[5]);
        if (udp_len < 8U || udp_len > payload_len) {
            return;
        }
        if (!is_local_dst && !is_bcast_dst &&
            !(dhcp_active && net_ntohs(dst_port) == 68U)) {
            return;
        }
        for (i = 0; i < NET_MAX_SOCKETS; i++) {
            struct net_sock *sk = &sock_table[i];
            if (sk->used && sk->type == SOCK_DGRAM && sk->bound && sk->local_port == dst_port) {
                if (!sk->connected ||
                    ((sk->peer_ip == 0U || sk->peer_ip == src_ip) &&
                     (sk->peer_port == 0U || sk->peer_port == src_port))) {
                    sock_enqueue_rx(sk, src_ip, src_port, &udp[8], (uint32_t)udp_len - 8U);
                }
            }
        }
    } else if (proto == IPPROTO_TCP && payload_len >= 20U) {
        const uint8_t *tcp = payload;
        uint16_t src_port, dst_port;
        uint32_t seq, ack, data_off, tcp_data_len;
        uint8_t flags;

        /* TCP must be unicast to our local IP and pass TCP checksum */
        if (!is_local_dst || net_tcp_checksum(src_ip, dst_ip, tcp, payload_len) != 0U) {
            return;
        }

        memcpy(&src_port, &tcp[0], 2U);
        memcpy(&dst_port, &tcp[2], 2U);
        seq = net_ntohl(read_be32(&tcp[4]));
        ack = net_ntohl(read_be32(&tcp[8]));
        data_off = (uint32_t)((tcp[12] >> 4) & 0x0FU) * 4U;
        flags = tcp[13];
        if (data_off < 20U || data_off > payload_len) {
            return;
        }
        tcp_data_len = payload_len - data_off;
        if (tcp_data_len > TCP_RCV_WIN) {
            tcp_data_len = TCP_RCV_WIN;
        }

        for (i = 0; i < NET_MAX_SOCKETS; i++) {
            struct net_sock *sk = &sock_table[i];
            if (!sk->used || sk->type != SOCK_STREAM || !sk->bound ||
                sk->local_port != dst_port || sk->peer_ip != src_ip ||
                sk->peer_port != src_port) {
                continue;
            }
            if (sk->tcp_state == TCP_STATE_SYN_SENT) {
                if ((flags & TCP_RST) != 0U) {
                    /* RFC 793 / RFC 5961: RST in SYN_SENT must ACK our exact SYN */
                    if ((flags & TCP_ACK) != 0U && ack == sk->tcp_snd_nxt) {
                        sk->tcp_rst   = TRUE;
                        sk->tcp_eof   = TRUE;
                        sk->tcp_state = TCP_STATE_CLOSED;
                    }
                    break;
                }
                if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
                    ack == sk->tcp_snd_nxt) {
                    sk->tcp_snd_una = ack;
                    sk->tcp_rcv_nxt = seq + 1U;
                    sk->tcp_state   = TCP_STATE_ESTABLISHED;
                    (void)tcp_send_segment(sk, TCP_ACK, sk->tcp_snd_nxt,
                                           sk->tcp_rcv_nxt, NULL, 0U);
                }
                break;
            }
            if (sk->tcp_state == TCP_STATE_ESTABLISHED ||
                sk->tcp_state == TCP_STATE_CLOSE_WAIT ||
                sk->tcp_state == TCP_STATE_FIN_WAIT) {
                int32_t seq_rel = (int32_t)(seq - sk->tcp_rcv_nxt);
                bool_t need_ack = FALSE;

                /* RFC 5961 §3.2: exact-match RST closes; in-window RST gets challenge ACK */
                if ((flags & TCP_RST) != 0U) {
                    if (seq == sk->tcp_rcv_nxt) {
                        sk->tcp_rst   = TRUE;
                        sk->tcp_eof   = TRUE;
                        sk->tcp_state = TCP_STATE_CLOSED;
                    } else if (seq_rel > 0 && (uint32_t)seq_rel < TCP_RCV_WIN) {
                        (void)tcp_send_segment(sk, TCP_ACK, sk->tcp_snd_nxt,
                                               sk->tcp_rcv_nxt, NULL, 0U);
                    }
                    break;
                }

                /* RFC 5961 §4: reject SYN in synchronized state with challenge ACK */
                if ((flags & TCP_SYN) != 0U) {
                    if (seq_rel >= -(int32_t)TCP_RCV_WIN && seq_rel < (int32_t)TCP_RCV_WIN) {
                        (void)tcp_send_segment(sk, TCP_ACK, sk->tcp_snd_nxt,
                                               sk->tcp_rcv_nxt, NULL, 0U);
                    }
                    break;
                }

                /* Drop segments completely outside our receive window */
                if (seq_rel >= (int32_t)TCP_RCV_WIN ||
                    (int32_t)((seq + tcp_data_len) - sk->tcp_rcv_nxt) < -(int32_t)TCP_RCV_WIN) {
                    break;
                }

                /* RFC 793: only advance SND.UNA if ACK is in (SND.UNA, SND.NXT] */
                if ((flags & TCP_ACK) != 0U &&
                    (int32_t)(ack - sk->tcp_snd_una) > 0 &&
                    (int32_t)(ack - sk->tcp_snd_nxt) <= 0) {
                    sk->tcp_snd_una = ack;
                }

                if (tcp_data_len > 0U) {
                    if (seq == sk->tcp_rcv_nxt) {
                        sock_enqueue_rx(sk, src_ip, src_port,
                                        &tcp[data_off], tcp_data_len);
                        sk->tcp_rcv_nxt += tcp_data_len;
                        need_ack = TRUE;
                    } else if (seq_rel < 0 &&
                               (int32_t)((seq + tcp_data_len) - sk->tcp_rcv_nxt) > 0) {
                        uint32_t skip = sk->tcp_rcv_nxt - seq;
                        uint32_t take = tcp_data_len - skip;
                        sock_enqueue_rx(sk, src_ip, src_port,
                                        &tcp[data_off + skip], take);
                        sk->tcp_rcv_nxt += take;
                        need_ack = TRUE;
                    } else if (seq_rel < 0) {
                        /* Duplicate retransmit of already-received data: re-ACK */
                        need_ack = TRUE;
                    }
                }

                /* Only honor FIN when it arrives exactly in sequence */
                if ((flags & TCP_FIN) != 0U && seq + tcp_data_len == sk->tcp_rcv_nxt) {
                    sk->tcp_rcv_nxt += 1U;
                    sk->tcp_eof = TRUE;
                    if (sk->tcp_state == TCP_STATE_ESTABLISHED) {
                        sk->tcp_state = TCP_STATE_CLOSE_WAIT;
                    } else {
                        sk->tcp_state = TCP_STATE_CLOSED;
                    }
                    need_ack = TRUE;
                }
                if (need_ack) {
                    (void)tcp_send_segment(sk, TCP_ACK, sk->tcp_snd_nxt,
                                           sk->tcp_rcv_nxt, NULL, 0U);
                }
            }
            break;
        }
    } else if (proto == IPPROTO_ICMP && payload_len >= 8U && is_local_dst) {
        const uint8_t *icmp = payload;
        for (i = 0; i < NET_MAX_SOCKETS; i++) {
            struct net_sock *sk = &sock_table[i];
            if (sk->used && sk->type == SOCK_RAW &&
                (sk->protocol == IPPROTO_ICMP || sk->protocol == IPPROTO_IP)) {
                if (!sk->connected || sk->peer_ip == 0U || sk->peer_ip == src_ip) {
                    sock_enqueue_rx(sk, src_ip, 0U, icmp, payload_len);
                }
            }
        }
    }
}

/* RFC 791 / RFC 5722 overlap-safe IPv4 fragment reassembler */
static void net_ipv4_reassemble(uint32_t src_ip, uint32_t dst_ip,
                                bool_t is_local_dst, bool_t is_bcast_dst,
                                uint16_t ip_id, uint8_t proto, uint16_t frag_off,
                                const uint8_t *frag_data, uint32_t frag_len)
{
    uint32_t byte_off = (uint32_t)(frag_off & 0x1FFFU) * 8U;
    bool_t   more_frags = ((frag_off & 0x2000U) != 0U) ? TRUE : FALSE;
    uint32_t now = pit_get_ticks();
    struct net_ipv4_reasm *slot = NULL;
    uint32_t i;
    uint32_t start_blk, num_blks, blk;

    if (frag_len == 0U || byte_off + frag_len > NET_REASM_MAX) {
        return;
    }
    if (more_frags && (frag_len & 7U) != 0U) {
        return; /* Non-final IPv4 fragments must be multiples of 8 bytes */
    }

    for (i = 0; i < NET_REASM_SLOTS; i++) {
        if (reasm_table[i].used && (int32_t)(now - reasm_table[i].expires_tick) >= 0) {
            reasm_table[i].used = FALSE;
        }
        if (reasm_table[i].used &&
            reasm_table[i].src_ip == src_ip &&
            reasm_table[i].dst_ip == dst_ip &&
            reasm_table[i].ip_id  == ip_id  &&
            reasm_table[i].proto  == proto) {
            slot = &reasm_table[i];
            break;
        }
    }

    if (slot == NULL) {
        for (i = 0; i < NET_REASM_SLOTS; i++) {
            if (!reasm_table[i].used) {
                slot = &reasm_table[i];
                break;
            }
        }
        if (slot == NULL) {
            slot = &reasm_table[0];
        }
        memset(slot, 0, sizeof(*slot));
        slot->used         = TRUE;
        slot->proto        = proto;
        slot->ip_id        = ip_id;
        slot->src_ip       = src_ip;
        slot->dst_ip       = dst_ip;
        slot->expires_tick = now + NET_REASM_TTL;
    }

    if (!more_frags) {
        uint32_t end_off = byte_off + frag_len;
        if (slot->have_last && slot->total_len != end_off) {
            slot->used = FALSE;
            return;
        }
        slot->have_last = TRUE;
        slot->total_len = end_off;
    } else if (slot->have_last && byte_off + frag_len > slot->total_len) {
        slot->used = FALSE;
        return;
    }

    start_blk = byte_off / 8U;
    num_blks  = (frag_len + 7U) / 8U;

    /* RFC 5722: reject overlapping fragments and discard the whole datagram */
    for (blk = start_blk; blk < start_blk + num_blks; blk++) {
        if ((slot->block_map[blk / 8U] & (1U << (blk & 7U))) != 0U) {
            slot->used = FALSE;
            return;
        }
    }

    memcpy(&slot->data[byte_off], frag_data, frag_len);
    for (blk = start_blk; blk < start_blk + num_blks; blk++) {
        slot->block_map[blk / 8U] |= (uint8_t)(1U << (blk & 7U));
    }

    if (slot->have_last && slot->total_len > 0U) {
        uint32_t need_blks = (slot->total_len + 7U) / 8U;
        uint32_t deliver_len;
        for (blk = 0; blk < need_blks; blk++) {
            if ((slot->block_map[blk / 8U] & (1U << (blk & 7U))) == 0U) {
                return; /* Still waiting for missing fragment(s) */
            }
        }
        deliver_len = slot->total_len;
        memcpy(reasm_deliver_buf, slot->data, deliver_len);
        slot->used = FALSE;
        net_process_ipv4_payload(src_ip, dst_ip, is_local_dst, is_bcast_dst,
                                 proto, reasm_deliver_buf, deliver_len);
    }
}

static void net_process_frame(const uint8_t *frame, uint32_t len)
{
    static const uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    struct xhci_net_info ni;
    uint16_t ethertype;

    if (len < 14U) {
        return;
    }

    /* Drop Ethernet frames not addressed to our MAC or broadcast */
    if (xhci_has_usb_net()) {
        xhci_get_net_info(&ni);
        if (memcmp(&frame[0], ni.mac, 6U) != 0 &&
            memcmp(&frame[0], bcast_mac, 6U) != 0) {
            return;
        }
    }

    ethertype = (uint16_t)(((uint16_t)frame[12] << 8) | (uint16_t)frame[13]);

    if (ethertype == 0x0806U && len >= 42U) {
        uint16_t htype = (uint16_t)(((uint16_t)frame[14] << 8) | (uint16_t)frame[15]);
        uint16_t ptype = (uint16_t)(((uint16_t)frame[16] << 8) | (uint16_t)frame[17]);
        uint8_t  hlen  = frame[18];
        uint8_t  plen  = frame[19];
        uint16_t oper  = (uint16_t)(((uint16_t)frame[20] << 8) | (uint16_t)frame[21]);
        uint32_t spa   = read_be32(&frame[28]);
        uint32_t tpa   = read_be32(&frame[38]);

        if (htype != 1U || ptype != 0x0800U || hlen != 6U || plen != 4U) {
            return;
        }
        if (!arp_valid_sender_ip(spa) || !arp_valid_unicast_mac(&frame[22])) {
            return;
        }

        if (oper == 1U && tpa == local_ip_be) {
            if ((spa & netmask_be) == (local_ip_be & netmask_be)) {
                arp_cache_put(spa, &frame[22]);
            }
            arp_send_packet(2U, &frame[22], &frame[22], spa);
        } else if (oper == 2U && tpa == local_ip_be) {
            /* Only accept solicited replies or refreshes of existing entries */
            if (spa == arp_pending_ip || arp_cache_has_ip(spa)) {
                arp_cache_put(spa, &frame[22]);
            }
        }
        return;
    }

    if (ethertype == 0x0800U && len >= 34U) {
        uint8_t  ver        = (uint8_t)(frame[14] >> 4);
        uint8_t  ihl        = (uint8_t)((frame[14] & 0x0FU) * 4U);
        uint16_t ip_tot_len = (uint16_t)(((uint16_t)frame[16] << 8) | (uint16_t)frame[17]);
        uint16_t ip_id      = (uint16_t)(((uint16_t)frame[18] << 8) | (uint16_t)frame[19]);
        uint16_t frag_off   = (uint16_t)(((uint16_t)frame[20] << 8) | (uint16_t)frame[21]);
        uint8_t  proto      = frame[23];
        uint32_t src_ip     = read_be32(&frame[26]);
        uint32_t dst_ip     = read_be32(&frame[30]);
        uint32_t subnet_bcast;
        bool_t   is_local_dst;
        bool_t   is_bcast_dst;
        uint32_t payload_off;
        uint32_t payload_len;

        if (ver != 4U || ihl < 20U || 14U + ihl > len || ip_tot_len < ihl) {
            return;
        }
        if (net_checksum16(&frame[14], ihl) != 0U) {
            return; /* Drop corrupted IPv4 header */
        }

        payload_off = 14U + ihl;
        payload_len = (uint32_t)ip_tot_len - ihl;
        if (payload_off + payload_len > len) {
            return; /* Drop truncated IPv4 packet */
        }

        subnet_bcast = (local_ip_be & netmask_be) | (~netmask_be);
        is_local_dst = (dst_ip == local_ip_be) ? TRUE : FALSE;
        is_bcast_dst = (dst_ip == 0xFFFFFFFFUL ||
                        (netmask_be != 0UL && dst_ip == subnet_bcast)) ? TRUE : FALSE;
        if (!is_local_dst && !is_bcast_dst && !dhcp_active) {
            return;
        }

        if ((frag_off & 0x3FFFU) != 0U) {
            net_ipv4_reassemble(src_ip, dst_ip, is_local_dst, is_bcast_dst,
                                ip_id, proto, frag_off,
                                &frame[payload_off], payload_len);
            return;
        }

        net_process_ipv4_payload(src_ip, dst_ip, is_local_dst, is_bcast_dst,
                                 proto, &frame[payload_off], payload_len);
    }
}

void net_poll(void)
{
    int budget = 8;

    if (!xhci_has_usb_net()) {
        return;
    }
    while (budget > 0) {
        int32_t n = xhci_net_recv(net_rx_frame_buf, sizeof(net_rx_frame_buf));
        if (n <= 0) {
            break;
        }
        net_process_frame(net_rx_frame_buf, (uint32_t)n);
        budget--;
    }
}

int net_arp_resolve(uint32_t ip_be, uint8_t out_mac[6])
{
    static const uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static const uint8_t zero_mac[6]  = { 0, 0, 0, 0, 0, 0 };
    int attempt;

    if (arp_cache_lookup(ip_be, out_mac)) {
        return 0;
    }
    if (!xhci_has_usb_net()) {
        return -NET_ENETUNREACH;
    }

    arp_pending_ip = ip_be;
    arp_send_packet(1U, bcast_mac, zero_mac, ip_be);
    for (attempt = 0; attempt < 150; attempt++) {
        net_poll();
        if (arp_cache_lookup(ip_be, out_mac)) {
            arp_pending_ip = 0UL;
            return 0;
        }
        pit_mdelay(5U);
    }
    arp_pending_ip = 0UL;
    return -NET_ENETUNREACH;
}

static int net_send_ipv4(uint32_t dst_ip_be, uint8_t proto,
                         const uint8_t *payload, uint32_t payload_len)
{
    struct xhci_net_info ni;
    uint32_t next_hop;
    uint8_t dst_mac[6];
    uint16_t ip_tot_len;
    uint16_t ck;

    if (!xhci_has_usb_net() || (14U + 20U + payload_len) > sizeof(net_tx_frame_buf)) {
        return -NET_ENETUNREACH;
    }

    if (dst_ip_be == 0xFFFFFFFFUL) {
        memset(dst_mac, 0xFF, 6U);
    } else {
        if ((dst_ip_be & netmask_be) == (local_ip_be & netmask_be)) {
            next_hop = dst_ip_be;
        } else {
            next_hop = gateway_ip_be;
        }

        if (net_arp_resolve(next_hop, dst_mac) < 0) {
            return -NET_ENETUNREACH;
        }
    }

    xhci_get_net_info(&ni);
    ip_tot_len = (uint16_t)(20U + payload_len);

    memcpy(&net_tx_frame_buf[0], dst_mac, 6U);
    memcpy(&net_tx_frame_buf[6], ni.mac, 6U);
    net_tx_frame_buf[12] = 0x08U;
    net_tx_frame_buf[13] = 0x00U;

    memset(&net_tx_frame_buf[14], 0, 20U);
    net_tx_frame_buf[14] = 0x45U;
    net_tx_frame_buf[16] = (uint8_t)((ip_tot_len >> 8) & 0xFFU);
    net_tx_frame_buf[17] = (uint8_t)(ip_tot_len & 0xFFU);
    net_tx_frame_buf[18] = (uint8_t)((ipv4_ident >> 8) & 0xFFU);
    net_tx_frame_buf[19] = (uint8_t)(ipv4_ident & 0xFFU);
    ipv4_ident++;
    net_tx_frame_buf[22] = 64U;
    net_tx_frame_buf[23] = proto;
    write_be32(&net_tx_frame_buf[26], dhcp_zero_src ? 0UL : local_ip_be);
    write_be32(&net_tx_frame_buf[30], dst_ip_be);
    ck = net_checksum16(&net_tx_frame_buf[14], 20U);
    net_tx_frame_buf[24] = (uint8_t)((ck >> 8) & 0xFFU);
    net_tx_frame_buf[25] = (uint8_t)(ck & 0xFFU);

    memcpy(&net_tx_frame_buf[34], payload, payload_len);
    return (xhci_net_send(net_tx_frame_buf, 34U + payload_len) >= 0) ? 0 : -NET_ENETUNREACH;
}

static uint16_t alloc_ephemeral_port(void)
{
    uint16_t port = next_ephemeral_port++;
    if (next_ephemeral_port >= 65500U) {
        next_ephemeral_port = 49152U;
    }
    return net_htons(port);
}

void net_init(void)
{
    memset(sock_table, 0, sizeof(sock_table));
    memset(arp_cache, 0, sizeof(arp_cache));
    memset(reasm_table, 0, sizeof(reasm_table));
    arp_pending_ip      = 0UL;
    dhcp_zero_src       = FALSE;
    dhcp_active         = FALSE;
    local_ip_be         = net_make_ipv4(10, 0, 2, 15);
    netmask_be          = net_make_ipv4(255, 255, 255, 0);
    gateway_ip_be       = net_make_ipv4(10, 0, 2, 2);
    dns_ip_be           = net_make_ipv4(10, 0, 2, 3);
    next_ephemeral_port = 49152U;
    ipv4_ident          = 1U;
    dns_txid            = 1U;
}

bool_t net_is_socket_fd(int fd)
{
    return (get_sock(fd) != NULL) ? TRUE : FALSE;
}

uint32_t net_get_local_ip(void)
{
    return local_ip_be;
}

uint32_t net_get_netmask(void)
{
    return netmask_be;
}

uint32_t net_get_gateway_ip(void)
{
    return gateway_ip_be;
}

uint32_t net_get_dns_ip(void)
{
    return dns_ip_be;
}

void net_set_config(uint32_t ip_be, uint32_t mask_be, uint32_t gw_be, uint32_t dns_be)
{
    local_ip_be   = ip_be;
    netmask_be    = (mask_be != 0UL) ? mask_be : net_make_ipv4(255, 255, 255, 0);
    gateway_ip_be = gw_be;
    if (dns_be != 0UL) {
        dns_ip_be = dns_be;
    } else if (gw_be != 0UL) {
        dns_ip_be = gw_be;
    }
    memset(arp_cache, 0, sizeof(arp_cache));
    memset(reasm_table, 0, sizeof(reasm_table));
}

static void dhcp_parse_options(const uint8_t *pkt, uint32_t len,
                               uint8_t *msg_type, uint32_t *mask_be,
                               uint32_t *gw_be, uint32_t *dns_be, uint32_t *srv_be)
{
    uint32_t pos = 240U;
    while (pos < len) {
        uint8_t opt = pkt[pos++];
        uint8_t olen;
        if (opt == 0U) {
            continue;
        }
        if (opt == 255U || pos >= len) {
            break;
        }
        olen = pkt[pos++];
        if (pos + olen > len) {
            break;
        }
        if (opt == 53U && olen >= 1U) {
            *msg_type = pkt[pos];
        } else if (opt == 1U && olen >= 4U) {
            *mask_be = read_be32(&pkt[pos]);
        } else if (opt == 3U && olen >= 4U) {
            *gw_be = read_be32(&pkt[pos]);
        } else if (opt == 6U && olen >= 4U) {
            *dns_be = read_be32(&pkt[pos]);
        } else if (opt == 54U && olen >= 4U) {
            *srv_be = read_be32(&pkt[pos]);
        }
        pos += olen;
    }
}

int net_dhcp(void)
{
    static uint8_t dhcp_tx[300];
    static uint8_t dhcp_rx[576];
    struct xhci_net_info ni;
    struct sockaddr_in bind_addr;
    struct sockaddr_in bcast_addr;
    struct sockaddr_in src_addr;
    uint32_t src_len;
    uint32_t xid;
    int sock;
    int32_t n;
    int attempt;
    uint32_t offered_ip = 0UL;
    uint32_t server_id  = 0UL;
    uint32_t opt_mask   = 0UL;
    uint32_t opt_gw     = 0UL;
    uint32_t opt_dns    = 0UL;
    bool_t got_offer    = FALSE;
    bool_t got_ack      = FALSE;

    if (!xhci_has_usb_net()) {
        return -NET_ENETUNREACH;
    }
    xhci_get_net_info(&ni);

    sock = net_socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return sock;
    }

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port   = net_htons(68U);
    bind_addr.sin_addr   = 0UL;
    if (net_bind(sock, (const struct sockaddr *)(const void *)&bind_addr,
                 (uint32_t)sizeof(bind_addr)) < 0) {
        (void)net_close(sock);
        return -NET_EINVAL;
    }

    memset(&bcast_addr, 0, sizeof(bcast_addr));
    bcast_addr.sin_family = AF_INET;
    bcast_addr.sin_port   = net_htons(67U);
    bcast_addr.sin_addr   = 0xFFFFFFFFUL;

    xid = 0x4F4B5300UL | (pit_get_ticks() & 0xFFFFUL);
    dhcp_active = TRUE;

    /* 1. DHCPDISCOVER -> wait for DHCPOFFER */
    for (attempt = 0; attempt < 2 && !got_offer; attempt++) {
        int wait;
        memset(dhcp_tx, 0, sizeof(dhcp_tx));
        dhcp_tx[0] = 1U; /* BOOTREQUEST */
        dhcp_tx[1] = 1U; /* Ethernet */
        dhcp_tx[2] = 6U; /* MAC len */
        write_be32(&dhcp_tx[4], net_htonl(xid));
        dhcp_tx[10] = 0x80U; /* Broadcast flag */
        memcpy(&dhcp_tx[28], ni.mac, 6U);
        dhcp_tx[236] = 0x63U; dhcp_tx[237] = 0x82U;
        dhcp_tx[238] = 0x53U; dhcp_tx[239] = 0x63U;
        dhcp_tx[240] = 53U; dhcp_tx[241] = 1U; dhcp_tx[242] = 1U; /* DISCOVER */
        dhcp_tx[243] = 55U; dhcp_tx[244] = 3U; dhcp_tx[245] = 1U; dhcp_tx[246] = 3U; dhcp_tx[247] = 6U;
        dhcp_tx[248] = 255U;

        dhcp_zero_src = TRUE;
        (void)net_sendto(sock, dhcp_tx, (uint32_t)sizeof(dhcp_tx), 0U,
                         (const struct sockaddr *)(const void *)&bcast_addr,
                         (uint32_t)sizeof(bcast_addr));
        dhcp_zero_src = FALSE;

        for (wait = 0; wait < 150 && !got_offer; wait++) {
            src_len = (uint32_t)sizeof(src_addr);
            n = net_recvfrom(sock, dhcp_rx, (uint32_t)sizeof(dhcp_rx), MSG_DONTWAIT,
                             (struct sockaddr *)(void *)&src_addr, &src_len);
            if (n >= 244 && dhcp_rx[0] == 2U &&
                net_ntohl(read_be32(&dhcp_rx[4])) == xid &&
                memcmp(&dhcp_rx[28], ni.mac, 6U) == 0 &&
                dhcp_rx[236] == 0x63U && dhcp_rx[237] == 0x82U &&
                dhcp_rx[238] == 0x53U && dhcp_rx[239] == 0x63U) {
                uint8_t mtype = 0U;
                dhcp_parse_options(dhcp_rx, (uint32_t)n, &mtype,
                                   &opt_mask, &opt_gw, &opt_dns, &server_id);
                if (mtype == 2U) { /* DHCPOFFER */
                    offered_ip = read_be32(&dhcp_rx[16]);
                    if (server_id == 0UL) {
                        server_id = read_be32(&dhcp_rx[20]);
                    }
                    if (server_id == 0UL) {
                        server_id = src_addr.sin_addr;
                    }
                    got_offer = TRUE;
                    break;
                }
            }
            pit_mdelay(10U);
        }
    }

    if (!got_offer || offered_ip == 0UL) {
        dhcp_active = FALSE;
        (void)net_close(sock);
        return -NET_ETIMEDOUT;
    }

    /* 2. DHCPREQUEST -> wait for DHCPACK */
    for (attempt = 0; attempt < 2 && !got_ack; attempt++) {
        int wait;
        uint32_t p = 240U;
        memset(dhcp_tx, 0, sizeof(dhcp_tx));
        dhcp_tx[0] = 1U;
        dhcp_tx[1] = 1U;
        dhcp_tx[2] = 6U;
        write_be32(&dhcp_tx[4], net_htonl(xid));
        dhcp_tx[10] = 0x80U;
        memcpy(&dhcp_tx[28], ni.mac, 6U);
        dhcp_tx[236] = 0x63U; dhcp_tx[237] = 0x82U;
        dhcp_tx[238] = 0x53U; dhcp_tx[239] = 0x63U;
        dhcp_tx[p++] = 53U; dhcp_tx[p++] = 1U; dhcp_tx[p++] = 3U; /* REQUEST */
        dhcp_tx[p++] = 50U; dhcp_tx[p++] = 4U; write_be32(&dhcp_tx[p], offered_ip); p += 4U;
        if (server_id != 0UL) {
            dhcp_tx[p++] = 54U; dhcp_tx[p++] = 4U; write_be32(&dhcp_tx[p], server_id); p += 4U;
        }
        dhcp_tx[p++] = 55U; dhcp_tx[p++] = 3U; dhcp_tx[p++] = 1U; dhcp_tx[p++] = 3U; dhcp_tx[p++] = 6U;
        dhcp_tx[p++] = 255U;

        dhcp_zero_src = TRUE;
        (void)net_sendto(sock, dhcp_tx, (uint32_t)sizeof(dhcp_tx), 0U,
                         (const struct sockaddr *)(const void *)&bcast_addr,
                         (uint32_t)sizeof(bcast_addr));
        dhcp_zero_src = FALSE;

        for (wait = 0; wait < 150 && !got_ack; wait++) {
            src_len = (uint32_t)sizeof(src_addr);
            n = net_recvfrom(sock, dhcp_rx, (uint32_t)sizeof(dhcp_rx), MSG_DONTWAIT,
                             (struct sockaddr *)(void *)&src_addr, &src_len);
            if (n >= 244 && dhcp_rx[0] == 2U &&
                net_ntohl(read_be32(&dhcp_rx[4])) == xid &&
                memcmp(&dhcp_rx[28], ni.mac, 6U) == 0 &&
                dhcp_rx[236] == 0x63U && dhcp_rx[237] == 0x82U &&
                dhcp_rx[238] == 0x53U && dhcp_rx[239] == 0x63U) {
                uint8_t mtype = 0U;
                dhcp_parse_options(dhcp_rx, (uint32_t)n, &mtype,
                                   &opt_mask, &opt_gw, &opt_dns, &server_id);
                if (mtype == 5U) { /* DHCPACK */
                    uint32_t ack_ip = read_be32(&dhcp_rx[16]);
                    if (ack_ip != 0UL) {
                        offered_ip = ack_ip;
                    }
                    got_ack = TRUE;
                    break;
                }
            }
            pit_mdelay(10U);
        }
    }

    dhcp_active = FALSE;
    (void)net_close(sock);
    if (!got_ack) {
        return -NET_ETIMEDOUT;
    }

    if (opt_gw == 0UL) {
        opt_gw = server_id;
    }
    net_set_config(offered_ip, opt_mask, opt_gw, opt_dns);
    return 0;
}

int net_socket(int domain, int type, int protocol)
{
    int base_type = type & 0x0F; /* Ignore SOCK_NONBLOCK / SOCK_CLOEXEC bits */
    int i;

    if (domain != AF_INET) {
        return -NET_EAFNOSUPPORT;
    }
    if (base_type != SOCK_STREAM && base_type != SOCK_DGRAM && base_type != SOCK_RAW) {
        return -NET_EPROTONOSUPP;
    }
    if (protocol == 0) {
        if (base_type == SOCK_DGRAM) {
            protocol = IPPROTO_UDP;
        } else if (base_type == SOCK_STREAM) {
            protocol = IPPROTO_TCP;
        } else {
            protocol = IPPROTO_ICMP;
        }
    }

    for (i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!sock_table[i].used) {
            memset(&sock_table[i], 0, sizeof(sock_table[i]));
            sock_table[i].used     = TRUE;
            sock_table[i].domain   = domain;
            sock_table[i].type     = base_type;
            sock_table[i].protocol = protocol;
            return NET_SOCK_FD_BASE + i;
        }
    }
    return -NET_EMFILE;
}

int net_bind(int sockfd, const struct sockaddr *addr, uint32_t addrlen)
{
    struct net_sock *sk = get_sock(sockfd);
    const struct sockaddr_in *sin;
    int i;

    if (sk == NULL) {
        return -NET_EBADF;
    }
    if (addr == NULL) {
        return -NET_EFAULT;
    }
    if (addrlen < (uint32_t)sizeof(struct sockaddr_in)) {
        return -NET_EINVAL;
    }
    sin = (const struct sockaddr_in *)(const void *)addr;
    if (sin->sin_family != AF_INET) {
        return -NET_EAFNOSUPPORT;
    }

    if (sin->sin_port != 0U) {
        for (i = 0; i < NET_MAX_SOCKETS; i++) {
            if (sock_table[i].used && &sock_table[i] != sk &&
                sock_table[i].bound && sock_table[i].type == sk->type &&
                sock_table[i].local_port == sin->sin_port) {
                return -NET_EADDRINUSE;
            }
        }
    }

    sk->local_ip   = sin->sin_addr;
    sk->local_port = (sin->sin_port != 0U) ? sin->sin_port : alloc_ephemeral_port();
    sk->bound      = TRUE;
    return 0;
}

int net_connect(int sockfd, const struct sockaddr *addr, uint32_t addrlen)
{
    struct net_sock *sk = get_sock(sockfd);
    const struct sockaddr_in *sin;

    if (sk == NULL) {
        return -NET_EBADF;
    }
    if (addr == NULL) {
        return -NET_EFAULT;
    }
    if (addrlen < (uint32_t)sizeof(struct sockaddr_in)) {
        return -NET_EINVAL;
    }
    sin = (const struct sockaddr_in *)(const void *)addr;
    if (sin->sin_family != AF_INET) {
        return -NET_EAFNOSUPPORT;
    }

    if (!sk->bound) {
        sk->local_ip   = is_loopback_ip(sin->sin_addr) ? sin->sin_addr : local_ip_be;
        sk->local_port = alloc_ephemeral_port();
        sk->bound      = TRUE;
    }
    sk->peer_ip   = sin->sin_addr;
    sk->peer_port = sin->sin_port;
    sk->connected = TRUE;

    if (sk->type == SOCK_STREAM && !is_loopback_ip(sin->sin_addr)) {
        uint32_t syn_seq = tcp_generate_isn(sk->local_ip, sk->local_port,
                                            sk->peer_ip, sk->peer_port);
        int wait;

        sk->tcp_snd_una = syn_seq;
        sk->tcp_snd_nxt = syn_seq + 1U;
        sk->tcp_rcv_nxt = 0U;
        sk->tcp_eof     = FALSE;
        sk->tcp_rst     = FALSE;
        sk->tcp_state   = TCP_STATE_SYN_SENT;

        if (tcp_send_segment(sk, TCP_SYN, syn_seq, 0U, NULL, 0U) < 0) {
            sk->connected = FALSE;
            sk->tcp_state = TCP_STATE_CLOSED;
            return -NET_ENETUNREACH;
        }

        for (wait = 0; wait < 300; wait++) {
            net_poll();
            if (sk->tcp_state == TCP_STATE_ESTABLISHED) {
                return 0;
            }
            if (sk->tcp_rst) {
                sk->connected = FALSE;
                sk->tcp_state = TCP_STATE_CLOSED;
                return -NET_ECONNREFUSED;
            }
            if (wait == 100) {
                (void)tcp_send_segment(sk, TCP_SYN, syn_seq, 0U, NULL, 0U);
            }
            pit_mdelay(10U);
        }

        sk->connected = FALSE;
        sk->tcp_state = TCP_STATE_CLOSED;
        return -NET_ETIMEDOUT;
    }
    return 0;
}

int32_t net_sendto(int sockfd, const void *buf, uint32_t len, uint32_t flags,
                   const struct sockaddr *dest_addr, uint32_t addrlen)
{
    struct net_sock *sk = get_sock(sockfd);
    uint32_t dst_ip;
    uint16_t dst_port;
    uint32_t src_ip;

    (void)flags;
    if (sk == NULL) {
        return -NET_EBADF;
    }
    if (buf == NULL && len > 0U) {
        return -NET_EFAULT;
    }
    if (len > NET_SOCK_RX_MAX) {
        len = NET_SOCK_RX_MAX;
    }

    if (dest_addr != NULL) {
        const struct sockaddr_in *sin;
        if (addrlen < (uint32_t)sizeof(struct sockaddr_in)) {
            return -NET_EINVAL;
        }
        sin = (const struct sockaddr_in *)(const void *)dest_addr;
        if (sin->sin_family != AF_INET) {
            return -NET_EAFNOSUPPORT;
        }
        dst_ip   = sin->sin_addr;
        dst_port = sin->sin_port;
    } else if (sk->connected) {
        dst_ip   = sk->peer_ip;
        dst_port = sk->peer_port;
    } else {
        return -NET_EDESTADDRREQ;
    }

    if (!sk->bound) {
        sk->local_ip   = is_loopback_ip(dst_ip) ? dst_ip : local_ip_be;
        sk->local_port = alloc_ephemeral_port();
        sk->bound      = TRUE;
    }
    src_ip = (sk->local_ip != 0U) ? sk->local_ip : local_ip_be;

    /* Loopback delivery (127.0.0.1 or own IP) */
    if (is_loopback_ip(dst_ip)) {
        if (sk->type == SOCK_RAW && sk->protocol == IPPROTO_ICMP && len >= 8U) {
            uint8_t reply[NET_SOCK_RX_MAX];
            uint16_t ck;
            memcpy(reply, buf, len);
            if (reply[0] == 8U) {
                reply[0] = 0U;
                reply[2] = 0U;
                reply[3] = 0U;
                ck = net_checksum16(reply, len);
                reply[2] = (uint8_t)((ck >> 8) & 0xFFU);
                reply[3] = (uint8_t)(ck & 0xFFU);
                sock_enqueue_rx(sk, dst_ip, 0U, reply, len);
            }
            return (int32_t)len;
        } else {
            int i;
            for (i = 0; i < NET_MAX_SOCKETS; i++) {
                struct net_sock *dst = &sock_table[i];
                if (dst->used && dst->bound && dst->type == sk->type &&
                    dst->local_port == dst_port) {
                    sock_enqueue_rx(dst, src_ip, sk->local_port, (const uint8_t *)buf, len);
                    return (int32_t)len;
                }
            }
            return (int32_t)len;
        }
    }

    /* External IPv4 packet transmission over USB Ethernet (RNDIS / CDC-ECM) */
    if (sk->type == SOCK_RAW && sk->protocol == IPPROTO_ICMP) {
        int rc = net_send_ipv4(dst_ip, IPPROTO_ICMP, (const uint8_t *)buf, len);
        return (rc == 0) ? (int32_t)len : rc;
    }

    if (sk->type == SOCK_DGRAM) {
        uint8_t udp_buf[1520];
        uint16_t udp_len = (uint16_t)(8U + len);
        int rc;
        if (udp_len > sizeof(udp_buf)) {
            return -NET_EINVAL;
        }
        memcpy(&udp_buf[0], &sk->local_port, 2U);
        memcpy(&udp_buf[2], &dst_port, 2U);
        udp_buf[4] = (uint8_t)((udp_len >> 8) & 0xFFU);
        udp_buf[5] = (uint8_t)(udp_len & 0xFFU);
        udp_buf[6] = 0U;
        udp_buf[7] = 0U;
        if (len > 0U) {
            memcpy(&udp_buf[8], buf, len);
        }
        rc = net_send_ipv4(dst_ip, IPPROTO_UDP, udp_buf, udp_len);
        return (rc == 0) ? (int32_t)len : rc;
    }

    if (sk->type == SOCK_STREAM) {
        const uint8_t *p = (const uint8_t *)buf;
        uint32_t sent = 0U;

        if (sk->tcp_rst || (sk->tcp_state != TCP_STATE_ESTABLISHED &&
                            sk->tcp_state != TCP_STATE_CLOSE_WAIT)) {
            return -NET_EPIPE;
        }
        while (sent < len) {
            uint32_t chunk = len - sent;
            uint32_t seg_seq = sk->tcp_snd_nxt;
            uint32_t exp_ack;
            int wait;
            bool_t acked = FALSE;

            if (chunk > 1024U) {
                chunk = 1024U;
            }
            exp_ack = seg_seq + chunk;
            if (tcp_send_segment(sk, (uint8_t)(TCP_PSH | TCP_ACK), seg_seq,
                                 sk->tcp_rcv_nxt, p + sent, chunk) < 0) {
                return (sent > 0U) ? (int32_t)sent : -NET_ENETUNREACH;
            }
            sk->tcp_snd_nxt = exp_ack;

            for (wait = 0; wait < 150; wait++) {
                net_poll();
                if (sk->tcp_rst) {
                    return (sent > 0U) ? (int32_t)sent : -NET_EPIPE;
                }
                if ((int32_t)(sk->tcp_snd_una - exp_ack) >= 0) {
                    acked = TRUE;
                    break;
                }
                if (wait == 50) {
                    (void)tcp_send_segment(sk, (uint8_t)(TCP_PSH | TCP_ACK), seg_seq,
                                           sk->tcp_rcv_nxt, p + sent, chunk);
                }
                pit_mdelay(10U);
            }
            if (!acked) {
                return (sent > 0U) ? (int32_t)sent : -NET_ETIMEDOUT;
            }
            sent += chunk;
        }
        return (int32_t)sent;
    }

    return -NET_ENETUNREACH;
}

int32_t net_send(int sockfd, const void *buf, uint32_t len, uint32_t flags)
{
    struct net_sock *sk = get_sock(sockfd);
    if (sk == NULL) {
        return -NET_EBADF;
    }
    if (!sk->connected) {
        return -NET_ENOTCONN;
    }
    return net_sendto(sockfd, buf, len, flags, NULL, 0U);
}

int32_t net_recvfrom(int sockfd, void *buf, uint32_t len, uint32_t flags,
                     struct sockaddr *src_addr, uint32_t *addrlen)
{
    struct net_sock *sk = get_sock(sockfd);
    struct net_rx_pkt *pkt;
    uint32_t copy_len;

    if (sk == NULL) {
        return -NET_EBADF;
    }
    if (buf == NULL && len > 0U) {
        return -NET_EFAULT;
    }

    net_poll();
    if (sk->rx_tail == sk->rx_head && (flags & MSG_DONTWAIT) == 0U &&
        !(sk->type == SOCK_STREAM && (sk->tcp_eof || sk->tcp_rst))) {
        int wait;
        for (wait = 0; wait < 200; wait++) {
            net_poll();
            if (sk->rx_tail != sk->rx_head ||
                (sk->type == SOCK_STREAM && (sk->tcp_eof || sk->tcp_rst))) {
                break;
            }
            pit_mdelay(5U);
        }
    }

    if (sk->rx_tail == sk->rx_head) {
        if (sk->type == SOCK_STREAM && (sk->tcp_eof || sk->tcp_rst)) {
            return 0;
        }
        return -NET_EAGAIN;
    }

    pkt = &sk->rx_q[sk->rx_tail];
    copy_len = (uint32_t)(pkt->len - pkt->off);
    if (copy_len > len) {
        copy_len = len;
    }
    if (copy_len > 0U) {
        memcpy(buf, &pkt->data[pkt->off], copy_len);
    }

    if (src_addr != NULL && addrlen != NULL && *addrlen >= (uint32_t)sizeof(struct sockaddr_in)) {
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port   = pkt->src_port;
        sin.sin_addr   = pkt->src_ip;
        memcpy(src_addr, &sin, sizeof(sin));
        *addrlen = (uint32_t)sizeof(sin);
    }

    if (sk->type == SOCK_STREAM) {
        pkt->off = (uint16_t)(pkt->off + copy_len);
        if (pkt->off >= pkt->len) {
            sk->rx_tail = (sk->rx_tail + 1U) % NET_SOCK_RX_SLOTS;
        }
    } else {
        sk->rx_tail = (sk->rx_tail + 1U) % NET_SOCK_RX_SLOTS;
    }
    return (int32_t)copy_len;
}

int32_t net_recv(int sockfd, void *buf, uint32_t len, uint32_t flags)
{
    return net_recvfrom(sockfd, buf, len, flags, NULL, NULL);
}

int net_getsockname(int sockfd, struct sockaddr *addr, uint32_t *addrlen)
{
    struct net_sock *sk = get_sock(sockfd);
    struct sockaddr_in sin;

    if (sk == NULL) {
        return -NET_EBADF;
    }
    if (addr == NULL || addrlen == NULL) {
        return -NET_EFAULT;
    }
    if (*addrlen < (uint32_t)sizeof(sin)) {
        return -NET_EINVAL;
    }
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port   = sk->local_port;
    sin.sin_addr   = sk->local_ip;
    memcpy(addr, &sin, sizeof(sin));
    *addrlen = (uint32_t)sizeof(sin);
    return 0;
}

int net_getpeername(int sockfd, struct sockaddr *addr, uint32_t *addrlen)
{
    struct net_sock *sk = get_sock(sockfd);
    struct sockaddr_in sin;

    if (sk == NULL) {
        return -NET_EBADF;
    }
    if (!sk->connected) {
        return -NET_ENOTCONN;
    }
    if (addr == NULL || addrlen == NULL) {
        return -NET_EFAULT;
    }
    if (*addrlen < (uint32_t)sizeof(sin)) {
        return -NET_EINVAL;
    }
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port   = sk->peer_port;
    sin.sin_addr   = sk->peer_ip;
    memcpy(addr, &sin, sizeof(sin));
    *addrlen = (uint32_t)sizeof(sin);
    return 0;
}

int net_close(int sockfd)
{
    struct net_sock *sk = get_sock(sockfd);
    if (sk == NULL) {
        return -NET_EBADF;
    }
    if (sk->type == SOCK_STREAM && sk->connected && !is_loopback_ip(sk->peer_ip) &&
        (sk->tcp_state == TCP_STATE_ESTABLISHED || sk->tcp_state == TCP_STATE_CLOSE_WAIT)) {
        int wait;
        (void)tcp_send_segment(sk, (uint8_t)(TCP_FIN | TCP_ACK), sk->tcp_snd_nxt,
                               sk->tcp_rcv_nxt, NULL, 0U);
        sk->tcp_snd_nxt++;
        sk->tcp_state = TCP_STATE_FIN_WAIT;
        for (wait = 0; wait < 20; wait++) {
            net_poll();
            if (sk->tcp_state == TCP_STATE_CLOSED) {
                break;
            }
            pit_mdelay(5U);
        }
    }
    memset(sk, 0, sizeof(*sk));
    return 0;
}

int net_parse_ipv4(const char *s, uint32_t *out_ip_be)
{
    uint8_t oct[4];
    int part = 0;
    const char *p = s;

    if (s == NULL || out_ip_be == NULL || *s == '\0') {
        return -1;
    }

    while (part < 4) {
        uint32_t val = 0U;
        int digits = 0;

        while (*p >= '0' && *p <= '9') {
            val = val * 10U + (uint32_t)(*p - '0');
            if (val > 255U) {
                return -1;
            }
            digits++;
            p++;
        }
        if (digits == 0) {
            return -1;
        }
        oct[part++] = (uint8_t)val;
        if (part < 4) {
            if (*p != '.') {
                return -1;
            }
            p++;
        }
    }
    if (*p != '\0') {
        return -1;
    }
    *out_ip_be = net_make_ipv4(oct[0], oct[1], oct[2], oct[3]);
    return 0;
}

static uint32_t dns_skip_name(const uint8_t *msg, uint32_t len, uint32_t pos)
{
    int guard = 0;
    while (pos < len && guard < 64) {
        uint8_t c = msg[pos];
        if (c == 0U) {
            return pos + 1U;
        }
        if ((c & 0xC0U) == 0xC0U) {
            return (pos + 2U <= len) ? (pos + 2U) : len;
        }
        pos += 1U + (uint32_t)c;
        guard++;
    }
    return len;
}

int net_resolve_host(const char *host, uint32_t *out_ip_be)
{
    uint8_t qbuf[256];
    uint8_t rbuf[512];
    struct sockaddr_in dst;
    uint32_t qpos;
    const char *p;
    uint16_t txid;
    int sock;
    int32_t n;
    uint16_t ancount;
    uint32_t rpos;
    uint16_t i;

    if (host == NULL || out_ip_be == NULL || host[0] == '\0') {
        return -1;
    }
    if (net_parse_ipv4(host, out_ip_be) == 0) {
        return 0;
    }
    if (strcmp(host, "localhost") == 0) {
        *out_ip_be = net_make_ipv4(127, 0, 0, 1);
        return 0;
    }
    if (!xhci_has_usb_net()) {
        return -1;
    }

    txid = dns_txid++;
    memset(qbuf, 0, 12U);
    qbuf[0] = (uint8_t)((txid >> 8) & 0xFFU);
    qbuf[1] = (uint8_t)(txid & 0xFFU);
    qbuf[2] = 0x01U; /* RD (Recursion Desired) */
    qbuf[5] = 0x01U; /* QDCOUNT = 1 */
    qpos = 12U;

    p = host;
    while (*p != '\0') {
        const char *dot = p;
        uint32_t llen;
        while (*dot != '\0' && *dot != '.') {
            dot++;
        }
        llen = (uint32_t)(dot - p);
        if (llen == 0U || llen > 63U || qpos + 1U + llen + 5U >= sizeof(qbuf)) {
            return -1;
        }
        qbuf[qpos++] = (uint8_t)llen;
        memcpy(&qbuf[qpos], p, llen);
        qpos += llen;
        p = (*dot == '.') ? (dot + 1) : dot;
    }
    qbuf[qpos++] = 0U;
    qbuf[qpos++] = 0U; qbuf[qpos++] = 1U; /* QTYPE = A */
    qbuf[qpos++] = 0U; qbuf[qpos++] = 1U; /* QCLASS = IN */

    sock = net_socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = net_htons(53U);
    dst.sin_addr   = dns_ip_be;

    if (net_sendto(sock, qbuf, qpos, 0U,
                   (const struct sockaddr *)(const void *)&dst,
                   (uint32_t)sizeof(dst)) < 0) {
        (void)net_close(sock);
        return -1;
    }

    n = net_recv(sock, rbuf, (uint32_t)sizeof(rbuf), 0U);
    (void)net_close(sock);
    if (n < 12) {
        return -1;
    }
    if (rbuf[0] != (uint8_t)((txid >> 8) & 0xFFU) ||
        rbuf[1] != (uint8_t)(txid & 0xFFU) ||
        (rbuf[2] & 0x80U) == 0U || (rbuf[3] & 0x0FU) != 0U) {
        return -1;
    }

    ancount = (uint16_t)(((uint16_t)rbuf[6] << 8) | (uint16_t)rbuf[7]);
    rpos = dns_skip_name(rbuf, (uint32_t)n, 12U);
    if (rpos + 4U > (uint32_t)n) {
        return -1;
    }
    rpos += 4U; /* Skip QTYPE + QCLASS */

    for (i = 0; i < ancount && rpos < (uint32_t)n; i++) {
        uint16_t rtype, rclass, rdlen;
        rpos = dns_skip_name(rbuf, (uint32_t)n, rpos);
        if (rpos + 10U > (uint32_t)n) {
            break;
        }
        rtype  = (uint16_t)(((uint16_t)rbuf[rpos] << 8) | (uint16_t)rbuf[rpos + 1U]);
        rclass = (uint16_t)(((uint16_t)rbuf[rpos + 2U] << 8) | (uint16_t)rbuf[rpos + 3U]);
        rdlen  = (uint16_t)(((uint16_t)rbuf[rpos + 8U] << 8) | (uint16_t)rbuf[rpos + 9U]);
        rpos += 10U;
        if (rpos + rdlen > (uint32_t)n) {
            break;
        }
        if (rtype == 1U && rclass == 1U && rdlen == 4U) {
            *out_ip_be = read_be32(&rbuf[rpos]);
            return 0;
        }
        rpos += rdlen;
    }
    return -1;
}
