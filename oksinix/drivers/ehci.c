/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - EHCI USB 2.0 Host Controller Driver (PCI 0x0C/0x03/0x20)
 */

#include "xhci_internal.h"
#include "pci.h"
#include "pit.h"
#include "io.h"
#include "serial.h"
#include "string.h"

/* EHCI Operational Register Offsets */
#define EHCI_USBCMD         0x00U
#define EHCI_USBSTS         0x04U
#define EHCI_USBINTR        0x08U
#define EHCI_FRINDEX        0x0CU
#define EHCI_CTRLDSSEGMENT  0x10U
#define EHCI_PERIODICBASE   0x14U
#define EHCI_ASYNCLISTADDR  0x18U
#define EHCI_CONFIGFLAG     0x40U
#define EHCI_PORTSC_BASE    0x44U

/* EHCI USBCMD bits */
#define EHCI_CMD_RS         (1UL << 0)
#define EHCI_CMD_HCRESET    (1UL << 1)
#define EHCI_CMD_PSE        (1UL << 4)
#define EHCI_CMD_ASE        (1UL << 5)
#define EHCI_CMD_IAAD       (1UL << 6)
#define EHCI_CMD_ITC_8      (8UL << 16)

/* EHCI USBSTS bits */
#define EHCI_STS_IAA        (1UL << 5)
#define EHCI_STS_HCHALTED   (1UL << 12)

/* EHCI PORTSC bits */
#define EHCI_PORT_CCS       (1UL << 0)
#define EHCI_PORT_CSC       (1UL << 1)
#define EHCI_PORT_PE        (1UL << 2)
#define EHCI_PORT_PEC       (1UL << 3)
#define EHCI_PORT_OCC       (1UL << 5)
#define EHCI_PORT_PR        (1UL << 8)
#define EHCI_PORT_PP        (1UL << 12)
#define EHCI_PORT_OWNER     (1UL << 13)
#define EHCI_PORT_RW1C      (EHCI_PORT_CSC | EHCI_PORT_PEC | EHCI_PORT_OCC)

/* EHCI qTD token bits */
#define EHCI_QTD_HALTED     (1UL << 6)
#define EHCI_QTD_ACTIVE     (1UL << 7)
#define EHCI_QTD_ERRMASK    (0x7CUL)
#define EHCI_QTD_PID_OUT    (0UL << 8)
#define EHCI_QTD_PID_IN     (1UL << 8)
#define EHCI_QTD_PID_SETUP  (2UL << 8)
#define EHCI_QTD_CERR3      (3UL << 10)
#define EHCI_QTD_DT         (1UL << 31)

#define EHCI_ASYNC_SLOTS    4U

struct ehci_qtd {
    volatile uint32_t next_qtd;
    volatile uint32_t alt_qtd;
    volatile uint32_t token;
    volatile uint32_t buf[5];
};

struct ehci_qh {
    volatile uint32_t horiz_link;
    volatile uint32_t ep_chars;
    volatile uint32_t ep_caps;
    volatile uint32_t cur_qtd;
    volatile uint32_t next_qtd;
    volatile uint32_t alt_qtd;
    volatile uint32_t token;
    volatile uint32_t buf[5];
    uint32_t          pad[4];
};

struct ehci_async_slot {
    bool_t               in_use;
    bool_t               is_bulk;
    struct xhci_usb_dev *dev;
    uint8_t              ep_dci;
    uint32_t             total_len;
    struct ehci_qh      *qh;
    struct ehci_qtd     *qtd;
};

struct ehci_hc {
    bool_t                 present;
    uint8_t                hc_idx;
    volatile uint8_t      *cap_base;
    volatile uint8_t      *op_base;
    uint32_t               num_ports;
    bool_t                 has_ppc;
    uint32_t               port_conn_mask;
    uint32_t              *periodic_list;
    struct ehci_qh        *async_head_qh;
    struct ehci_qh        *sync_qh;
    struct ehci_qtd       *sync_qtds;
    uint8_t               *setup_buf;
    uint8_t               *sync_data_buf;
    struct ehci_async_slot async_slots[EHCI_ASYNC_SLOTS];
};

static struct ehci_hc ehci_hcs[USB_MAX_HC_INST];
static int            ehci_count = 0;
static uint32_t       ehci_last_hotplug_tick = 0;

static uint32_t ehci_cap_read32(const struct ehci_hc *hc, uint32_t off)
{
    return *(volatile const uint32_t *)(hc->cap_base + off);
}

static uint32_t ehci_op_read32(const struct ehci_hc *hc, uint32_t off)
{
    return *(volatile const uint32_t *)(hc->op_base + off);
}

static void ehci_op_write32(const struct ehci_hc *hc, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(hc->op_base + off) = val;
}

