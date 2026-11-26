/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - UHCI USB 1.1 Host Controller Driver (PCI 0x0C/0x03/0x00)
 */

#include "xhci_internal.h"
#include "pci.h"
#include "pit.h"
#include "io.h"
#include "serial.h"
#include "string.h"

/* UHCI I/O register offsets */
#define UHCI_USBCMD     0x00U
#define UHCI_USBSTS     0x02U
#define UHCI_USBINTR    0x04U
#define UHCI_FRNUM      0x06U
#define UHCI_FRBASEADD  0x08U
#define UHCI_SOFMOD     0x0CU
#define UHCI_PORTSC1    0x10U

/* UHCI USBCMD bits */
#define UHCI_CMD_RS      (1U << 0)
#define UHCI_CMD_HCRESET (1U << 1)
#define UHCI_CMD_GRESET  (1U << 2)
#define UHCI_CMD_CF      (1U << 6)
#define UHCI_CMD_MAXP    (1U << 7)

/* UHCI PORTSC bits */
#define UHCI_PORT_CCS   (1U << 0)
#define UHCI_PORT_CSC   (1U << 1)
#define UHCI_PORT_PE    (1U << 2)
#define UHCI_PORT_PEC   (1U << 3)
#define UHCI_PORT_LSDA  (1U << 8)
#define UHCI_PORT_PR    (1U << 9)
#define UHCI_PORT_RW1C  (UHCI_PORT_CSC | UHCI_PORT_PEC)

/* UHCI TD ctrl_status bits (exclude bit 19 NAK Received from fatal error mask) */
#define UHCI_TD_STALLED (1UL << 22)
#define UHCI_TD_ACTIVE  (1UL << 23)
#define UHCI_TD_LS      (1UL << 26)
#define UHCI_TD_CERR3   (3UL << 27)
#define UHCI_TD_SPD     (1UL << 29)
#define UHCI_TD_ERRMASK (0x00760000UL)

/* USB PIDs */
#define USB_PID_SETUP   0x2DU
#define USB_PID_IN      0x69U
#define USB_PID_OUT     0xE1U

#define UHCI_SYNC_TDS   68U
#define UHCI_ASYNC_SLOTS 4U
#define UHCI_ASYNC_TDS  36U

struct uhci_qh {
    volatile uint32_t head_link;
    volatile uint32_t elem_link;
    uint32_t          pad0;
    uint32_t          pad1;
};

struct uhci_td {
    volatile uint32_t link;
    volatile uint32_t ctrl_status;
    volatile uint32_t token;
    volatile uint32_t buffer;
};

struct uhci_async_slot {
    bool_t               in_use;
    bool_t               is_bulk;
    struct xhci_usb_dev *dev;
    uint8_t              ep_dci;
    uint32_t             num_tds;
    uint32_t             total_len;
    struct uhci_qh      *qh;
    struct uhci_td      *tds;
};

struct uhci_hc {
    bool_t                 present;
    uint8_t                hc_idx;
    uint16_t               io_base;
    uint32_t               num_ports;
    uint32_t               port_conn_mask;
    uint32_t              *frame_list;
    struct uhci_qh        *int_qh;
    struct uhci_qh        *async_qh;
    struct uhci_td        *sync_tds;
    uint8_t               *setup_buf;
    uint8_t               *sync_data_buf;
    struct uhci_async_slot async_slots[UHCI_ASYNC_SLOTS];
};

static struct uhci_hc uhci_hcs[USB_MAX_HC_INST];
static int            uhci_count = 0;
static uint32_t       uhci_last_hotplug_tick = 0;

static uint32_t uhci_make_token(uint8_t pid, uint8_t addr, uint8_t ep,
                                uint8_t toggle, uint32_t len)
{
    uint32_t max_len = (len == 0U) ? 0x7FFUL : ((len - 1U) & 0x7FFUL);
    return (uint32_t)pid |
           (((uint32_t)addr & 0x7FUL) << 8) |
           (((uint32_t)ep & 0x0FUL) << 15) |
           (((uint32_t)toggle & 1UL) << 19) |
           (max_len << 21);
}

