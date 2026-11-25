/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - USB RNDIS & CDC-ECM Network Class Driver
 * Supports real RNDIS hardware (Android tethering, Linux g_ether/f_rndis, 4G/LTE modems)
 * as well as QEMU usb-net and CDC-ECM USB Ethernet adapters.
 */

#include "xhci_internal.h"
#include "pit.h"
#include "serial.h"
#include "string.h"

/* RNDIS 1.0 Message Types */
#define RNDIS_MSG_PACKET        0x00000001UL
#define RNDIS_MSG_INIT          0x00000002UL
#define RNDIS_MSG_INIT_C        0x80000002UL
#define RNDIS_MSG_HALT          0x00000003UL
#define RNDIS_MSG_QUERY         0x00000004UL
#define RNDIS_MSG_QUERY_C       0x80000004UL
#define RNDIS_MSG_SET           0x00000005UL
#define RNDIS_MSG_SET_C         0x80000005UL
#define RNDIS_MSG_RESET         0x00000006UL
#define RNDIS_MSG_RESET_C       0x80000006UL
#define RNDIS_MSG_INDICATE      0x00000007UL
#define RNDIS_MSG_KEEPALIVE     0x00000008UL
#define RNDIS_MSG_KEEPALIVE_C   0x80000008UL

/* RNDIS Status Indications */
#define RNDIS_STATUS_MEDIA_CONN 0x4001000BUL
#define RNDIS_STATUS_MEDIA_DISC 0x4001000CUL

/* RNDIS OIDs and Packet Filter Bits */
#define RNDIS_OID_MAX_FRAME_SZ  0x00010106UL
#define RNDIS_OID_PACKET_FILTER 0x0001010EUL
#define RNDIS_OID_MEDIA_STATUS  0x00010114UL
#define RNDIS_OID_PERM_ADDR     0x01010101UL
#define RNDIS_OID_CURR_ADDR     0x01010102UL
#define RNDIS_FILTER_DEFAULT    0x0000000FUL /* DIRECTED | MULTICAST | ALL_MULTICAST | BROADCAST */
#define RNDIS_FILTER_BASIC      0x0000000BUL /* DIRECTED | MULTICAST | BROADCAST */
#define RNDIS_HDR_LEN           44U

/* CDC-ECM / CDC Class Requests */
#define CDC_REQ_SEND_ENCAP_CMD  0x00U
#define CDC_REQ_GET_ENCAP_RESP  0x01U
#define CDC_ECM_SET_PKT_FILTER  0x43U
#define CDC_ECM_FILTER_DEFAULT  0x000EU      /* DIRECTED | BROADCAST | ALL_MULTICAST */

/* DMA and RX Ring Sizing (16 KiB DMA supports real-hardware RNDIS packet aggregation) */
#define USBNET_DMA_BUF_SIZE     16384U
#define USBNET_ETH_MIN_LEN      14U
#define USBNET_ETH_MAX_LEN      1518U
#define USBNET_RX_RING_SLOTS    16U

static struct xhci_usb_dev *net_dev            = NULL;
static struct xhci_net_info net_info;
static uint8_t             *net_rx_dma         = NULL;
static uint8_t             *net_tx_dma         = NULL;
static uint8_t             *net_int_dma        = NULL;
static uint32_t             rndis_req_id       = 1U;
static uint32_t             rndis_rx_xfer_size = 2048U;
static uint32_t             rndis_tx_max_size  = 2048U;
static uint32_t             rndis_pkt_align    = 1U;
static uint32_t             rx_armed_len       = 2048U;
static volatile bool_t      rndis_resp_pending = FALSE;
static bool_t               rndis_link_up      = TRUE;

/* Received Ethernet frame ring buffer */
static uint8_t  rx_ring_data[USBNET_RX_RING_SLOTS][USBNET_ETH_MAX_LEN];
static uint16_t rx_ring_len[USBNET_RX_RING_SLOTS];
static uint32_t rx_ring_head = 0;
static uint32_t rx_ring_tail = 0;

static uint32_t net_read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void net_write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return (int)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (int)(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return (int)(c - 'A' + 10);
    }
    return -1;
}