static void ehci_qtd_set_buf(struct ehci_qtd *qtd, const void *buf, uint32_t len)
{
    uint32_t addr = (uint32_t)(uintptr_t)buf;
    uint32_t page = addr & ~0xFFFUL;
    uint32_t i;

    if (len == 0U || buf == NULL) {
        for (i = 0; i < 5U; i++) {
            qtd->buf[i] = 0U;
        }
        return;
    }
    qtd->buf[0] = addr;
    for (i = 1U; i < 5U; i++) {
        qtd->buf[i] = page + (i * 4096U);
    }
}

static void ehci_setup_qh_ep(struct ehci_qh *qh, const struct xhci_usb_dev *dev,
                             uint8_t ep_num, uint16_t mps, bool_t is_ctrl,
                             bool_t is_int)
{
    uint32_t eps;
    uint32_t chars;
    uint32_t caps;

    if (dev->speed == 2U) {
        eps = 1U; /* Low Speed */
    } else if (dev->speed == 1U) {
        eps = 0U; /* Full Speed */
    } else {
        eps = 2U; /* High Speed */
    }

    /* DTC=1 (bit 14): take data toggle from qTD */
    chars = ((uint32_t)dev->usb_addr & 0x7FUL) |
            (((uint32_t)ep_num & 0x0FUL) << 8) |
            (eps << 12) |
            (1UL << 14) |
            (((uint32_t)mps & 0x7FFUL) << 16);
    if (is_ctrl && eps != 2U) {
        chars |= (1UL << 27); /* Control Endpoint Flag for FS/LS split transactions */
    }
    if (!is_int) {
        chars |= (4UL << 28); /* NAK reload count */
    }

    caps = (1UL << 30); /* Mult = 1 */
    if (is_int) {
        caps |= 0x01UL; /* S-Mask = microframe 0 */
        if (eps != 2U) {
            caps |= (0x1CUL << 8); /* C-Mask = microframes 2,3,4 for split IN */
        }
    }
    if (eps != 2U && dev->tt_hub_addr != 0U) {
        caps |= (((uint32_t)dev->tt_hub_addr & 0x7FUL) << 16) |
                (((uint32_t)dev->tt_port & 0x7FUL) << 23);
    }

    qh->ep_chars = chars;
    qh->ep_caps  = caps;
}

static void ehci_async_doorbell(struct ehci_hc *hc)
{
    uint32_t cmd = ehci_op_read32(hc, EHCI_USBCMD);
    int wait;

    ehci_op_write32(hc, EHCI_USBSTS, EHCI_STS_IAA);
    ehci_op_write32(hc, EHCI_USBCMD, cmd | EHCI_CMD_IAAD);
    for (wait = 0; wait < 200; wait++) {
        if ((ehci_op_read32(hc, EHCI_USBSTS) & EHCI_STS_IAA) != 0UL) {
            ehci_op_write32(hc, EHCI_USBSTS, EHCI_STS_IAA);
            return;
        }
        pit_udelay(100U);
    }
}

static void ehci_async_link_qh(struct ehci_hc *hc, struct ehci_qh *qh)
{
    qh->horiz_link = hc->async_head_qh->horiz_link;
    cpu_memory_barrier();
    hc->async_head_qh->horiz_link = ((uint32_t)(uintptr_t)qh) | 2U;
    cpu_memory_barrier();
}

static void ehci_async_unlink_qh(struct ehci_hc *hc, struct ehci_qh *qh)
{
    uint32_t target = ((uint32_t)(uintptr_t)qh) | 2U;
    struct ehci_qh *cur = hc->async_head_qh;
    int guard = 16;

    while (guard > 0) {
        if (cur->horiz_link == target) {
            cur->horiz_link = qh->horiz_link;
            cpu_memory_barrier();
            ehci_async_doorbell(hc);
            return;
        }
        if (cur->horiz_link == (((uint32_t)(uintptr_t)hc->async_head_qh) | 2U)) {
            break;
        }
        cur = (struct ehci_qh *)(uintptr_t)(cur->horiz_link & ~0x1FUL);
        guard--;
    }
}