static uint32_t uhci_td_actlen(uint32_t ctrl_status)
{
    uint32_t raw = ctrl_status & 0x7FFUL;
    return (raw == 0x7FFUL) ? 0U : (raw + 1U);
}

static int uhci_run_sync_chain(struct uhci_hc *hc, uint32_t num_tds,
                               uint32_t *out_completed, uint32_t *out_err_status)
{
    int timeout = 40000;
    uint32_t i;
    uint32_t err_st = 0;
    uint32_t done_cnt = 0;

    cpu_memory_barrier();
    hc->async_qh->elem_link = (uint32_t)(uintptr_t)&hc->sync_tds[0];
    cpu_memory_barrier();

    while (timeout > 0) {
        uint32_t elem = hc->async_qh->elem_link;
        if ((elem & 1U) != 0U) {
            break;
        }
        /* Check if current TD stopped due to short packet or error */
        {
            uint32_t cur_phys = elem & ~0x0FUL;
            struct uhci_td *cur_td = (struct uhci_td *)(uintptr_t)cur_phys;
            if ((cur_td->ctrl_status & UHCI_TD_ACTIVE) == 0UL) {
                break;
            }
        }
        cpu_pause();
        if ((timeout & 0x1FU) == 0) {
            pit_udelay(25U);
        }
        timeout--;
    }

    hc->async_qh->elem_link = 1U;
    cpu_memory_barrier();

    /* Clear any pending USBSTS status bits */
    outw((uint16_t)(hc->io_base + UHCI_USBSTS), 0x001FU);

    for (i = 0; i < num_tds; i++) {
        uint32_t st = hc->sync_tds[i].ctrl_status;
        if ((st & UHCI_TD_ACTIVE) != 0UL) {
            if (timeout <= 0) {
                err_st = UHCI_TD_ACTIVE;
            }
            break;
        }
        if ((st & UHCI_TD_ERRMASK) != 0UL) {
            err_st = st;
            break;
        }
        done_cnt++;
    }

    if (out_completed != NULL) {
        *out_completed = done_cnt;
    }
    if (out_err_status != NULL) {
        *out_err_status = err_st;
    }
    if ((err_st & UHCI_TD_STALLED) != 0UL) {
        return -2;
    }
    if (err_st != 0UL || timeout <= 0) {
        return -1;
    }
    return 0;
}

