/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - OHCI USB 1.1 Host Controller Driver (PCI 0x0C/0x03/0x10)
 */

#include "xhci_internal.h"
#include "pci.h"
#include "pit.h"
#include "io.h"
#include "serial.h"
#include "string.h"

/* OHCI Operational Register Offsets */
#define OHCI_REVISION       0x00U
#define OHCI_CONTROL        0x04U
#define OHCI_CMD_STATUS     0x08U
#define OHCI_INTR_STATUS    0x0CU
#define OHCI_INTR_DISABLE   0x14U
#define OHCI_HCCA           0x18U
#define OHCI_CTRL_HEAD_ED   0x20U
#define OHCI_CTRL_CUR_ED    0x24U
#define OHCI_BULK_HEAD_ED   0x28U
#define OHCI_BULK_CUR_ED    0x2CU
#define OHCI_FM_INTERVAL    0x34U
#define OHCI_PERIODIC_START 0x40U
#define OHCI_RH_DESC_A      0x48U
#define OHCI_RH_STATUS      0x50U
#define OHCI_RH_PORT_BASE   0x54U

/* OHCI HcControl bits */
#define OHCI_CTRL_PLE       (1UL << 2)
#define OHCI_CTRL_CLE       (1UL << 4)
#define OHCI_CTRL_BLE       (1UL << 5)
#define OHCI_CTRL_HCFS_MASK (3UL << 6)
#define OHCI_CTRL_HCFS_OPER (2UL << 6)
#define OHCI_CTRL_IR        (1UL << 8)

/* OHCI HcCommandStatus bits */
#define OHCI_CMD_HCR        (1UL << 0)
#define OHCI_CMD_CLF        (1UL << 1)
#define OHCI_CMD_BLF        (1UL << 2)
#define OHCI_CMD_OCR        (1UL << 3)

/* OHCI Root Hub Port Status bits */
#define OHCI_PORT_CCS       (1UL << 0)
#define OHCI_PORT_PES       (1UL << 1)
#define OHCI_PORT_PRS       (1UL << 4)
#define OHCI_PORT_PPS       (1UL << 8)
#define OHCI_PORT_LSDA      (1UL << 9)
#define OHCI_PORT_CSC       (1UL << 16)
#define OHCI_PORT_PESC      (1UL << 17)
#define OHCI_PORT_PRSC      (1UL << 20)

/* OHCI ED / TD flags */
#define OHCI_ED_SKIP        (1UL << 14)
#define OHCI_TD_R           (1UL << 18)
#define OHCI_TD_DP_SETUP    (0UL << 19)
#define OHCI_TD_DP_OUT      (1UL << 19)
#define OHCI_TD_DP_IN       (2UL << 19)
#define OHCI_TD_DI_NONE     (7UL << 21)
#define OHCI_TD_T_ED        (0UL << 24)
#define OHCI_TD_T_DATA0     (2UL << 24)
#define OHCI_TD_T_DATA1     (3UL << 24)
#define OHCI_TD_CC_NOTACC   (0xFUL << 28)

#define OHCI_SYNC_TDS       8U
#define OHCI_ASYNC_SLOTS    4U

struct ohci_hcca {
    volatile uint32_t int_table[32];
    volatile uint16_t frame_num;
    volatile uint16_t pad1;
    volatile uint32_t done_head;
    uint8_t           reserved[116];
};

struct ohci_ed {
    volatile uint32_t ctrl;
    volatile uint32_t tail_p;
    volatile uint32_t head_p;
    volatile uint32_t next_ed;
};

struct ohci_td {
    volatile uint32_t ctrl;
    volatile uint32_t cbp;
    volatile uint32_t next_td;
    volatile uint32_t be;
};

struct ohci_async_slot {
    bool_t               in_use;
    bool_t               is_bulk;
    struct xhci_usb_dev *dev;
    uint8_t              ep_dci;
    uint32_t             total_len;
    struct ohci_ed      *ed;
    struct ohci_td      *td;
    struct ohci_td      *tail_td;
};