static void usbnet_rx_push(const uint8_t *frame, uint32_t len)
{
    uint32_t next_head;

    if (len < USBNET_ETH_MIN_LEN || len > USBNET_ETH_MAX_LEN) {
        return;
    }
    next_head = (rx_ring_head + 1U) % USBNET_RX_RING_SLOTS;
    if (next_head == rx_ring_tail) {
        /* Ring full: drop oldest frame */
        rx_ring_tail = (rx_ring_tail + 1U) % USBNET_RX_RING_SLOTS;
    }
    memcpy(rx_ring_data[rx_ring_head], frame, len);
    rx_ring_len[rx_ring_head] = (uint16_t)len;
    rx_ring_head = next_head;
    net_info.rx_packets++;
    net_info.rx_bytes += len;
}

static void usbnet_arm_rx(struct xhci_usb_dev *dev)
{
    if (dev != NULL && dev->bulk_in_dci != 0 && net_rx_dma != NULL) {
        uint32_t xfer = (net_info.protocol == XHCI_NET_PROTO_RNDIS) ? rndis_rx_xfer_size : 2048U;
        if (xfer < 2048U) {
            xfer = 2048U;
        }
        if (xfer > USBNET_DMA_BUF_SIZE) {
            xfer = USBNET_DMA_BUF_SIZE;
        }
        rx_armed_len = xfer;
        xhci_bulk_in_submit(dev, net_rx_dma, xfer);
    }
}

static void usbnet_arm_int(struct xhci_usb_dev *dev)
{
    if (dev != NULL && dev->int_in_dci != 0 && net_int_dma != NULL) {
        uint32_t len = (dev->int_in_mps > 0 && dev->int_in_mps <= 64U) ? dev->int_in_mps : 16U;
        memset(net_int_dma, 0, len);
        xhci_interrupt_in_submit(dev, net_int_dma, len);
    }
}

static void rndis_handle_unsolicited(struct xhci_usb_dev *dev, const uint8_t *msg, uint32_t msg_len)
{
    uint32_t mtype;

    if (msg_len < 12U) {
        return;
    }
    mtype = net_read_le32(&msg[0]);
    if (mtype == RNDIS_MSG_KEEPALIVE) {
        /* Real RNDIS hardware expects RNDIS_MSG_KEEPALIVE_C or it resets the link */
        uint8_t reply[16];
        uint32_t req = net_read_le32(&msg[8]);
        net_write_le32(&reply[0],  RNDIS_MSG_KEEPALIVE_C);
        net_write_le32(&reply[4],  16U);
        net_write_le32(&reply[8],  req);
        net_write_le32(&reply[12], 0U);
        (void)xhci_control_transfer(dev, 0x21U, CDC_REQ_SEND_ENCAP_CMD,
                                    0U, dev->iface_num, (uint16_t)sizeof(reply), reply);
    } else if (mtype == RNDIS_MSG_INDICATE) {
        uint32_t status = net_read_le32(&msg[8]);
        if (status == RNDIS_STATUS_MEDIA_CONN) {
            rndis_link_up = TRUE;
            kprintf("[USBNET] RNDIS link UP\n");
        } else if (status == RNDIS_STATUS_MEDIA_DISC) {
            rndis_link_up = FALSE;
            kprintf("[USBNET] RNDIS link DOWN\n");
        }
    }
}

static void rndis_drain_notifications(void)
{
    uint8_t resp[128];
    int budget = 4;

    if (net_dev == NULL || net_info.protocol != XHCI_NET_PROTO_RNDIS) {
        rndis_resp_pending = FALSE;
        return;
    }

    rndis_resp_pending = FALSE;
    while (budget > 0) {
        uint32_t rtype, rlen;
        memset(resp, 0, sizeof(resp));
        if (xhci_control_transfer(net_dev, 0xA1U, CDC_REQ_GET_ENCAP_RESP,
                                  0U, net_dev->iface_num, (uint16_t)sizeof(resp), resp) < 0) {
            break;
        }
        rtype = net_read_le32(&resp[0]);
        rlen  = net_read_le32(&resp[4]);
        if (rtype == 0U || rlen < 12U) {
            break;
        }
        rndis_handle_unsolicited(net_dev, resp, rlen);
        budget--;
    }
}