int uhci_control_transfer(struct xhci_usb_dev *dev,
                          uint8_t bmRequestType, uint8_t bRequest,
                          uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                          void *data)
{
    struct uhci_hc *hc;
    uint32_t ls_flag;
    uint32_t td_idx = 0;
    uint16_t mps;
    bool_t is_in = ((bmRequestType & 0x80U) != 0U) ? TRUE : FALSE;
    uint32_t off = 0;
    uint8_t toggle = 1U;
    uint32_t i;
    int rc;

    if (dev == NULL || dev->hc_idx >= USB_MAX_HC_INST || wLength > 1024U) {
        return -1;
    }
    hc = &uhci_hcs[dev->hc_idx];
    if (!hc->present) {
        return -1;
    }

    ls_flag = (dev->speed == 2U) ? UHCI_TD_LS : 0UL;
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
    hc->sync_tds[td_idx].ctrl_status = UHCI_TD_ACTIVE | UHCI_TD_CERR3 | ls_flag;
    hc->sync_tds[td_idx].token       = uhci_make_token(USB_PID_SETUP, dev->usb_addr, 0U, 0U, 8U);
    hc->sync_tds[td_idx].buffer      = (uint32_t)(uintptr_t)hc->setup_buf;
    td_idx++;

    /* 2. DATA TDs (DATA1, alternating) */
    while (off < (uint32_t)wLength && td_idx < UHCI_SYNC_TDS - 1U) {
        uint32_t chunk = (uint32_t)wLength - off;
        if (chunk > (uint32_t)mps) {
            chunk = (uint32_t)mps;
        }
        hc->sync_tds[td_idx].ctrl_status = UHCI_TD_ACTIVE | UHCI_TD_CERR3 | ls_flag |
                                           (is_in ? UHCI_TD_SPD : 0UL);
        hc->sync_tds[td_idx].token       = uhci_make_token(is_in ? USB_PID_IN : USB_PID_OUT,
                                                           dev->usb_addr, 0U, toggle, chunk);
        hc->sync_tds[td_idx].buffer      = (uint32_t)(uintptr_t)&hc->sync_data_buf[off];
        td_idx++;
        off += chunk;
        toggle ^= 1U;
    }

    /* 3. STATUS TD (DATA1, opposite direction of data stage or IN if no data) */
    {
        uint8_t status_pid = (wLength == 0U || !is_in) ? USB_PID_IN : USB_PID_OUT;
        hc->sync_tds[td_idx].ctrl_status = UHCI_TD_ACTIVE | UHCI_TD_CERR3 | ls_flag;
        hc->sync_tds[td_idx].token       = uhci_make_token(status_pid, dev->usb_addr, 0U, 1U, 0U);
        hc->sync_tds[td_idx].buffer      = 0U;
        td_idx++;
    }

    for (i = 0; i < td_idx; i++) {
        if (i + 1U < td_idx) {
            hc->sync_tds[i].link = ((uint32_t)(uintptr_t)&hc->sync_tds[i + 1U]) | 4U; /* Depth-first */
        } else {
            hc->sync_tds[i].link = 1U; /* Terminate */
        }
    }

    rc = uhci_run_sync_chain(hc, td_idx, NULL, NULL);
    /* If DATA IN stopped early on a short packet, execute the STATUS OUT stage alone */
    if (rc == 0 && is_in && wLength > 0U &&
        (hc->sync_tds[td_idx - 1U].ctrl_status & UHCI_TD_ACTIVE) != 0UL) {
        hc->sync_tds[0].link        = 1U;
        hc->sync_tds[0].ctrl_status = UHCI_TD_ACTIVE | UHCI_TD_CERR3 | ls_flag;
        hc->sync_tds[0].token       = uhci_make_token(USB_PID_OUT, dev->usb_addr, 0U, 1U, 0U);
        hc->sync_tds[0].buffer      = 0U;
        rc = uhci_run_sync_chain(hc, 1U, NULL, NULL);
    }

    if (rc < 0) {
        return -1;
    }
    if (wLength > 0U && is_in && data != NULL) {
        memcpy(data, hc->sync_data_buf, wLength);
    }
    return 0;
}

void uhci_clear_bulk_stall(struct xhci_usb_dev *dev, bool_t is_in)
{
    uint8_t dci;
    uint8_t ep_addr;

    if (dev == NULL) {
        return;
    }
    dci = is_in ? dev->bulk_in_dci : dev->bulk_out_dci;
    ep_addr = (uint8_t)((dci / 2U) | (is_in ? 0x80U : 0x00U));
    (void)uhci_control_transfer(dev, 0x02U, 0x01U, 0U, ep_addr, 0U, NULL);
    if (is_in) {
        dev->toggle_in = 0U;
    } else {
        dev->toggle_out = 0U;
    }
}