struct ohci_hc {
    bool_t                 present;
    uint8_t                hc_idx;
    volatile uint8_t      *mmio;
    uint32_t               num_ports;
    uint32_t               port_conn_mask;
    struct ohci_hcca      *hcca;
    struct ohci_ed        *int_head_ed;
    struct ohci_ed        *bulk_head_ed;
    struct ohci_ed        *sync_ctrl_ed;
    struct ohci_ed        *sync_bulk_ed;
    struct ohci_td        *sync_tds;
    struct ohci_td        *sync_tail_td;
    uint8_t               *setup_buf;
    uint8_t               *sync_data_buf;
    struct ohci_async_slot async_slots[OHCI_ASYNC_SLOTS];
};

static struct ohci_hc ohci_hcs[USB_MAX_HC_INST];
static int            ohci_count = 0;
static uint32_t       ohci_last_hotplug_tick = 0;

static uint32_t ohci_read32(const struct ohci_hc *hc, uint32_t off)
{
    return *(volatile const uint32_t *)(hc->mmio + off);
}

static void ohci_write32(const struct ohci_hc *hc, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(hc->mmio + off) = val;
}

static uint32_t ohci_make_ed_ctrl(uint8_t addr, uint8_t ep, uint8_t speed,
                                  uint16_t mps, bool_t skip)
{
    return ((uint32_t)addr & 0x7FUL) |
           (((uint32_t)ep & 0x0FUL) << 7) |
           ((speed == 2U) ? (1UL << 13) : 0UL) |
           (skip ? OHCI_ED_SKIP : 0UL) |
           (((uint32_t)mps & 0x7FFUL) << 16);
}

static int ohci_wait_ed(struct ohci_hc *hc, struct ohci_ed *ed,
                        uint32_t list_flag, uint32_t num_tds, uint8_t *out_cc)
{
    int timeout = 40000;
    uint32_t i;
    uint8_t cc = 0U;

    cpu_memory_barrier();
    ed->ctrl &= ~OHCI_ED_SKIP;
    cpu_memory_barrier();
    ohci_write32(hc, OHCI_CMD_STATUS, list_flag);

    while (timeout > 0) {
        uint32_t hp = ed->head_p;
        if ((hp & 1UL) != 0UL || (hp & ~0x0FUL) == ed->tail_p) {
            break;
        }
        cpu_pause();
        if ((timeout & 0x1FU) == 0) {
            pit_udelay(25U);
        }
        timeout--;
    }

    ed->ctrl |= OHCI_ED_SKIP;
    cpu_memory_barrier();

    for (i = 0; i < num_tds; i++) {
        uint8_t td_cc = (uint8_t)((hc->sync_tds[i].ctrl >> 28) & 0x0FU);
        if (td_cc == 0x0FU) {
            if (timeout <= 0) {
                cc = 0x0FU;
            }
            break;
        }
        /* CC=0 (NoError) and CC=9 (DataUnderrun with R=1) are normal completions */
        if (td_cc != 0U && td_cc != 9U) {
            cc = td_cc;
            break;
        }
    }

    if (out_cc != NULL) {
        *out_cc = cc;
    }
    if (cc == 4U) { /* OHCI CC=4: STALL */
        return -2;
    }
    if (cc != 0U || timeout <= 0) {
        return -1;
    }
    return 0;
}