static int ehci_run_sync_qh(struct ehci_hc *hc, uint32_t num_qtds, uint32_t *out_final_token)
{
    int timeout = 40000;
    uint32_t i;
    uint32_t err_tok = 0U;

    hc->sync_qh->cur_qtd  = 0U;
    hc->sync_qh->next_qtd = (uint32_t)(uintptr_t)&hc->sync_qtds[0];
    hc->sync_qh->alt_qtd  = 1U;
    hc->sync_qh->token    = 0U;
    for (i = 0; i < 5U; i++) {
        hc->sync_qh->buf[i] = 0U;
    }

    ehci_async_link_qh(hc, hc->sync_qh);

    while (timeout > 0) {
        uint32_t qh_tok = hc->sync_qh->token;
        uint32_t last_tok = hc->sync_qtds[num_qtds - 1U].token;
        if ((qh_tok & EHCI_QTD_HALTED) != 0UL) {
            err_tok = qh_tok;
            break;
        }
        if ((last_tok & EHCI_QTD_ACTIVE) == 0UL) {
            break;
        }
        cpu_pause();
        if ((timeout & 0x1FU) == 0) {
            pit_udelay(25U);
        }
        timeout--;
    }

    if (out_final_token != NULL) {
        *out_final_token = hc->sync_qh->token;
    }

    ehci_async_unlink_qh(hc, hc->sync_qh);

    for (i = 0; i < num_qtds; i++) {
        uint32_t tok = hc->sync_qtds[i].token;
        if ((tok & EHCI_QTD_HALTED) != 0UL) {
            err_tok = tok;
            break;
        }
        if ((tok & EHCI_QTD_ACTIVE) != 0UL) {
            if (timeout <= 0) {
                err_tok = EHCI_QTD_ACTIVE;
            }
            break;
        }
    }

    if ((err_tok & EHCI_QTD_HALTED) != 0UL && (err_tok & 0x3CUL) == 0UL) {
        /* Halted without DataBuf/Babble/XactErr => STALL */
        return -2;
    }
    if (err_tok != 0UL || timeout <= 0) {
        return -1;
    }
    return 0;
}

int ehci_control_transfer(struct xhci_usb_dev *dev,
                          uint8_t bmRequestType, uint8_t bRequest,
                          uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                          void *data)
{
    struct ehci_hc *hc;
    uint32_t idx = 0U;
    uint16_t mps;
    bool_t is_in = ((bmRequestType & 0x80U) != 0U) ? TRUE : FALSE;
    int rc;

    if (dev == NULL || dev->hc_idx >= USB_MAX_HC_INST || wLength > 1024U) {
        return -1;
    }
    hc = &ehci_hcs[dev->hc_idx];
    if (!hc->present) {
        return -1;
    }

    mps = (dev->ep0_mps >= 8U && dev->ep0_mps <= 64U) ? dev->ep0_mps : 64U;

    hc->setup_buf[0] = bmRequestType;
    hc->setup_buf[1] = bRequest;
    hc->setup_buf[2] = (uint8_t)(wValue & 0xFFU);
    hc->setup_buf[3] = (uint8_t)((wValue >> 8) & 0xFFU);
    hc->setup_buf[4] = (uint8_t)(wIndex & 0xFFU);
    hc->setup_buf[5] = (uint8_t)((wIndex >> 8) & 0xFFU);
    hc->setup_buf[6] = (uint8_t)(wLength & 0xFFU);
    hc->setup_buf[7] = (uint8_t)((wLength >> 8) & 0xFFU);

    if (wLength > 0U) {
        if (!is_in && data != NULL) {
            memcpy(hc->sync_data_buf, data, wLength);
        } else {
            memset(hc->sync_data_buf, 0, wLength);
        }
    }

    /* 1. SETUP qTD (DATA0) */
    hc->sync_qtds[idx].next_qtd = (uint32_t)(uintptr_t)&hc->sync_qtds[idx + 1U];
    hc->sync_qtds[idx].alt_qtd  = 1U;
    hc->sync_qtds[idx].token    = EHCI_QTD_ACTIVE | EHCI_QTD_PID_SETUP |
                                  EHCI_QTD_CERR3 | (8UL << 16);
    ehci_qtd_set_buf(&hc->sync_qtds[idx], hc->setup_buf, 8U);
    idx++;

    /* 2. Optional DATA qTD (DATA1; alt_qtd jumps to STATUS on short packet) */
    if (wLength > 0U) {
        hc->sync_qtds[idx].next_qtd = (uint32_t)(uintptr_t)&hc->sync_qtds[idx + 1U];
        hc->sync_qtds[idx].alt_qtd  = (uint32_t)(uintptr_t)&hc->sync_qtds[idx + 1U];
        hc->sync_qtds[idx].token    = EHCI_QTD_ACTIVE | EHCI_QTD_DT | EHCI_QTD_CERR3 |
                                      (is_in ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT) |
                                      ((uint32_t)wLength << 16);
        ehci_qtd_set_buf(&hc->sync_qtds[idx], hc->sync_data_buf, wLength);
        idx++;
    }

    /* 3. STATUS qTD (DATA1) */
    hc->sync_qtds[idx].next_qtd = 1U;
    hc->sync_qtds[idx].alt_qtd  = 1U;
    hc->sync_qtds[idx].token    = EHCI_QTD_ACTIVE | EHCI_QTD_DT | EHCI_QTD_CERR3 |
                                  ((wLength == 0U || !is_in) ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT);
    ehci_qtd_set_buf(&hc->sync_qtds[idx], NULL, 0U);
    idx++;

    ehci_setup_qh_ep(hc->sync_qh, dev, 0U, mps, TRUE, FALSE);

    rc = ehci_run_sync_qh(hc, idx, NULL);
    if (rc < 0) {
        return -1;
    }
    if (wLength > 0U && is_in && data != NULL) {
        memcpy(data, hc->sync_data_buf, wLength);
    }
    return 0;
}