int uhci_bulk_transfer(struct xhci_usb_dev *dev, bool_t is_in,
                       void *buf, uint32_t len)
{
    struct uhci_hc *hc;
    uint8_t  dci;
    uint8_t  ep_num;
    uint16_t mps;
    uint8_t  pid;
    uint8_t *toggle_ptr;
    uint8_t *p = (uint8_t *)buf;
    uint32_t remaining = len;

    if (dev == NULL || dev->hc_idx >= USB_MAX_HC_INST) {
        return -1;
    }
    hc = &uhci_hcs[dev->hc_idx];
    if (!hc->present) {
        return -1;
    }

    dci        = is_in ? dev->bulk_in_dci : dev->bulk_out_dci;
    ep_num     = (uint8_t)(dci / 2U);
    mps        = is_in ? dev->bulk_in_mps : dev->bulk_out_mps;
    if (mps == 0U || mps > 64U) {
        mps = 64U;
    }
    pid        = is_in ? USB_PID_IN : USB_PID_OUT;
    toggle_ptr = is_in ? &dev->toggle_in : &dev->toggle_out;

    do {
        uint32_t td_cnt = 0U;
        uint32_t batch_bytes = 0U;
        uint32_t i;
        uint32_t completed = 0U;
        uint32_t err_st = 0U;
        int rc;

        if (remaining == 0U) {
            /* Zero-length packet */
            hc->sync_tds[0].link        = 1U;
            hc->sync_tds[0].ctrl_status = UHCI_TD_ACTIVE | UHCI_TD_CERR3;
            hc->sync_tds[0].token       = uhci_make_token(pid, dev->usb_addr, ep_num,
                                                          *toggle_ptr, 0U);
            hc->sync_tds[0].buffer      = 0U;
            td_cnt = 1U;
        } else {
            uint8_t t = *toggle_ptr;
            while (batch_bytes < remaining && td_cnt < 64U) {
                uint32_t chunk = remaining - batch_bytes;
                if (chunk > (uint32_t)mps) {
                    chunk = (uint32_t)mps;
                }
                hc->sync_tds[td_cnt].ctrl_status = UHCI_TD_ACTIVE | UHCI_TD_CERR3 |
                                                   (is_in ? UHCI_TD_SPD : 0UL);
                hc->sync_tds[td_cnt].token       = uhci_make_token(pid, dev->usb_addr,
                                                                   ep_num, t, chunk);
                hc->sync_tds[td_cnt].buffer      = (uint32_t)(uintptr_t)(p + batch_bytes);
                batch_bytes += chunk;
                td_cnt++;
                t ^= 1U;
            }
            for (i = 0; i < td_cnt; i++) {
                hc->sync_tds[i].link = (i + 1U < td_cnt)
                                           ? (((uint32_t)(uintptr_t)&hc->sync_tds[i + 1U]) | 4U)
                                           : 1U;
            }
        }

        rc = uhci_run_sync_chain(hc, td_cnt, &completed, &err_st);
        *toggle_ptr = (uint8_t)((*toggle_ptr + completed) & 1U);

        if (rc == -2) {
            uhci_clear_bulk_stall(dev, is_in);
            return -2;
        }
        if (rc < 0) {
            return -1;
        }

        /* On short IN packet, stop cleanly */
        if (is_in && completed < td_cnt) {
            break;
        }
        p += batch_bytes;
        remaining -= batch_bytes;
    } while (remaining > 0U);

    return 0;
}

static struct uhci_async_slot *uhci_get_async_slot(struct uhci_hc *hc,
                                                   struct xhci_usb_dev *dev,
                                                   bool_t is_bulk)
{
    uint32_t i;
    for (i = 0; i < UHCI_ASYNC_SLOTS; i++) {
        if (hc->async_slots[i].in_use &&
            hc->async_slots[i].dev == dev &&
            hc->async_slots[i].is_bulk == is_bulk) {
            return &hc->async_slots[i];
        }
    }
    for (i = 0; i < UHCI_ASYNC_SLOTS; i++) {
        if (!hc->async_slots[i].in_use) {
            hc->async_slots[i].in_use  = TRUE;
            hc->async_slots[i].is_bulk = is_bulk;
            hc->async_slots[i].dev     = dev;
            return &hc->async_slots[i];
        }
    }
    return NULL;
}