int ohci_control_transfer(struct xhci_usb_dev *dev,
                          uint8_t bmRequestType, uint8_t bRequest,
                          uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                          void *data)
{
    struct ohci_hc *hc;
    uint32_t td_idx = 0U;
    uint16_t mps;
    bool_t is_in = ((bmRequestType & 0x80U) != 0U) ? TRUE : FALSE;
    uint32_t i;
    int rc;

    if (dev == NULL || dev->hc_idx >= USB_MAX_HC_INST || wLength > 1024U) {
        return -1;
    }
    hc = &ohci_hcs[dev->hc_idx];
    if (!hc->present) {
        return -1;
    }

    mps = (dev->ep0_mps >= 8U && dev->ep0_mps <= 64U) ? dev->ep0_mps : 8U;

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

    /* 1. SETUP TD (DATA0) */
    hc->sync_tds[td_idx].ctrl = OHCI_TD_DP_SETUP | OHCI_TD_DI_NONE | OHCI_TD_T_DATA0 | OHCI_TD_CC_NOTACC;
    hc->sync_tds[td_idx].cbp  = (uint32_t)(uintptr_t)hc->setup_buf;
    hc->sync_tds[td_idx].be   = (uint32_t)(uintptr_t)(hc->setup_buf + 7U);
    td_idx++;

    /* 2. DATA TD (single OHCI TD handles up to 4096B with automatic packetization) */
    if (wLength > 0U) {
        hc->sync_tds[td_idx].ctrl = (is_in ? (OHCI_TD_DP_IN | OHCI_TD_R) : OHCI_TD_DP_OUT) |
                                    OHCI_TD_DI_NONE | OHCI_TD_T_DATA1 | OHCI_TD_CC_NOTACC;
        hc->sync_tds[td_idx].cbp  = (uint32_t)(uintptr_t)hc->sync_data_buf;
        hc->sync_tds[td_idx].be   = (uint32_t)(uintptr_t)(hc->sync_data_buf + wLength - 1U);
        td_idx++;
    }

    /* 3. STATUS TD (DATA1) */
    hc->sync_tds[td_idx].ctrl = ((wLength == 0U || !is_in) ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT) |
                                OHCI_TD_DI_NONE | OHCI_TD_T_DATA1 | OHCI_TD_CC_NOTACC;
    hc->sync_tds[td_idx].cbp  = 0U;
    hc->sync_tds[td_idx].be   = 0U;
    td_idx++;

    for (i = 0; i < td_idx; i++) {
        hc->sync_tds[i].next_td = (i + 1U < td_idx)
                                      ? (uint32_t)(uintptr_t)&hc->sync_tds[i + 1U]
                                      : (uint32_t)(uintptr_t)hc->sync_tail_td;
    }

    hc->sync_ctrl_ed->ctrl   = ohci_make_ed_ctrl(dev->usb_addr, 0U, dev->speed, mps, TRUE);
    hc->sync_ctrl_ed->tail_p = (uint32_t)(uintptr_t)hc->sync_tail_td;
    hc->sync_ctrl_ed->head_p = (uint32_t)(uintptr_t)&hc->sync_tds[0];
    hc->sync_ctrl_ed->next_ed = 0U;
    ohci_write32(hc, OHCI_CTRL_HEAD_ED, (uint32_t)(uintptr_t)hc->sync_ctrl_ed);
    ohci_write32(hc, OHCI_CTRL_CUR_ED, 0U);

    rc = ohci_wait_ed(hc, hc->sync_ctrl_ed, OHCI_CMD_CLF, td_idx, NULL);
    if (rc < 0) {
        return -1;
    }
    if (wLength > 0U && is_in && data != NULL) {
        memcpy(data, hc->sync_data_buf, wLength);
    }
    return 0;
}

void ohci_clear_bulk_stall(struct xhci_usb_dev *dev, bool_t is_in)
{
    uint8_t dci;
    uint8_t ep_addr;

    if (dev == NULL) {
        return;
    }
    dci = is_in ? dev->bulk_in_dci : dev->bulk_out_dci;
    ep_addr = (uint8_t)((dci / 2U) | (is_in ? 0x80U : 0x00U));
    (void)ohci_control_transfer(dev, 0x02U, 0x01U, 0U, ep_addr, 0U, NULL);
    if (is_in) {
        dev->toggle_in = 0U;
    } else {
        dev->toggle_out = 0U;
    }
}