static int rndis_send_cmd(struct xhci_usb_dev *dev,
                          const uint8_t *cmd, uint16_t cmd_len,
                          uint8_t *resp, uint16_t resp_max,
                          uint32_t expect_type, uint32_t expect_req_id)
{
    int attempt;

    if (xhci_control_transfer(dev, 0x21U, CDC_REQ_SEND_ENCAP_CMD,
                              0U, dev->iface_num, cmd_len, (void *)(uintptr_t)cmd) < 0) {
        return -1;
    }

    /* Real RNDIS devices (Android/LTE modems) may take up to 1s and STALL or return 0x00 while busy */
    for (attempt = 0; attempt < 100; attempt++) {
        xhci_poll();
        memset(resp, 0, resp_max);
        if (xhci_control_transfer(dev, 0xA1U, CDC_REQ_GET_ENCAP_RESP,
                                  0U, dev->iface_num, resp_max, resp) == 0) {
            uint32_t rtype = net_read_le32(&resp[0]);
            uint32_t rlen  = net_read_le32(&resp[4]);
            uint32_t rreq  = net_read_le32(&resp[8]);
            if (rtype == expect_type && rlen >= 16U && rreq == expect_req_id) {
                return 0;
            }
            if (rtype == RNDIS_MSG_KEEPALIVE || rtype == RNDIS_MSG_INDICATE) {
                rndis_handle_unsolicited(dev, resp, rlen);
                continue;
            }
        }
        pit_mdelay(10U);
    }
    return -1;
}

static int rndis_query_oid(struct xhci_usb_dev *dev, uint32_t oid,
                           uint8_t *out_buf, uint32_t out_max, uint32_t *out_len)
{
    uint8_t cmd[28];
    uint8_t resp[256];
    uint32_t req = rndis_req_id++;
    uint32_t status, info_len, info_off;

    memset(cmd, 0, sizeof(cmd));
    net_write_le32(&cmd[0],  RNDIS_MSG_QUERY);
    net_write_le32(&cmd[4],  28U);
    net_write_le32(&cmd[8],  req);
    net_write_le32(&cmd[12], oid);
    net_write_le32(&cmd[16], 0U);
    net_write_le32(&cmd[20], 0U);
    net_write_le32(&cmd[24], 0U);

    if (rndis_send_cmd(dev, cmd, (uint16_t)sizeof(cmd),
                       resp, (uint16_t)sizeof(resp), RNDIS_MSG_QUERY_C, req) < 0) {
        return -1;
    }

    status   = net_read_le32(&resp[12]);
    info_len = net_read_le32(&resp[16]);
    info_off = net_read_le32(&resp[20]);
    if (status != 0U || info_len == 0U || (8U + info_off + info_len) > sizeof(resp)) {
        return -1;
    }
    if (info_len > out_max) {
        info_len = out_max;
    }
    memcpy(out_buf, &resp[8U + info_off], info_len);
    if (out_len != NULL) {
        *out_len = info_len;
    }
    return 0;
}

static int rndis_set_oid_u32(struct xhci_usb_dev *dev, uint32_t oid, uint32_t val)
{
    uint8_t cmd[32];
    uint8_t resp[32];
    uint32_t req = rndis_req_id++;

    memset(cmd, 0, sizeof(cmd));
    net_write_le32(&cmd[0],  RNDIS_MSG_SET);
    net_write_le32(&cmd[4],  32U);
    net_write_le32(&cmd[8],  req);
    net_write_le32(&cmd[12], oid);
    net_write_le32(&cmd[16], 4U);
    net_write_le32(&cmd[20], 20U);
    net_write_le32(&cmd[24], 0U);
    net_write_le32(&cmd[28], val);

    if (rndis_send_cmd(dev, cmd, (uint16_t)sizeof(cmd),
                       resp, (uint16_t)sizeof(resp), RNDIS_MSG_SET_C, req) < 0) {
        return -1;
    }
    return (net_read_le32(&resp[12]) == 0U) ? 0 : -1;
}