static void uhci_submit_async_in(struct xhci_usb_dev *dev, bool_t is_bulk,
                                 void *buf, uint32_t len)
{
    struct uhci_hc *hc;
    struct uhci_async_slot *slot;
    uint8_t  dci;
    uint8_t  ep_num;
    uint16_t mps;
    uint32_t ls_flag;
    uint32_t off = 0U;
    uint32_t td_cnt = 0U;
    uint8_t  t;
    uint32_t i;

    if (dev == NULL || dev->hc_idx >= USB_MAX_HC_INST || buf == NULL || len == 0U) {
        return;
    }
    hc = &uhci_hcs[dev->hc_idx];
    if (!hc->present) {
        return;
    }
    slot = uhci_get_async_slot(hc, dev, is_bulk);
    if (slot == NULL) {
        return;
    }

    slot->qh->elem_link = 1U;
    cpu_memory_barrier();

    dci     = is_bulk ? dev->bulk_in_dci : dev->int_in_dci;
    ep_num  = (uint8_t)(dci / 2U);
    mps     = is_bulk ? dev->bulk_in_mps : dev->int_in_mps;
    if (mps == 0U || mps > 64U) {
        mps = is_bulk ? 64U : 8U;
    }
    ls_flag = (dev->speed == 2U) ? UHCI_TD_LS : 0UL;
    t       = is_bulk ? dev->toggle_in : dev->toggle_int;

    while (off < len && td_cnt < UHCI_ASYNC_TDS) {
        uint32_t chunk = len - off;
        if (chunk > (uint32_t)mps) {
            chunk = (uint32_t)mps;
        }
        slot->tds[td_cnt].ctrl_status = UHCI_TD_ACTIVE | UHCI_TD_CERR3 | UHCI_TD_SPD | ls_flag;
        slot->tds[td_cnt].token       = uhci_make_token(USB_PID_IN, dev->usb_addr, ep_num, t, chunk);
        slot->tds[td_cnt].buffer      = (uint32_t)(uintptr_t)((uint8_t *)buf + off);
        off += chunk;
        td_cnt++;
        t ^= 1U;
    }
    for (i = 0; i < td_cnt; i++) {
        slot->tds[i].link = (i + 1U < td_cnt)
                                ? (((uint32_t)(uintptr_t)&slot->tds[i + 1U]) | 4U)
                                : 1U;
    }

    slot->ep_dci    = dci;
    slot->num_tds   = td_cnt;
    slot->total_len = off;

    if (is_bulk) {
        dev->async_bulk_active = TRUE;
        dev->async_bulk_buf    = buf;
        dev->async_bulk_len    = off;
    } else {
        dev->async_int_active = TRUE;
        dev->async_int_buf    = buf;
        dev->async_int_len    = off;
    }

    cpu_memory_barrier();
    slot->qh->elem_link = (uint32_t)(uintptr_t)&slot->tds[0];
    cpu_memory_barrier();
}

void uhci_interrupt_in_submit(struct xhci_usb_dev *dev, void *buf, uint32_t len)
{
    uhci_submit_async_in(dev, FALSE, buf, len);
}

void uhci_bulk_in_submit(struct xhci_usb_dev *dev, void *buf, uint32_t len)
{
    uhci_submit_async_in(dev, TRUE, buf, len);
}

void uhci_release_device(struct xhci_usb_dev *dev)
{
    struct uhci_hc *hc;
    uint32_t i;

    if (dev == NULL || dev->hc_idx >= USB_MAX_HC_INST) {
        return;
    }
    hc = &uhci_hcs[dev->hc_idx];
    for (i = 0; i < UHCI_ASYNC_SLOTS; i++) {
        if (hc->async_slots[i].in_use && hc->async_slots[i].dev == dev) {
            hc->async_slots[i].qh->elem_link = 1U;
            hc->async_slots[i].in_use        = FALSE;
            hc->async_slots[i].dev           = NULL;
        }
    }
    dev->async_int_active  = FALSE;
    dev->async_bulk_active = FALSE;
}