int ohci_bulk_transfer(struct xhci_usb_dev *dev, bool_t is_in,
                       void *buf, uint32_t len)
{
    struct ohci_hc *hc;
    uint8_t  dci;
    uint8_t  ep_num;
    uint16_t mps;
    uint8_t *toggle_ptr;
    uint8_t *p = (uint8_t *)buf;
    uint32_t remaining = len;

    if (dev == NULL || dev->hc_idx >= USB_MAX_HC_INST) {
        return -1;
    }
    hc = &ohci_hcs[dev->hc_idx];
    if (!hc->present) {
        return -1;
    }

    dci        = is_in ? dev->bulk_in_dci : dev->bulk_out_dci;
    ep_num     = (uint8_t)(dci / 2U);
    mps        = is_in ? dev->bulk_in_mps : dev->bulk_out_mps;
    if (mps == 0U || mps > 64U) {
        mps = 64U;
    }
    toggle_ptr = is_in ? &dev->toggle_in : &dev->toggle_out;

    do {
        uint32_t chunk = (remaining > 4096U) ? 4096U : remaining;
        uint8_t  cc = 0U;
        int rc;

        hc->sync_tds[0].ctrl    = (is_in ? (OHCI_TD_DP_IN | OHCI_TD_R) : OHCI_TD_DP_OUT) |
                                  OHCI_TD_DI_NONE | OHCI_TD_T_ED | OHCI_TD_CC_NOTACC;
        hc->sync_tds[0].cbp     = (chunk > 0U) ? (uint32_t)(uintptr_t)p : 0U;
        hc->sync_tds[0].be      = (chunk > 0U) ? (uint32_t)(uintptr_t)(p + chunk - 1U) : 0U;
        hc->sync_tds[0].next_td = (uint32_t)(uintptr_t)hc->sync_tail_td;

        hc->sync_bulk_ed->ctrl   = ohci_make_ed_ctrl(dev->usb_addr, ep_num, dev->speed, mps, TRUE);
        hc->sync_bulk_ed->tail_p = (uint32_t)(uintptr_t)hc->sync_tail_td;
        hc->sync_bulk_ed->head_p = ((uint32_t)(uintptr_t)&hc->sync_tds[0]) |
                                   (((uint32_t)*toggle_ptr & 1UL) << 1);

        rc = ohci_wait_ed(hc, hc->sync_bulk_ed, OHCI_CMD_BLF, 1U, &cc);
        *toggle_ptr = (uint8_t)((hc->sync_bulk_ed->head_p >> 1) & 1UL);

        if (rc == -2) {
            ohci_clear_bulk_stall(dev, is_in);
            return -2;
        }
        if (rc < 0) {
            return -1;
        }

        /* Short IN packet ends transfer early */
        if (is_in && chunk > 0U && hc->sync_tds[0].cbp != 0U) {
            break;
        }
        p += chunk;
        remaining -= chunk;
    } while (remaining > 0U);

    return 0;
}

static struct ohci_async_slot *ohci_get_async_slot(struct ohci_hc *hc,
                                                   struct xhci_usb_dev *dev,
                                                   bool_t is_bulk)
{
    uint32_t i;
    for (i = 0; i < OHCI_ASYNC_SLOTS; i++) {
        if (hc->async_slots[i].in_use &&
            hc->async_slots[i].dev == dev &&
            hc->async_slots[i].is_bulk == is_bulk) {
            return &hc->async_slots[i];
        }
    }
    for (i = 0; i < OHCI_ASYNC_SLOTS; i++) {
        if (!hc->async_slots[i].in_use && hc->async_slots[i].is_bulk == is_bulk) {
            hc->async_slots[i].in_use = TRUE;
            hc->async_slots[i].dev    = dev;
            return &hc->async_slots[i];
        }
    }
    return NULL;
}

static void ohci_submit_async_in(struct xhci_usb_dev *dev, bool_t is_bulk,
                                 void *buf, uint32_t len)
{
    struct ohci_hc *hc;
    struct ohci_async_slot *slot;
    uint8_t  dci;
    uint8_t  ep_num;
    uint16_t mps;
    uint8_t  t;

    if (dev == NULL || dev->hc_idx >= USB_MAX_HC_INST || buf == NULL || len == 0U) {
        return;
    }
    hc = &ohci_hcs[dev->hc_idx];
    if (!hc->present) {
        return;
    }
    if (len > 4096U) {
        len = 4096U;
    }

    slot = ohci_get_async_slot(hc, dev, is_bulk);
    if (slot == NULL) {
        return;
    }

    slot->ed->ctrl |= OHCI_ED_SKIP;
    cpu_memory_barrier();

    dci    = is_bulk ? dev->bulk_in_dci : dev->int_in_dci;
    ep_num = (uint8_t)(dci / 2U);
    mps    = is_bulk ? dev->bulk_in_mps : dev->int_in_mps;
    if (mps == 0U || mps > 64U) {
        mps = is_bulk ? 64U : 8U;
    }
    t = is_bulk ? dev->toggle_in : dev->toggle_int;

    slot->td->ctrl    = OHCI_TD_DP_IN | OHCI_TD_R | OHCI_TD_DI_NONE |
                        OHCI_TD_T_ED | OHCI_TD_CC_NOTACC;
    slot->td->cbp     = (uint32_t)(uintptr_t)buf;
    slot->td->be      = (uint32_t)(uintptr_t)((uint8_t *)buf + len - 1U);
    slot->td->next_td = (uint32_t)(uintptr_t)slot->tail_td;

    slot->ed->ctrl    = ohci_make_ed_ctrl(dev->usb_addr, ep_num, dev->speed, mps, TRUE);
    slot->ed->tail_p  = (uint32_t)(uintptr_t)slot->tail_td;
    slot->ed->head_p  = ((uint32_t)(uintptr_t)slot->td) | (((uint32_t)t & 1UL) << 1);

    slot->ep_dci    = dci;
    slot->total_len = len;
    if (is_bulk) {
        dev->async_bulk_active = TRUE;
        dev->async_bulk_buf    = buf;
        dev->async_bulk_len    = len;
    } else {
        dev->async_int_active = TRUE;
        dev->async_int_buf    = buf;
        dev->async_int_len    = len;
    }

    cpu_memory_barrier();
    slot->ed->ctrl &= ~OHCI_ED_SKIP;
    cpu_memory_barrier();
    if (is_bulk) {
        ohci_write32(hc, OHCI_CMD_STATUS, OHCI_CMD_BLF);
    }
}