static void rndis_send_halt(struct xhci_usb_dev *dev)
{
    uint8_t cmd[12];
    net_write_le32(&cmd[0], RNDIS_MSG_HALT);
    net_write_le32(&cmd[4], 12U);
    net_write_le32(&cmd[8], rndis_req_id++);
    (void)xhci_control_transfer(dev, 0x21U, CDC_REQ_SEND_ENCAP_CMD,
                                0U, dev->iface_num, (uint16_t)sizeof(cmd), cmd);
}

static int usbnet_init_rndis(struct xhci_usb_dev *dev)
{
    uint8_t cmd[24];
    uint8_t resp[64];
    uint32_t req;
    uint32_t mac_len = 0;
    uint32_t max_pkts, dev_max_xfer, align_pow;
    uint8_t val_buf[4];
    uint32_t val_len = 0;
    int init_try;
    bool_t init_ok = FALSE;

    for (init_try = 0; init_try < 2; init_try++) {
        req = rndis_req_id++;
        memset(cmd, 0, sizeof(cmd));
        net_write_le32(&cmd[0],  RNDIS_MSG_INIT);
        net_write_le32(&cmd[4],  24U);
        net_write_le32(&cmd[8],  req);
        net_write_le32(&cmd[12], 1U);
        net_write_le32(&cmd[16], 0U);
        net_write_le32(&cmd[20], USBNET_DMA_BUF_SIZE);

        if (rndis_send_cmd(dev, cmd, (uint16_t)sizeof(cmd),
                           resp, (uint16_t)sizeof(resp), RNDIS_MSG_INIT_C, req) == 0 &&
            net_read_le32(&resp[12]) == 0U) {
            init_ok = TRUE;
            break;
        }
        /* Real hardware may still be active from a previous session: send HALT and retry */
        rndis_send_halt(dev);
        pit_mdelay(25U);
    }

    if (!init_ok) {
        kprintf("[USBNET] RNDIS_MSG_INIT failed on slot %u (%04x:%04x)\n",
                (uint32_t)dev->slot_id, (uint32_t)dev->vendor_id, (uint32_t)dev->product_id);
        return -1;
    }

    max_pkts     = net_read_le32(&resp[28]);
    dev_max_xfer = net_read_le32(&resp[32]);
    align_pow    = net_read_le32(&resp[36]);

    rndis_rx_xfer_size = USBNET_DMA_BUF_SIZE;
    if (dev_max_xfer >= 2048U && dev_max_xfer <= USBNET_DMA_BUF_SIZE) {
        rndis_rx_xfer_size = dev_max_xfer;
    }
    rndis_tx_max_size = (dev_max_xfer >= 1600U && dev_max_xfer <= USBNET_DMA_BUF_SIZE)
                            ? dev_max_xfer
                            : 2048U;
    rndis_pkt_align = (align_pow <= 6U) ? (1U << align_pow) : 1U;

    /* Query MAC address (try current address, then permanent address) */
    if (rndis_query_oid(dev, RNDIS_OID_CURR_ADDR, net_info.mac, 6U, &mac_len) < 0 || mac_len < 6U) {
        if (rndis_query_oid(dev, RNDIS_OID_PERM_ADDR, net_info.mac, 6U, &mac_len) < 0 || mac_len < 6U) {
            net_info.mac[0] = 0x02U;
            net_info.mac[1] = 0x00U;
            net_info.mac[2] = 0x00U;
            net_info.mac[3] = 0x00U;
            net_info.mac[4] = 0x00U;
            net_info.mac[5] = 0x01U;
        }
    }

    /* Query optional maximum frame size & media connect status */
    net_info.mtu = 1500U;
    if (rndis_query_oid(dev, RNDIS_OID_MAX_FRAME_SZ, val_buf, 4U, &val_len) == 0 && val_len >= 4U) {
        uint32_t dev_mtu = net_read_le32(val_buf);
        if (dev_mtu >= 576U && dev_mtu <= 1500U) {
            net_info.mtu = (uint16_t)dev_mtu;
        }
    }
    rndis_link_up = TRUE;
    if (rndis_query_oid(dev, RNDIS_OID_MEDIA_STATUS, val_buf, 4U, &val_len) == 0 && val_len >= 4U) {
        rndis_link_up = (net_read_le32(val_buf) == 0U) ? TRUE : FALSE;
    }

    /* Enable reception of directed, broadcast, and multicast Ethernet frames */
    if (rndis_set_oid_u32(dev, RNDIS_OID_PACKET_FILTER, RNDIS_FILTER_DEFAULT) < 0) {
        (void)rndis_set_oid_u32(dev, RNDIS_OID_PACKET_FILTER, RNDIS_FILTER_BASIC);
    }

    kprintf("[USBNET] RNDIS negotiated max_xfer=%u max_pkts=%u align=%u link=%s (%04x:%04x)\n",
            rndis_rx_xfer_size, max_pkts, rndis_pkt_align,
            rndis_link_up ? "UP" : "DOWN",
            (uint32_t)dev->vendor_id, (uint32_t)dev->product_id);
    return 0;
}