void ehci_clear_bulk_stall(struct xhci_usb_dev *dev, bool_t is_in)
{
    uint8_t dci;
    uint8_t ep_addr;

    if (dev == NULL) {
        return;
    }
    dci = is_in ? dev->bulk_in_dci : dev->bulk_out_dci;
    ep_addr = (uint8_t)((dci / 2U) | (is_in ? 0x80U : 0x00U));
    (void)ehci_control_transfer(dev, 0x02U, 0x01U, 0U, ep_addr, 0U, NULL);
    if (is_in) {
        dev->toggle_in = 0U;
    } else {
        dev->toggle_out = 0U;
    }
}

int ehci_bulk_transfer(struct xhci_usb_dev *dev, bool_t is_in,
                       void *buf, uint32_t len)
{
    struct ehci_hc *hc;
    uint8_t  dci;
    uint8_t  ep_num;
    uint16_t mps;
    uint8_t *toggle_ptr;
    uint8_t *p = (uint8_t *)buf;
    uint32_t remaining = len;

    if (dev == NULL || dev->hc_idx >= USB_MAX_HC_INST) {
        return -1;
    }
    hc = &ehci_hcs[dev->hc_idx];
    if (!hc->present) {
        return -1;
    }

    dci    = is_in ? dev->bulk_in_dci : dev->bulk_out_dci;
    ep_num = (uint8_t)(dci / 2U);
    mps    = is_in ? dev->bulk_in_mps : dev->bulk_out_mps;
    if (mps == 0U) {
        mps = (dev->speed == 3U) ? 512U : 64U;
    }
    toggle_ptr = is_in ? &dev->toggle_in : &dev->toggle_out;

    ehci_setup_qh_ep(hc->sync_qh, dev, ep_num, mps, FALSE, FALSE);

    do {
        uint32_t chunk = (remaining > 16384U) ? 16384U : remaining;
        uint32_t final_tok = 0U;
        uint32_t rem_bytes;
        int rc;

        hc->sync_qtds[0].next_qtd = 1U;
        hc->sync_qtds[0].alt_qtd  = 1U;
        hc->sync_qtds[0].token    = EHCI_QTD_ACTIVE | EHCI_QTD_CERR3 |
                                    (is_in ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT) |
                                    (((*toggle_ptr & 1U) != 0U) ? EHCI_QTD_DT : 0UL) |
                                    (chunk << 16);
        ehci_qtd_set_buf(&hc->sync_qtds[0], p, chunk);

        rc = ehci_run_sync_qh(hc, 1U, &final_tok);
        *toggle_ptr = (uint8_t)((final_tok >> 31) & 1UL);

        if (rc == -2) {
            ehci_clear_bulk_stall(dev, is_in);
            return -2;
        }
        if (rc < 0) {
            return -1;
        }

        rem_bytes = (hc->sync_qtds[0].token >> 16) & 0x7FFFUL;
        if (is_in && rem_bytes > 0U) {
            break; /* Short IN packet ends transfer */
        }
        p += chunk;
        remaining -= chunk;
    } while (remaining > 0U);

    return 0;
}

static struct ehci_async_slot *ehci_get_async_slot(struct ehci_hc *hc,
                                                   struct xhci_usb_dev *dev,
                                                   bool_t is_bulk)
{
    uint32_t i;
    for (i = 0; i < EHCI_ASYNC_SLOTS; i++) {
        if (hc->async_slots[i].in_use &&
            hc->async_slots[i].dev == dev &&
            hc->async_slots[i].is_bulk == is_bulk) {
            return &hc->async_slots[i];
        }
    }
    for (i = 0; i < EHCI_ASYNC_SLOTS; i++) {
        if (!hc->async_slots[i].in_use && hc->async_slots[i].is_bulk == is_bulk) {
            hc->async_slots[i].in_use = TRUE;
            hc->async_slots[i].dev    = dev;
            return &hc->async_slots[i];
        }
    }
    return NULL;
}