void ohci_interrupt_in_submit(struct xhci_usb_dev *dev, void *buf, uint32_t len)
{
    ohci_submit_async_in(dev, FALSE, buf, len);
}

void ohci_bulk_in_submit(struct xhci_usb_dev *dev, void *buf, uint32_t len)
{
    ohci_submit_async_in(dev, TRUE, buf, len);
}

void ohci_release_device(struct xhci_usb_dev *dev)
{
    struct ohci_hc *hc;
    uint32_t i;

    if (dev == NULL || dev->hc_idx >= USB_MAX_HC_INST) {
        return;
    }
    hc = &ohci_hcs[dev->hc_idx];
    for (i = 0; i < OHCI_ASYNC_SLOTS; i++) {
        if (hc->async_slots[i].in_use && hc->async_slots[i].dev == dev) {
            hc->async_slots[i].ed->ctrl |= OHCI_ED_SKIP;
            hc->async_slots[i].in_use    = FALSE;
            hc->async_slots[i].dev       = NULL;
        }
    }
    dev->async_int_active  = FALSE;
    dev->async_bulk_active = FALSE;
}

static void ohci_enumerate_port(struct ohci_hc *hc, uint32_t port_idx)
{
    uint32_t port_off = OHCI_RH_PORT_BASE + (port_idx * 4U);
    uint32_t st;
    uint8_t  speed;
    int      wait;

    /* Ensure port power is enabled */
    ohci_write32(hc, port_off, OHCI_PORT_PPS);
    pit_mdelay(10U);

    /* Assert Port Reset (PRS) and wait for PRSC or PRS clear */
    ohci_write32(hc, port_off, OHCI_PORT_PRS);
    for (wait = 0; wait < 200; wait++) {
        st = ohci_read32(hc, port_off);
        if ((st & OHCI_PORT_PRSC) != 0UL || (st & OHCI_PORT_PRS) == 0UL) {
            break;
        }
        pit_mdelay(1U);
    }

    /* Clear PRSC / CSC / PESC */
    ohci_write32(hc, port_off, OHCI_PORT_PRSC | OHCI_PORT_CSC | OHCI_PORT_PESC);
    pit_mdelay(20U);

    st = ohci_read32(hc, port_off);
    if ((st & (OHCI_PORT_CCS | OHCI_PORT_PES)) != (OHCI_PORT_CCS | OHCI_PORT_PES)) {
        return;
    }

    speed = ((st & OHCI_PORT_LSDA) != 0UL) ? 2U : 1U;
    (void)usb_core_enumerate_device(USB_HCI_OHCI, hc->hc_idx,
                                    (uint8_t)(port_idx + 1U), 0U, 0U,
                                    speed, 0U, 0U);
}