static int usbnet_init_ecm(struct xhci_usb_dev *dev)
{
    bool_t got_mac = FALSE;

    if (dev->net_mac_str_idx != 0) {
        uint8_t str_desc[32];
        memset(str_desc, 0, sizeof(str_desc));
        if (xhci_control_transfer(dev, 0x80U, 0x06U,
                                  (uint16_t)(0x0300U | dev->net_mac_str_idx),
                                  0x0409U, 26U, str_desc) == 0 &&
            str_desc[0] >= 26U && str_desc[1] == 0x03U) {
            int i;
            got_mac = TRUE;
            for (i = 0; i < 6; i++) {
                int hi = hex_nibble((char)str_desc[2 + i * 4]);
                int lo = hex_nibble((char)str_desc[4 + i * 4]);
                if (hi < 0 || lo < 0) {
                    got_mac = FALSE;
                    break;
                }
                net_info.mac[i] = (uint8_t)((hi << 4) | lo);
            }
        }
    }

    if (!got_mac) {
        net_info.mac[0] = 0x02U;
        net_info.mac[1] = 0x00U;
        net_info.mac[2] = 0x00U;
        net_info.mac[3] = 0x00U;
        net_info.mac[4] = 0x00U;
        net_info.mac[5] = 0x02U;
    }
    net_info.mtu = 1500U;

    /* Enable CDC-ECM packet filter */
    (void)xhci_control_transfer(dev, 0x21U, CDC_ECM_SET_PKT_FILTER,
                                CDC_ECM_FILTER_DEFAULT, dev->iface_num, 0U, NULL);
    return 0;
}

void usbnet_reset(void)
{
    net_dev            = NULL;
    net_rx_dma         = NULL;
    net_tx_dma         = NULL;
    net_int_dma        = NULL;
    rndis_req_id       = 1U;
    rndis_rx_xfer_size = 2048U;
    rndis_tx_max_size  = 2048U;
    rndis_pkt_align    = 1U;
    rx_armed_len       = 2048U;
    rndis_resp_pending = FALSE;
    rndis_link_up      = TRUE;
    rx_ring_head       = 0;
    rx_ring_tail       = 0;
    memset(&net_info, 0, sizeof(net_info));
}