static void ehci_submit_async_in(struct xhci_usb_dev *dev, bool_t is_bulk,
                                 void *buf, uint32_t len)
{
    struct ehci_hc *hc;
    struct ehci_async_slot *slot;
    uint8_t  dci;
    uint8_t  ep_num;
    uint16_t mps;
    uint8_t  t;
    uint32_t i;

    if (dev == NULL || dev->hc_idx >= USB_MAX_HC_INST || buf == NULL || len == 0U) {
        return;
    }
    hc = &ehci_hcs[dev->hc_idx];
    if (!hc->present) {
        return;
    }
    if (len > 16384U) {
        len = 16384U;
    }

    slot = ehci_get_async_slot(hc, dev, is_bulk);
    if (slot == NULL) {
        return;
    }

    dci    = is_bulk ? dev->bulk_in_dci : dev->int_in_dci;
    ep_num = (uint8_t)(dci / 2U);
    mps    = is_bulk ? dev->bulk_in_mps : dev->int_in_mps;
    if (mps == 0U) {
        mps = is_bulk ? ((dev->speed == 3U) ? 512U : 64U) : 8U;
    }
    t = is_bulk ? dev->toggle_in : dev->toggle_int;

    /* Unlink async bulk QH if previously active before re-arming */
    if (is_bulk) {
        ehci_async_unlink_qh(hc, slot->qh);
    }

    slot->qtd->next_qtd = 1U;
    slot->qtd->alt_qtd  = 1U;
    slot->qtd->token    = EHCI_QTD_ACTIVE | EHCI_QTD_CERR3 | EHCI_QTD_PID_IN |
                          (((t & 1U) != 0U) ? EHCI_QTD_DT : 0UL) |
                          (len << 16);
    ehci_qtd_set_buf(slot->qtd, buf, len);

    ehci_setup_qh_ep(slot->qh, dev, ep_num, mps, FALSE, !is_bulk);
    slot->qh->cur_qtd  = 0U;
    slot->qh->next_qtd = (uint32_t)(uintptr_t)slot->qtd;
    slot->qh->alt_qtd  = 1U;
    slot->qh->token    = 0U;
    for (i = 0; i < 5U; i++) {
        slot->qh->buf[i] = 0U;
    }

    slot->ep_dci    = dci;
    slot->total_len = len;
    if (is_bulk) {
        dev->async_bulk_active = TRUE;
        dev->async_bulk_buf    = buf;
        dev->async_bulk_len    = len;
        ehci_async_link_qh(hc, slot->qh);
    } else {
        dev->async_int_active = TRUE;
        dev->async_int_buf    = buf;
        dev->async_int_len    = len;
        cpu_memory_barrier();
    }
}

void ehci_interrupt_in_submit(struct xhci_usb_dev *dev, void *buf, uint32_t len)
{
    ehci_submit_async_in(dev, FALSE, buf, len);
}

void ehci_bulk_in_submit(struct xhci_usb_dev *dev, void *buf, uint32_t len)
{
    ehci_submit_async_in(dev, TRUE, buf, len);
}

void ehci_release_device(struct xhci_usb_dev *dev)
{
    struct ehci_hc *hc;
    uint32_t i;

    if (dev == NULL || dev->hc_idx >= USB_MAX_HC_INST) {
        return;
    }
    hc = &ehci_hcs[dev->hc_idx];
    for (i = 0; i < EHCI_ASYNC_SLOTS; i++) {
        if (hc->async_slots[i].in_use && hc->async_slots[i].dev == dev) {
            if (hc->async_slots[i].is_bulk) {
                ehci_async_unlink_qh(hc, hc->async_slots[i].qh);
            } else {
                hc->async_slots[i].qh->next_qtd = 1U;
                hc->async_slots[i].qh->token    = EHCI_QTD_HALTED;
            }
            hc->async_slots[i].in_use = FALSE;
            hc->async_slots[i].dev    = NULL;
        }
    }
    dev->async_int_active  = FALSE;
    dev->async_bulk_active = FALSE;
}

static void ehci_enumerate_port(struct ehci_hc *hc, uint32_t port_idx)
{
    uint32_t port_off = EHCI_PORTSC_BASE + (port_idx * 4U);
    uint32_t st = ehci_op_read32(hc, port_off);
    int wait;

    if ((st & EHCI_PORT_CCS) == 0UL || (st & EHCI_PORT_OWNER) != 0UL) {
        return;
    }

    /* LineStatus == 1 (K-state) indicates Low-Speed device: hand off to companion UHCI/OHCI */
    if (((st >> 10) & 3UL) == 1UL) {
        ehci_op_write32(hc, port_off, (st & ~EHCI_PORT_RW1C) | EHCI_PORT_OWNER);
        return;
    }

    /* Assert Port Reset (PR=1, PE=0) for 50 ms */
    ehci_op_write32(hc, port_off, ((st & ~EHCI_PORT_RW1C) & ~EHCI_PORT_PE) | EHCI_PORT_PR);
    pit_mdelay(50U);

    /* Deassert Port Reset */
    st = ehci_op_read32(hc, port_off);
    ehci_op_write32(hc, port_off, (st & ~EHCI_PORT_RW1C) & ~EHCI_PORT_PR);
    for (wait = 0; wait < 100; wait++) {
        st = ehci_op_read32(hc, port_off);
        if ((st & EHCI_PORT_PR) == 0UL) {
            break;
        }
        pit_mdelay(1U);
    }
    pit_mdelay(20U);

    st = ehci_op_read32(hc, port_off);
    if ((st & EHCI_PORT_CCS) == 0UL) {
        return;
    }

    /* If Port Enable (PE) did not assert after reset, device is Full-Speed: hand off to companion */
    if ((st & EHCI_PORT_PE) == 0UL) {
        ehci_op_write32(hc, port_off, (st & ~EHCI_PORT_RW1C) | EHCI_PORT_OWNER);
        return;
    }

    /* Clear CSC / PEC */
    ehci_op_write32(hc, port_off, (st & ~EHCI_PORT_RW1C) | EHCI_PORT_CSC | EHCI_PORT_PEC);

    /* High-Speed (480 Mb/s, speed = 3U) */
    (void)usb_core_enumerate_device(USB_HCI_EHCI, hc->hc_idx,
                                    (uint8_t)(port_idx + 1U), 0U, 0U,
                                    3U, 0U, 0U);
}