void ohci_poll(void)
{
    int h;
    uint32_t now_ticks = pit_get_ticks();
    bool_t do_hotplug = ((now_ticks - ohci_last_hotplug_tick) >= 250U) ? TRUE : FALSE;

    if (do_hotplug) {
        ohci_last_hotplug_tick = now_ticks;
    }

    for (h = 0; h < ohci_count; h++) {
        struct ohci_hc *hc = &ohci_hcs[h];
        uint32_t i;

        if (!hc->present) {
            continue;
        }

        /* 1. Poll async Interrupt IN and Bulk IN EDs */
        for (i = 0; i < OHCI_ASYNC_SLOTS; i++) {
            struct ohci_async_slot *slot = &hc->async_slots[i];
            uint32_t hp;

            if (!slot->in_use || slot->dev == NULL) {
                continue;
            }
            hp = slot->ed->head_p;
            if ((hp & 1UL) != 0UL || (hp & ~0x0FUL) == slot->ed->tail_p) {
                struct xhci_usb_dev *dev = slot->dev;
                uint8_t  ep_dci = slot->ep_dci;
                bool_t   is_bulk = slot->is_bulk;
                uint32_t total_len = slot->total_len;
                uint8_t  td_cc = (uint8_t)((slot->td->ctrl >> 28) & 0x0FU);
                uint32_t residual = 0U;
                uint8_t  cc = 1U;

                slot->ed->ctrl |= OHCI_ED_SKIP;
                cpu_memory_barrier();

                if (td_cc == 4U) {
                    cc = 6U; /* STALL */
                    residual = total_len;
                } else if (td_cc != 0U && td_cc != 9U) {
                    cc = 4U; /* Transaction error */
                    residual = total_len;
                } else {
                    uint32_t cbp = slot->td->cbp;
                    uint32_t be  = slot->td->be;
                    if (cbp != 0U && cbp <= be) {
                        residual = (be - cbp) + 1U;
                        if (residual > total_len) {
                            residual = total_len;
                        }
                    }
                    if (residual > 0U || td_cc == 9U) {
                        cc = 13U; /* Short Packet */
                    }
                }

                if (is_bulk) {
                    dev->toggle_in = (uint8_t)((hp >> 1) & 1UL);
                    dev->async_bulk_active = FALSE;
                } else {
                    dev->toggle_int = (uint8_t)((hp >> 1) & 1UL);
                    dev->async_int_active = FALSE;
                }

                slot->in_use = FALSE;
                slot->dev    = NULL;

                /* Clear WritebackDoneHead status if set */
                ohci_write32(hc, OHCI_INTR_STATUS, 0x0000007FUL);
                usb_core_dispatch_async(dev, ep_dci, cc, residual);
            } else if (slot->is_bulk) {
                /* Keep bulk list filled for async Bulk IN polling */
                ohci_write32(hc, OHCI_CMD_STATUS, OHCI_CMD_BLF);
            }
        }

        /* 2. Hotplug detection on OHCI root hub ports */
        if (do_hotplug) {
            uint32_t p;
            for (p = 0; p < hc->num_ports; p++) {
                uint32_t port_off = OHCI_RH_PORT_BASE + (p * 4U);
                uint32_t st = ohci_read32(hc, port_off);
                uint32_t bit = (1U << p);

                if ((st & OHCI_PORT_CCS) == 0UL) {
                    if ((hc->port_conn_mask & bit) != 0U) {
                        hc->port_conn_mask &= ~bit;
                        ohci_write32(hc, port_off, OHCI_PORT_CSC | OHCI_PORT_PESC);
                        usb_core_disconnect_port(USB_HCI_OHCI, hc->hc_idx,
                                                 (uint8_t)(p + 1U), 0U, 0U);
                    }
                } else if ((hc->port_conn_mask & bit) == 0U && xhci_has_free_slot()) {
                    hc->port_conn_mask |= bit;
                    pit_mdelay(30U);
                    ohci_enumerate_port(hc, p);
                }
            }
        }
    }
}