void usbnet_init_device(struct xhci_usb_dev *dev)
{
    int rc;

    if (net_dev != NULL || dev->bulk_in_dci == 0 || dev->bulk_out_dci == 0) {
        return;
    }

    if (net_rx_dma == NULL) {
        net_rx_dma  = (uint8_t *)xhci_dma_alloc(USBNET_DMA_BUF_SIZE, 64U);
        net_tx_dma  = (uint8_t *)xhci_dma_alloc(USBNET_DMA_BUF_SIZE, 64U);
        net_int_dma = (uint8_t *)xhci_dma_alloc(64U, 64U);
    }
    if (net_rx_dma == NULL || net_tx_dma == NULL || net_int_dma == NULL) {
        return;
    }

    memset(&net_info, 0, sizeof(net_info));
    rx_ring_head       = 0;
    rx_ring_tail       = 0;
    rndis_resp_pending = FALSE;

    /* Arm Interrupt IN before RNDIS INIT so real hardware RESPONSE_AVAILABLE events don't stall */
    net_dev = dev;
    usbnet_arm_int(dev);

    if (dev->dev_type == USB_DEV_NET_RNDIS) {
        net_info.protocol = XHCI_NET_PROTO_RNDIS;
        rc = usbnet_init_rndis(dev);
    } else {
        net_info.protocol = XHCI_NET_PROTO_ECM;
        rc = usbnet_init_ecm(dev);
    }

    if (rc < 0) {
        net_dev = NULL;
        memset(&net_info, 0, sizeof(net_info));
        return;
    }

    net_info.present = TRUE;
    net_info.slot_id = dev->slot_id;
    if (net_info.mtu == 0U) {
        net_info.mtu = 1500U;
    }

    kprintf("[USBNET] USB Ethernet (%s) ready on slot %u MAC=%02x:%02x:%02x:%02x:%02x:%02x (IN=%u OUT=%u)\n",
            (net_info.protocol == XHCI_NET_PROTO_RNDIS) ? "RNDIS" : "CDC-ECM",
            (uint32_t)dev->slot_id,
            (uint32_t)net_info.mac[0], (uint32_t)net_info.mac[1], (uint32_t)net_info.mac[2],
            (uint32_t)net_info.mac[3], (uint32_t)net_info.mac[4], (uint32_t)net_info.mac[5],
            (uint32_t)dev->bulk_in_dci, (uint32_t)dev->bulk_out_dci);

    usbnet_arm_rx(dev);
}

void usbnet_remove_device(struct xhci_usb_dev *dev)
{
    if (net_dev == dev) {
        kprintf("[USBNET] USB Ethernet disconnected (slot %u)\n", (uint32_t)dev->slot_id);
        net_dev            = NULL;
        rndis_resp_pending = FALSE;
        rx_ring_head       = 0;
        rx_ring_tail       = 0;
        memset(&net_info, 0, sizeof(net_info));
    }
}

bool_t usbnet_handle_transfer_event(uint8_t slot_id, uint8_t ep_dci,
                                    uint8_t cc, uint32_t residual)
{
    if (net_dev == NULL || net_dev->slot_id != slot_id) {
        return FALSE;
    }

    if (ep_dci == net_dev->int_in_dci && net_dev->int_in_dci != 0) {
        if ((cc == 1U || cc == 13U) && net_info.protocol == XHCI_NET_PROTO_RNDIS) {
            /* MS-RNDIS RESPONSE_AVAILABLE notification: 01 00 00 00 00 00 00 00 */
            if (net_int_dma[0] == 0x01U && net_int_dma[1] == 0x00U) {
                rndis_resp_pending = TRUE;
            }
        }
        usbnet_arm_int(net_dev);
        return TRUE;
    }

    if (ep_dci == net_dev->bulk_in_dci) {
        if ((cc == 1U || cc == 13U) && residual <= rx_armed_len) {
            uint32_t actual_len = rx_armed_len - residual;
            if (net_info.protocol == XHCI_NET_PROTO_RNDIS) {
                uint32_t pos = 0;
                /* Real RNDIS devices may send 0-byte or 1-byte (0x00) short-packet terminators */
                while (pos + RNDIS_HDR_LEN <= actual_len) {
                    uint32_t msg_type = net_read_le32(&net_rx_dma[pos + 0U]);
                    uint32_t msg_len  = net_read_le32(&net_rx_dma[pos + 4U]);
                    uint32_t data_off = net_read_le32(&net_rx_dma[pos + 8U]);
                    uint32_t data_len = net_read_le32(&net_rx_dma[pos + 12U]);
                    uint32_t pkt_off;
                    uint32_t next_pos;

                    if (msg_type != RNDIS_MSG_PACKET || msg_len < RNDIS_HDR_LEN ||
                        pos + msg_len > actual_len) {
                        break;
                    }
                    pkt_off = pos + 8U + data_off;
                    if (data_len >= USBNET_ETH_MIN_LEN &&
                        pkt_off + data_len <= pos + msg_len) {
                        usbnet_rx_push(&net_rx_dma[pkt_off], data_len);
                    }
                    next_pos = pos + msg_len;
                    if (rndis_pkt_align > 1U && next_pos + RNDIS_HDR_LEN <= actual_len &&
                        net_read_le32(&net_rx_dma[next_pos]) != RNDIS_MSG_PACKET) {
                        next_pos = (next_pos + (rndis_pkt_align - 1U)) & ~(rndis_pkt_align - 1U);
                    }
                    pos = next_pos;
                }
            } else {
                if (actual_len >= USBNET_ETH_MIN_LEN) {
                    usbnet_rx_push(net_rx_dma, actual_len);
                }
            }
        } else if (cc == 3U || cc == 4U || cc == 6U) {
            /* Recover halted Bulk IN endpoint after Babble (3), XactErr (4), or Stall (6) */
            xhci_clear_bulk_stall(net_dev, TRUE);
        }
        usbnet_arm_rx(net_dev);
        return TRUE;
    }

    return FALSE;
}