static void uhci_enumerate_port(struct uhci_hc *hc, uint32_t port_idx)
{
    uint16_t port_reg = (uint16_t)(hc->io_base + UHCI_PORTSC1 + (port_idx * 2U));
    uint16_t st;
    uint8_t  speed;
    int      wait;

    /* Assert Port Reset (50 ms) */
    st = inw(port_reg);
    outw(port_reg, (uint16_t)((st & ~UHCI_PORT_RW1C) | UHCI_PORT_PR));
    pit_mdelay(50U);

    /* Deassert Port Reset */
    st = inw(port_reg);
    outw(port_reg, (uint16_t)(st & ~(UHCI_PORT_PR | UHCI_PORT_RW1C)));
    pit_mdelay(10U);

    /* Enable port and clear CSC/PEC */
    for (wait = 0; wait < 10; wait++) {
        st = inw(port_reg);
        if ((st & UHCI_PORT_CCS) == 0U) {
            return;
        }
        outw(port_reg, (uint16_t)(st | UHCI_PORT_PE | UHCI_PORT_RW1C));
        pit_mdelay(5U);
        st = inw(port_reg);
        if ((st & UHCI_PORT_PE) != 0U) {
            break;
        }
    }
    pit_mdelay(15U);

    st = inw(port_reg);
    if ((st & (UHCI_PORT_CCS | UHCI_PORT_PE)) != (UHCI_PORT_CCS | UHCI_PORT_PE)) {
        return;
    }

    speed = ((st & UHCI_PORT_LSDA) != 0U) ? 2U : 1U;
    (void)usb_core_enumerate_device(USB_HCI_UHCI, hc->hc_idx,
                                    (uint8_t)(port_idx + 1U), 0U, 0U,
                                    speed, 0U, 0U);
}

void uhci_poll(void)
{
    int h;
    uint32_t now_ticks = pit_get_ticks();
    bool_t do_hotplug = ((now_ticks - uhci_last_hotplug_tick) >= 250U) ? TRUE : FALSE;

    if (do_hotplug) {
        uhci_last_hotplug_tick = now_ticks;
    }

    for (h = 0; h < uhci_count; h++) {
        struct uhci_hc *hc = &uhci_hcs[h];
        uint32_t i;

        if (!hc->present) {
            continue;
        }

        /* 1. Poll async Interrupt IN and Bulk IN slots */
        for (i = 0; i < UHCI_ASYNC_SLOTS; i++) {
            struct uhci_async_slot *slot = &hc->async_slots[i];
            uint32_t elem;
            bool_t finished = FALSE;

            if (!slot->in_use || slot->dev == NULL || slot->num_tds == 0U) {
                continue;
            }
            elem = slot->qh->elem_link;
            if ((elem & 1U) != 0U) {
                finished = TRUE;
            } else {
                struct uhci_td *cur_td = (struct uhci_td *)(uintptr_t)(elem & ~0x0FUL);
                if ((cur_td->ctrl_status & UHCI_TD_ACTIVE) == 0UL) {
                    finished = TRUE;
                }
            }

            if (finished) {
                struct xhci_usb_dev *dev = slot->dev;
                uint8_t  ep_dci = slot->ep_dci;
                bool_t   is_bulk = slot->is_bulk;
                uint32_t total_len = slot->total_len;
                uint32_t actual = 0U;
                uint32_t completed = 0U;
                uint8_t  cc = 1U;
                uint32_t t;

                slot->qh->elem_link = 1U;
                cpu_memory_barrier();

                for (t = 0; t < slot->num_tds; t++) {
                    uint32_t st = slot->tds[t].ctrl_status;
                    if ((st & UHCI_TD_ACTIVE) != 0UL) {
                        break;
                    }
                    if ((st & UHCI_TD_STALLED) != 0UL) {
                        cc = 6U;
                        break;
                    }
                    if ((st & UHCI_TD_ERRMASK) != 0UL) {
                        cc = 4U;
                        break;
                    }
                    actual += uhci_td_actlen(st);
                    completed++;
                }

                if (cc == 1U && actual < total_len) {
                    cc = 13U; /* Short Packet */
                }
                if (is_bulk) {
                    dev->toggle_in = (uint8_t)((dev->toggle_in + completed) & 1U);
                    dev->async_bulk_active = FALSE;
                } else {
                    dev->toggle_int = (uint8_t)((dev->toggle_int + completed) & 1U);
                    dev->async_int_active = FALSE;
                }

                slot->in_use = FALSE;
                slot->dev    = NULL;

                /* Clear USBSTS status bits before callback may re-submit */
                outw((uint16_t)(hc->io_base + UHCI_USBSTS), 0x001FU);
                usb_core_dispatch_async(dev, ep_dci, cc,
                                        (actual <= total_len) ? (total_len - actual) : 0U);
            }
        }

        /* 2. Hotplug detection on UHCI root ports */
        if (do_hotplug) {
            uint32_t p;
            for (p = 0; p < hc->num_ports; p++) {
                uint16_t port_reg = (uint16_t)(hc->io_base + UHCI_PORTSC1 + (p * 2U));
                uint16_t st = inw(port_reg);
                uint32_t bit = (1U << p);

                if ((st & UHCI_PORT_CCS) == 0U) {
                    if ((hc->port_conn_mask & bit) != 0U) {
                        hc->port_conn_mask &= ~bit;
                        outw(port_reg, (uint16_t)((st & ~UHCI_PORT_PE) | UHCI_PORT_RW1C));
                        usb_core_disconnect_port(USB_HCI_UHCI, hc->hc_idx,
                                                 (uint8_t)(p + 1U), 0U, 0U);
                    }
                } else if ((hc->port_conn_mask & bit) == 0U && xhci_has_free_slot()) {
                    hc->port_conn_mask |= bit;
                    pit_mdelay(30U);
                    uhci_enumerate_port(hc, p);
                }
            }
        }
    }
}

