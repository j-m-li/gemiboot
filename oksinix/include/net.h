/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - BSD-Style Sockets & IPv4/ARP/UDP/ICMP Stack
 */

#ifndef OKSINIX_NET_H
#define OKSINIX_NET_H

#include "types.h"

/* BSD socket address families */
#define AF_INET          2

/* BSD socket types */
#define SOCK_STREAM      1
#define SOCK_DGRAM       2
#define SOCK_RAW         3

/* IP protocols */
#define IPPROTO_IP       0
#define IPPROTO_ICMP     1
#define IPPROTO_TCP      6
#define IPPROTO_UDP      17

/* Socket flags */
#define MSG_PEEK         0x02U
#define MSG_DONTWAIT     0x40U

/* Socket descriptor table (disjoint from VFS fds 0..31) */
#define NET_MAX_SOCKETS  8
#define NET_SOCK_FD_BASE 64

struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;      /* network byte order (big-endian) */
    uint32_t sin_addr;      /* network byte order (big-endian) */
    uint8_t  sin_zero[8];
};

uint16_t net_htons(uint16_t v);
uint16_t net_ntohs(uint16_t v);
uint32_t net_htonl(uint32_t v);
uint32_t net_ntohl(uint32_t v);
uint32_t net_make_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);

void     net_init(void);
void     net_poll(void);
bool_t   net_is_socket_fd(int fd);
uint32_t net_get_local_ip(void);
uint32_t net_get_netmask(void);
uint32_t net_get_gateway_ip(void);
uint32_t net_get_dns_ip(void);
void     net_set_config(uint32_t ip_be, uint32_t mask_be, uint32_t gw_be, uint32_t dns_be);
int      net_dhcp(void);
int      net_arp_resolve(uint32_t ip_be, uint8_t out_mac[6]);
int      net_parse_ipv4(const char *s, uint32_t *out_ip_be);
int      net_resolve_host(const char *host, uint32_t *out_ip_be);

/* BSD-style socket primitives (return >= 0 or negative Linux errno) */
int      net_socket(int domain, int type, int protocol);
int      net_bind(int sockfd, const struct sockaddr *addr, uint32_t addrlen);
int      net_connect(int sockfd, const struct sockaddr *addr, uint32_t addrlen);
int32_t  net_sendto(int sockfd, const void *buf, uint32_t len, uint32_t flags,
                    const struct sockaddr *dest_addr, uint32_t addrlen);
int32_t  net_send(int sockfd, const void *buf, uint32_t len, uint32_t flags);
int32_t  net_recvfrom(int sockfd, void *buf, uint32_t len, uint32_t flags,
                      struct sockaddr *src_addr, uint32_t *addrlen);
int32_t  net_recv(int sockfd, void *buf, uint32_t len, uint32_t flags);
int      net_getsockname(int sockfd, struct sockaddr *addr, uint32_t *addrlen);
int      net_getpeername(int sockfd, struct sockaddr *addr, uint32_t *addrlen);
int      net_close(int sockfd);
void     net_set_socket_owner(int sockfd, int pid);
void     net_close_pid_sockets(int pid);
void     net_close_all_user_sockets(void);

#endif /* OKSINIX_NET_H */