static int ohci_init_one(const struct pci_device *pci_dev, uint8_t hc_idx)
{
    struct ohci_hc *hc = &ohci_hcs[hc_idx];
    uint32_t rev;
    uint32_t ctrl;
    uint32_t fi;
    uint32_t fsmps;
    uint32_t desc_a;
    uint32_t pwr_delay;
    uint32_t i;

    if (pci_dev->bar0_mmio == 0UL) {
        return -1;
    }

    memset(hc, 0, sizeof(*hc));
    hc->hc_idx = hc_idx;
    hc->mmio   = (volatile uint8_t *)(uintptr_t)pci_dev->bar0_mmio;

    rev = ohci_read32(hc, OHCI_REVISION);
    if (rev == 0xFFFFFFFFUL || (rev & 0xFFU) == 0U) {
        return -1;
    }

    /* SMM/BIOS ownership handoff (OHCI 1.0a §5.1.1.3) */
    ctrl = ohci_read32(hc, OHCI_CONTROL);
    if ((ctrl & OHCI_CTRL_IR) != 0UL) {
        ohci_write32(hc, OHCI_CMD_STATUS, OHCI_CMD_OCR);
        for (i = 0; i < 500U; i++) {
            if ((ohci_read32(hc, OHCI_CONTROL) & OHCI_CTRL_IR) == 0UL) {
                break;
            }
            pit_mdelay(1U);
        }
    } else if ((ctrl & OHCI_CTRL_HCFS_MASK) != 0UL) {
        ohci_write32(hc, OHCI_CONTROL, 0UL);
        pit_mdelay(50U);
    }

    /* Save FrameInterval before software reset */
    fi = ohci_read32(hc, OHCI_FM_INTERVAL) & 0x3FFFUL;
    if (fi == 0UL) {
        fi = 0x2EDFUL;
    }

    /* Host Controller Software Reset */
    ohci_write32(hc, OHCI_CMD_STATUS, OHCI_CMD_HCR);
    for (i = 0; i < 50U; i++) {
        if ((ohci_read32(hc, OHCI_CMD_STATUS) & OHCI_CMD_HCR) == 0UL) {
            break;
        }
        pit_mdelay(1U);
    }

    /* Allocate HCCA, EDs, TDs, and sync buffers */
    hc->hcca          = (struct ohci_hcca *)xhci_dma_alloc(sizeof(struct ohci_hcca), 256U);
    hc->int_head_ed   = (struct ohci_ed *)xhci_dma_alloc(sizeof(struct ohci_ed), 16U);
    hc->bulk_head_ed  = (struct ohci_ed *)xhci_dma_alloc(sizeof(struct ohci_ed), 16U);
    hc->sync_ctrl_ed  = (struct ohci_ed *)xhci_dma_alloc(sizeof(struct ohci_ed), 16U);
    hc->sync_bulk_ed  = (struct ohci_ed *)xhci_dma_alloc(sizeof(struct ohci_ed), 16U);
    hc->sync_tds      = (struct ohci_td *)xhci_dma_alloc(OHCI_SYNC_TDS * sizeof(struct ohci_td), 16U);
    hc->sync_tail_td  = (struct ohci_td *)xhci_dma_alloc(sizeof(struct ohci_td), 16U);
    hc->setup_buf     = (uint8_t *)xhci_dma_alloc(64U, 16U);
    hc->sync_data_buf = (uint8_t *)xhci_dma_alloc(1024U, 16U);
    if (hc->hcca == NULL || hc->int_head_ed == NULL || hc->bulk_head_ed == NULL ||
        hc->sync_ctrl_ed == NULL || hc->sync_bulk_ed == NULL || hc->sync_tds == NULL ||
        hc->sync_tail_td == NULL || hc->setup_buf == NULL || hc->sync_data_buf == NULL) {
        return -1;
    }

    hc->int_head_ed->ctrl  = OHCI_ED_SKIP;
    hc->bulk_head_ed->ctrl = OHCI_ED_SKIP;
    hc->sync_ctrl_ed->ctrl = OHCI_ED_SKIP;
    hc->sync_bulk_ed->ctrl = OHCI_ED_SKIP;

    /* Slots 0..2 = Interrupt IN (linked on Periodic list), Slot 3 = Async Bulk IN (on Bulk list) */
    for (i = 0; i < OHCI_ASYNC_SLOTS; i++) {
        hc->async_slots[i].is_bulk = (i == OHCI_ASYNC_SLOTS - 1U) ? TRUE : FALSE;
        hc->async_slots[i].ed      = (struct ohci_ed *)xhci_dma_alloc(sizeof(struct ohci_ed), 16U);
        hc->async_slots[i].td      = (struct ohci_td *)xhci_dma_alloc(sizeof(struct ohci_td), 16U);
        hc->async_slots[i].tail_td = (struct ohci_td *)xhci_dma_alloc(sizeof(struct ohci_td), 16U);
        if (hc->async_slots[i].ed == NULL || hc->async_slots[i].td == NULL ||
            hc->async_slots[i].tail_td == NULL) {
            return -1;
        }
        hc->async_slots[i].ed->ctrl = OHCI_ED_SKIP;
    }

    /* Link Interrupt EDs: HCCA->int_table[*] -> int_head_ed -> slot[0] -> slot[1] -> slot[2] */
    hc->int_head_ed->next_ed = (uint32_t)(uintptr_t)hc->async_slots[0].ed;
    hc->async_slots[0].ed->next_ed = (uint32_t)(uintptr_t)hc->async_slots[1].ed;
    hc->async_slots[1].ed->next_ed = (uint32_t)(uintptr_t)hc->async_slots[2].ed;
    hc->async_slots[2].ed->next_ed = 0U;
    for (i = 0; i < 32U; i++) {
        hc->hcca->int_table[i] = (uint32_t)(uintptr_t)hc->int_head_ed;
    }

    /* Link Bulk EDs: HcBulkHeadED -> bulk_head_ed -> sync_bulk_ed -> slot[3] (async bulk IN) */
    hc->bulk_head_ed->next_ed      = (uint32_t)(uintptr_t)hc->sync_bulk_ed;
    hc->sync_bulk_ed->next_ed      = (uint32_t)(uintptr_t)hc->async_slots[3].ed;
    hc->async_slots[3].ed->next_ed = 0U;

    ohci_write32(hc, OHCI_INTR_DISABLE, 0x8000007FUL);
    ohci_write32(hc, OHCI_HCCA, (uint32_t)(uintptr_t)hc->hcca);
    ohci_write32(hc, OHCI_CTRL_HEAD_ED, 0U);
    ohci_write32(hc, OHCI_BULK_HEAD_ED, (uint32_t)(uintptr_t)hc->bulk_head_ed);

    fsmps = ((fi - 210UL) * 6UL) / 7UL;
    ohci_write32(hc, OHCI_FM_INTERVAL, (fsmps << 16) | fi);
    ohci_write32(hc, OHCI_PERIODIC_START, (fi * 9UL) / 10UL);

    /* Transition to Operational state with Periodic, Control, and Bulk lists enabled */
    ohci_write32(hc, OHCI_CONTROL, OHCI_CTRL_HCFS_OPER | OHCI_CTRL_PLE |
                                   OHCI_CTRL_CLE | OHCI_CTRL_BLE);

    desc_a = ohci_read32(hc, OHCI_RH_DESC_A);
    hc->num_ports = desc_a & 0xFFU;
    if (hc->num_ports > 15U) {
        hc->num_ports = 15U;
    }
    pwr_delay = ((desc_a >> 24) & 0xFFU) * 2U;
    if (pwr_delay < 25U) {
        pwr_delay = 25U;
    }

    /* Power on all root hub ports */
    ohci_write32(hc, OHCI_RH_STATUS, (1UL << 16)); /* LPSC: SetGlobalPower */
    for (i = 0; i < hc->num_ports; i++) {
        ohci_write32(hc, OHCI_RH_PORT_BASE + (i * 4U), OHCI_PORT_PPS);
    }
    pit_mdelay(pwr_delay);

    hc->present = TRUE;
    kprintf("[OHCI] Host controller #%u running (MMIO=0x%08x, rev=0x%02x, ports=%u)\n",
            (uint32_t)hc_idx, pci_dev->bar0_mmio, (uint32_t)(rev & 0xFFU), hc->num_ports);

    for (i = 0; i < hc->num_ports && xhci_has_free_slot(); i++) {
        uint32_t st = ohci_read32(hc, OHCI_RH_PORT_BASE + (i * 4U));
        if ((st & OHCI_PORT_CCS) != 0UL) {
            hc->port_conn_mask |= (1U << i);
            ohci_enumerate_port(hc, i);
        }
    }

    usb_core_record_controller(USB_HCI_OHCI, pci_dev, hc->num_ports);
    return 0;
}

int ohci_init_all(void)
{
    struct pci_device pci_dev;
    int idx;

    ohci_count = 0;
    memset(ohci_hcs, 0, sizeof(ohci_hcs));

    for (idx = 0; idx < USB_MAX_HC_INST; idx++) {
        if (pci_find_class_nth(0x0CU, 0x03U, 0x10U, idx, &pci_dev) < 0) {
            break;
        }
        if (ohci_init_one(&pci_dev, (uint8_t)ohci_count) == 0) {
            ohci_count++;
        }
    }
    ohci_last_hotplug_tick = pit_get_ticks();
    return ohci_count;
}