void ehci_poll(void)
{
    int h;
    uint32_t now_ticks = pit_get_ticks();
    bool_t do_hotplug = ((now_ticks - ehci_last_hotplug_tick) >= 250U) ? TRUE : FALSE;

    if (do_hotplug) {
        ehci_last_hotplug_tick = now_ticks;
    }

    for (h = 0; h < ehci_count; h++) {
        struct ehci_hc *hc = &ehci_hcs[h];
        uint32_t i;

        if (!hc->present) {
            continue;
        }

        /* 1. Poll async Interrupt IN and Bulk IN QHs */
        for (i = 0; i < EHCI_ASYNC_SLOTS; i++) {
            struct ehci_async_slot *slot = &hc->async_slots[i];
            uint32_t tok;

            if (!slot->in_use || slot->dev == NULL) {
                continue;
            }
            tok = slot->qtd->token;
            if ((tok & EHCI_QTD_ACTIVE) == 0UL || (slot->qh->token & EHCI_QTD_HALTED) != 0UL) {
                struct xhci_usb_dev *dev = slot->dev;
                uint8_t  ep_dci = slot->ep_dci;
                bool_t   is_bulk = slot->is_bulk;
                uint32_t total_len = slot->total_len;
                uint32_t qh_tok = slot->qh->token;
                uint32_t residual = (tok >> 16) & 0x7FFFUL;
                uint8_t  cc = 1U;

                if (is_bulk) {
                    ehci_async_unlink_qh(hc, slot->qh);
                } else {
                    slot->qh->next_qtd = 1U;
                    slot->qh->token    = EHCI_QTD_HALTED;
                }

                if ((tok & EHCI_QTD_HALTED) != 0UL || (qh_tok & EHCI_QTD_HALTED) != 0UL) {
                    uint32_t err = (tok | qh_tok) & 0x3CUL;
                    cc = (err == 0UL) ? 6U : 4U; /* STALL vs XactErr/Babble */
                    residual = total_len;
                } else if (residual > 0U) {
                    if (residual > total_len) {
                        residual = total_len;
                    }
                    cc = 13U; /* Short Packet */
                }

                if (is_bulk) {
                    dev->toggle_in = (uint8_t)((qh_tok >> 31) & 1UL);
                    dev->async_bulk_active = FALSE;
                } else {
                    dev->toggle_int = (uint8_t)((qh_tok >> 31) & 1UL);
                    dev->async_int_active = FALSE;
                }

                slot->in_use = FALSE;
                slot->dev    = NULL;

                usb_core_dispatch_async(dev, ep_dci, cc, residual);
            }
        }

        /* 2. Hotplug detection on EHCI root ports */
        if (do_hotplug) {
            uint32_t p;
            for (p = 0; p < hc->num_ports; p++) {
                uint32_t port_off = EHCI_PORTSC_BASE + (p * 4U);
                uint32_t st = ehci_op_read32(hc, port_off);
                uint32_t bit = (1U << p);

                if ((st & EHCI_PORT_OWNER) != 0UL) {
                    continue; /* Port owned by companion UHCI/OHCI */
                }
                if ((st & EHCI_PORT_CCS) == 0UL) {
                    if ((hc->port_conn_mask & bit) != 0U) {
                        hc->port_conn_mask &= ~bit;
                        ehci_op_write32(hc, port_off, (st & ~EHCI_PORT_RW1C) | EHCI_PORT_CSC | EHCI_PORT_PEC);
                        usb_core_disconnect_port(USB_HCI_EHCI, hc->hc_idx,
                                                 (uint8_t)(p + 1U), 0U, 0U);
                    }
                } else if ((hc->port_conn_mask & bit) == 0U && xhci_has_free_slot()) {
                    hc->port_conn_mask |= bit;
                    pit_mdelay(30U);
                    ehci_enumerate_port(hc, p);
                }
            }
        }
    }
}