int32_t xhci_net_send(const uint8_t *frame, uint32_t len)
{
    uint32_t msg_len;
    uint32_t xfer_len;
    int rc;

    if (net_dev == NULL || !net_info.present || frame == NULL ||
        len < USBNET_ETH_MIN_LEN || len > USBNET_ETH_MAX_LEN) {
        return -1;
    }

    if (net_info.protocol == XHCI_NET_PROTO_RNDIS) {
        msg_len  = RNDIS_HDR_LEN + len;
        xfer_len = msg_len;
        if (xfer_len + 1U > rndis_tx_max_size || xfer_len + 1U > USBNET_DMA_BUF_SIZE) {
            return -1;
        }
        memset(net_tx_dma, 0, RNDIS_HDR_LEN);
        net_write_le32(&net_tx_dma[0],  RNDIS_MSG_PACKET);
        net_write_le32(&net_tx_dma[4],  msg_len);
        net_write_le32(&net_tx_dma[8],  36U);
        net_write_le32(&net_tx_dma[12], len);
        memcpy(&net_tx_dma[RNDIS_HDR_LEN], frame, len);

        /* Real RNDIS hardware (Linux rndis_host convention): append a 1-byte 0x00 pad
         * when transfer length is an exact multiple of bulk_out_mps instead of relying on ZLP */
        if (net_dev->bulk_out_mps > 0 && (xfer_len % net_dev->bulk_out_mps) == 0U) {
            net_tx_dma[xfer_len] = 0U;
            xfer_len++;
        }
    } else {
        xfer_len = len;
        memcpy(net_tx_dma, frame, len);
    }

    rc = xhci_bulk_transfer(net_dev, FALSE, net_tx_dma, xfer_len);
    if (rc == -2) {
        /* Bulk OUT stalled and was cleared: retry once */
        rc = xhci_bulk_transfer(net_dev, FALSE, net_tx_dma, xfer_len);
    }
    if (rc < 0) {
        return -1;
    }

    /* For CDC-ECM, terminate exact MaxPacketSize multiples with a ZLP */
    if (net_info.protocol == XHCI_NET_PROTO_ECM &&
        net_dev->bulk_out_mps > 0 && (xfer_len % net_dev->bulk_out_mps) == 0U) {
        (void)xhci_bulk_transfer(net_dev, FALSE, net_tx_dma, 0U);
    }

    net_info.tx_packets++;
    net_info.tx_bytes += len;
    return (int32_t)len;
}

int32_t xhci_net_recv(uint8_t *buf, uint32_t max_len)
{
    uint32_t len;

    if (net_dev == NULL || !net_info.present || buf == NULL || max_len == 0U) {
        return -1;
    }
    xhci_poll();
    if (rndis_resp_pending) {
        rndis_drain_notifications();
    }
    if (rx_ring_tail == rx_ring_head) {
        return 0;
    }

    len = rx_ring_len[rx_ring_tail];
    if (len > max_len) {
        len = max_len;
    }
    memcpy(buf, rx_ring_data[rx_ring_tail], len);
    rx_ring_tail = (rx_ring_tail + 1U) % USBNET_RX_RING_SLOTS;
    return (int32_t)len;
}

bool_t xhci_has_usb_net(void)
{
    return (net_dev != NULL && net_info.present) ? TRUE : FALSE;
}

void xhci_get_net_info(struct xhci_net_info *out)
{
    if (out != NULL) {
        *out = net_info;
    }
}