static int uhci_init_one(const struct pci_device *pci_dev, uint8_t hc_idx)
{
    struct uhci_hc *hc = &uhci_hcs[hc_idx];
    uint16_t io = pci_dev->bar4_io;
    uint32_t leg;
    uint32_t i;

    if (io == 0U) {
        return -1;
    }

    memset(hc, 0, sizeof(*hc));
    hc->hc_idx    = hc_idx;
    hc->io_base   = io;
    hc->num_ports = 2U;

    /* Disable BIOS Legacy Keyboard/Mouse SMI emulation (PCI USBLEGSUP at 0xC0) */
    leg = pci_read32(pci_dev->bus, pci_dev->slot, pci_dev->func, 0xC0U);
    pci_write32(pci_dev->bus, pci_dev->slot, pci_dev->func, 0xC0U,
                (leg & 0xFFFF0000UL) | 0x8F00UL);

    /* Global Reset (15 ms), then Host Controller Reset */
    outw((uint16_t)(io + UHCI_USBCMD), UHCI_CMD_GRESET);
    pit_mdelay(15U);
    outw((uint16_t)(io + UHCI_USBCMD), 0U);
    pit_mdelay(5U);

    outw((uint16_t)(io + UHCI_USBCMD), UHCI_CMD_HCRESET);
    for (i = 0; i < 100U; i++) {
        if ((inw((uint16_t)(io + UHCI_USBCMD)) & UHCI_CMD_HCRESET) == 0U) {
            break;
        }
        pit_mdelay(1U);
    }

    outw((uint16_t)(io + UHCI_USBINTR), 0U);
    outw((uint16_t)(io + UHCI_USBSTS), 0xFFFFU);
    outw((uint16_t)(io + UHCI_FRNUM), 0U);
    outb((uint16_t)(io + UHCI_SOFMOD), 64U);

    hc->frame_list    = (uint32_t *)xhci_dma_alloc(4096U, 4096U);
    hc->int_qh        = (struct uhci_qh *)xhci_dma_alloc(sizeof(struct uhci_qh), 16U);
    hc->async_qh      = (struct uhci_qh *)xhci_dma_alloc(sizeof(struct uhci_qh), 16U);
    hc->sync_tds      = (struct uhci_td *)xhci_dma_alloc(UHCI_SYNC_TDS * sizeof(struct uhci_td), 16U);
    hc->setup_buf     = (uint8_t *)xhci_dma_alloc(64U, 16U);
    hc->sync_data_buf = (uint8_t *)xhci_dma_alloc(1024U, 16U);
    if (hc->frame_list == NULL || hc->int_qh == NULL || hc->async_qh == NULL ||
        hc->sync_tds == NULL || hc->setup_buf == NULL || hc->sync_data_buf == NULL) {
        return -1;
    }

    hc->async_qh->head_link = 1U;
    hc->async_qh->elem_link = 1U;

    /* Allocate persistent QH + TD pools for async Interrupt IN / Bulk IN transfers */
    for (i = 0; i < UHCI_ASYNC_SLOTS; i++) {
        hc->async_slots[i].qh  = (struct uhci_qh *)xhci_dma_alloc(sizeof(struct uhci_qh), 16U);
        hc->async_slots[i].tds = (struct uhci_td *)xhci_dma_alloc(
                                     UHCI_ASYNC_TDS * sizeof(struct uhci_td), 16U);
        if (hc->async_slots[i].qh == NULL || hc->async_slots[i].tds == NULL) {
            return -1;
        }
        hc->async_slots[i].qh->elem_link = 1U;
    }

    /* Chain: frame_list[*] -> int_qh -> async_slots[0..N-1].qh -> async_qh */
    hc->int_qh->elem_link = 1U;
    hc->int_qh->head_link = ((uint32_t)(uintptr_t)hc->async_slots[0].qh) | 2U;
    for (i = 0; i < UHCI_ASYNC_SLOTS; i++) {
        if (i + 1U < UHCI_ASYNC_SLOTS) {
            hc->async_slots[i].qh->head_link = ((uint32_t)(uintptr_t)hc->async_slots[i + 1U].qh) | 2U;
        } else {
            hc->async_slots[i].qh->head_link = ((uint32_t)(uintptr_t)hc->async_qh) | 2U;
        }
    }
    for (i = 0; i < 1024U; i++) {
        hc->frame_list[i] = ((uint32_t)(uintptr_t)hc->int_qh) | 2U;
    }

    outl((uint16_t)(io + UHCI_FRBASEADD), (uint32_t)(uintptr_t)hc->frame_list);
    outw((uint16_t)(io + UHCI_USBCMD), UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);

    hc->present = TRUE;
    kprintf("[UHCI] Host controller #%u running (IO=0x%04x, ports=%u)\n",
            (uint32_t)hc_idx, (uint32_t)io, hc->num_ports);

    pit_mdelay(25U);
    for (i = 0; i < hc->num_ports && xhci_has_free_slot(); i++) {
        uint16_t st = inw((uint16_t)(io + UHCI_PORTSC1 + (i * 2U)));
        if ((st & UHCI_PORT_CCS) != 0U) {
            hc->port_conn_mask |= (1U << i);
            uhci_enumerate_port(hc, i);
        }
    }

    usb_core_record_controller(USB_HCI_UHCI, pci_dev, hc->num_ports);
    return 0;
}

int uhci_init_all(void)
{
    struct pci_device pci_dev;
    int idx;

    uhci_count = 0;
    memset(uhci_hcs, 0, sizeof(uhci_hcs));

    for (idx = 0; idx < USB_MAX_HC_INST; idx++) {
        if (pci_find_class_nth(0x0CU, 0x03U, 0x00U, idx, &pci_dev) < 0) {
            break;
        }
        if (uhci_init_one(&pci_dev, (uint8_t)uhci_count) == 0) {
            uhci_count++;
        }
    }
    uhci_last_hotplug_tick = pit_get_ticks();
    return uhci_count;
}