static void ehci_bios_handoff(const struct pci_device *pci_dev, uint32_t hccparams)
{
    uint8_t eecp = (uint8_t)((hccparams >> 8) & 0xFFU);
    int guard = 16;

    while (eecp >= 0x40U && guard > 0) {
        uint32_t reg = pci_read32(pci_dev->bus, pci_dev->slot, pci_dev->func, eecp);
        uint8_t cap_id = (uint8_t)(reg & 0xFFU);
        uint8_t next   = (uint8_t)((reg >> 8) & 0xFFU);

        if (reg == 0xFFFFFFFFUL) {
            break;
        }
        if (cap_id == 1U) {
            if ((reg & (1UL << 16)) != 0UL) {
                int wait;
                pci_write32(pci_dev->bus, pci_dev->slot, pci_dev->func, eecp, reg | (1UL << 24));
                for (wait = 0; wait < 500; wait++) {
                    reg = pci_read32(pci_dev->bus, pci_dev->slot, pci_dev->func, eecp);
                    if ((reg & (1UL << 16)) == 0UL) {
                        break;
                    }
                    pit_mdelay(1U);
                }
                if ((reg & (1UL << 16)) != 0UL) {
                    pci_write32(pci_dev->bus, pci_dev->slot, pci_dev->func, eecp,
                                (reg & ~(1UL << 16)) | (1UL << 24));
                }
            }
            /* Disable Legacy SMIs in USBLEGCTLSTS */
            pci_write32(pci_dev->bus, pci_dev->slot, pci_dev->func, (uint8_t)(eecp + 4U), 0UL);
            break;
        }
        eecp = next;
        guard--;
    }
}

static int ehci_init_one(const struct pci_device *pci_dev, uint8_t hc_idx)
{
    struct ehci_hc *hc = &ehci_hcs[hc_idx];
    uint32_t cap0;
    uint8_t  cap_len;
    uint32_t hcsparams;
    uint32_t hccparams;
    uint32_t cmd;
    uint32_t i;

    if (pci_dev->bar0_mmio == 0UL) {
        return -1;
    }

    memset(hc, 0, sizeof(*hc));
    hc->hc_idx   = hc_idx;
    hc->cap_base = (volatile uint8_t *)(uintptr_t)pci_dev->bar0_mmio;

    cap0 = ehci_cap_read32(hc, 0x00U);
    if (cap0 == 0xFFFFFFFFUL || (cap0 & 0xFFU) == 0U) {
        return -1;
    }
    cap_len       = (uint8_t)(cap0 & 0xFFU);
    hc->op_base   = hc->cap_base + cap_len;
    hcsparams     = ehci_cap_read32(hc, 0x04U);
    hccparams     = ehci_cap_read32(hc, 0x08U);
    hc->num_ports = hcsparams & 0x0FU;
    hc->has_ppc   = ((hcsparams & (1UL << 4)) != 0UL) ? TRUE : FALSE;

    ehci_bios_handoff(pci_dev, hccparams);

    /* Halt controller */
    cmd = ehci_op_read32(hc, EHCI_USBCMD);
    ehci_op_write32(hc, EHCI_USBCMD, cmd & ~EHCI_CMD_RS);
    for (i = 0; i < 100U; i++) {
        if ((ehci_op_read32(hc, EHCI_USBSTS) & EHCI_STS_HCHALTED) != 0UL) {
            break;
        }
        pit_mdelay(1U);
    }

    /* Reset controller */
    ehci_op_write32(hc, EHCI_USBCMD, EHCI_CMD_HCRESET);
    for (i = 0; i < 200U; i++) {
        if ((ehci_op_read32(hc, EHCI_USBCMD) & EHCI_CMD_HCRESET) == 0UL) {
            break;
        }
        pit_mdelay(1U);
    }

    ehci_op_write32(hc, EHCI_CTRLDSSEGMENT, 0UL);
    ehci_op_write32(hc, EHCI_USBINTR, 0UL);
    ehci_op_write32(hc, EHCI_USBSTS, 0x3FUL);

    hc->periodic_list = (uint32_t *)xhci_dma_alloc(4096U, 4096U);
    hc->async_head_qh = (struct ehci_qh *)xhci_dma_alloc(sizeof(struct ehci_qh), 64U);
    hc->sync_qh       = (struct ehci_qh *)xhci_dma_alloc(sizeof(struct ehci_qh), 64U);
    hc->sync_qtds     = (struct ehci_qtd *)xhci_dma_alloc(4U * sizeof(struct ehci_qtd), 32U);
    hc->setup_buf     = (uint8_t *)xhci_dma_alloc(64U, 32U);
    hc->sync_data_buf = (uint8_t *)xhci_dma_alloc(1024U, 64U);
    if (hc->periodic_list == NULL || hc->async_head_qh == NULL || hc->sync_qh == NULL ||
        hc->sync_qtds == NULL || hc->setup_buf == NULL || hc->sync_data_buf == NULL) {
        return -1;
    }

    /* Setup permanent reclamation head QH on Async Schedule */
    hc->async_head_qh->horiz_link = ((uint32_t)(uintptr_t)hc->async_head_qh) | 2U;
    hc->async_head_qh->ep_chars   = (1UL << 15); /* H = 1 */
    hc->async_head_qh->ep_caps    = (1UL << 30); /* Mult = 1 */
    hc->async_head_qh->next_qtd   = 1U;
    hc->async_head_qh->alt_qtd    = 1U;
    hc->async_head_qh->token      = EHCI_QTD_HALTED;

    /* Slots 0..2 = Interrupt IN (Periodic schedule), Slot 3 = Async Bulk IN (Async schedule) */
    for (i = 0; i < EHCI_ASYNC_SLOTS; i++) {
        hc->async_slots[i].is_bulk = (i == EHCI_ASYNC_SLOTS - 1U) ? TRUE : FALSE;
        hc->async_slots[i].qh      = (struct ehci_qh *)xhci_dma_alloc(sizeof(struct ehci_qh), 64U);
        hc->async_slots[i].qtd     = (struct ehci_qtd *)xhci_dma_alloc(sizeof(struct ehci_qtd), 32U);
        if (hc->async_slots[i].qh == NULL || hc->async_slots[i].qtd == NULL) {
            return -1;
        }
        hc->async_slots[i].qh->horiz_link = 1U;
        hc->async_slots[i].qh->ep_caps    = (1UL << 30);
        hc->async_slots[i].qh->next_qtd   = 1U;
        hc->async_slots[i].qh->alt_qtd    = 1U;
        hc->async_slots[i].qh->token      = EHCI_QTD_HALTED;
    }

    /* Link Interrupt QHs into Periodic Frame List: periodic[*] -> slot[0] -> slot[1] -> slot[2] */
    hc->async_slots[0].qh->horiz_link = ((uint32_t)(uintptr_t)hc->async_slots[1].qh) | 2U;
    hc->async_slots[1].qh->horiz_link = ((uint32_t)(uintptr_t)hc->async_slots[2].qh) | 2U;
    hc->async_slots[2].qh->horiz_link = 1U;
    for (i = 0; i < 1024U; i++) {
        hc->periodic_list[i] = ((uint32_t)(uintptr_t)hc->async_slots[0].qh) | 2U;
    }

    ehci_op_write32(hc, EHCI_PERIODICBASE, (uint32_t)(uintptr_t)hc->periodic_list);
    ehci_op_write32(hc, EHCI_ASYNCLISTADDR, (uint32_t)(uintptr_t)hc->async_head_qh);

    /* Run controller with Periodic & Async schedules enabled */
    ehci_op_write32(hc, EHCI_USBCMD, EHCI_CMD_ITC_8 | EHCI_CMD_ASE | EHCI_CMD_PSE | EHCI_CMD_RS);
    for (i = 0; i < 100U; i++) {
        if ((ehci_op_read32(hc, EHCI_USBSTS) & EHCI_STS_HCHALTED) == 0UL) {
            break;
        }
        pit_mdelay(1U);
    }

    /* Route all root ports to EHCI (non-HS devices will be released to companion UHCI/OHCI) */
    ehci_op_write32(hc, EHCI_CONFIGFLAG, 1UL);
    pit_mdelay(20U);

    if (hc->has_ppc) {
        for (i = 0; i < hc->num_ports; i++) {
            uint32_t off = EHCI_PORTSC_BASE + (i * 4U);
            uint32_t st  = ehci_op_read32(hc, off);
            ehci_op_write32(hc, off, (st & ~EHCI_PORT_RW1C) | EHCI_PORT_PP);
        }
        pit_mdelay(40U);
    }

    hc->present = TRUE;
    kprintf("[EHCI] Host controller #%u running (MMIO=0x%08x, ports=%u)\n",
            (uint32_t)hc_idx, pci_dev->bar0_mmio, hc->num_ports);

    for (i = 0; i < hc->num_ports && xhci_has_free_slot(); i++) {
        uint32_t st = ehci_op_read32(hc, EHCI_PORTSC_BASE + (i * 4U));
        if ((st & EHCI_PORT_CCS) != 0UL && (st & EHCI_PORT_OWNER) == 0UL) {
            hc->port_conn_mask |= (1U << i);
            ehci_enumerate_port(hc, i);
            /* If port was released to companion UHCI/OHCI, clear our conn bit */
            if ((ehci_op_read32(hc, EHCI_PORTSC_BASE + (i * 4U)) & EHCI_PORT_OWNER) != 0UL) {
                hc->port_conn_mask &= ~(1U << i);
            }
        }
    }

    usb_core_record_controller(USB_HCI_EHCI, pci_dev, hc->num_ports);
    return 0;
}

int ehci_init_all(void)
{
    struct pci_device pci_dev;
    int idx;

    ehci_count = 0;
    memset(ehci_hcs, 0, sizeof(ehci_hcs));

    for (idx = 0; idx < USB_MAX_HC_INST; idx++) {
        if (pci_find_class_nth(0x0CU, 0x03U, 0x20U, idx, &pci_dev) < 0) {
            break;
        }
        if (ehci_init_one(&pci_dev, (uint8_t)ehci_count) == 0) {
            ehci_count++;
        }
    }
    ehci_last_hotplug_tick = pit_get_ticks();
    return ehci_count;
}
