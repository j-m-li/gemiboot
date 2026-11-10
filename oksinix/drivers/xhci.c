/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - xHCI USB 3.0 Host Controller Driver
 * Supports USB Hub, USB HID Keyboard, USB HID Mouse, and USB Mass Storage (BOT/SCSI)
 */

#include "xhci.h"
#include "pci.h"
#include "pit.h"
#include "ps2.h"
#include "io.h"
#include "serial.h"
#include "string.h"

/* xHCI Ring and Pool sizes (512 KiB DMA pool for Scratchpad pages + rings/buffers) */
#define XHCI_RING_TRBS     256
#define XHCI_MAX_SLOTS     16
#define XHCI_DMA_POOL_SIZE 524288U

/* PORTSC Write-1-to-Clear bits: PED (bit 1) and CSC..CEC (bits 17..23) */
#define PORTSC_RW1C_MASK   ((1U << 1) | (0x7FU << 17))

/* xHCI TRB Types */
#define TRB_TYPE_NORMAL            1U
#define TRB_TYPE_SETUP_STAGE       2U
#define TRB_TYPE_DATA_STAGE        3U
#define TRB_TYPE_STATUS_STAGE      4U
#define TRB_TYPE_LINK              6U
#define TRB_TYPE_ENABLE_SLOT       9U
#define TRB_TYPE_DISABLE_SLOT      10U
#define TRB_TYPE_ADDRESS_DEVICE    11U
#define TRB_TYPE_CONFIGURE_EP      12U
#define TRB_TYPE_EVALUATE_CTX      13U
#define TRB_TYPE_RESET_EP          14U
#define TRB_TYPE_SET_TR_DEQ        16U
#define TRB_TYPE_TRANSFER_EVENT    32U
#define TRB_TYPE_CMD_COMPLETION    33U
#define TRB_TYPE_PORT_STATUS_CHG   34U

/* xHCI TRB Flag bits */
#define TRB_CYCLE                  (1U << 0)
#define TRB_LINK_TOGGLE_CYCLE      (1U << 1)
#define TRB_IOC                    (1U << 5)
#define TRB_IDT                    (1U << 6)
#define TRB_DIR_IN                 (1U << 16)

/* USB Hub Class Feature Selectors */
#define HUB_FEAT_PORT_RESET        4U
#define HUB_FEAT_PORT_POWER        8U
#define HUB_FEAT_C_PORT_CONNECTION 16U
#define HUB_FEAT_C_PORT_RESET      20U
#define HUB_FEAT_C_PORT_LINK_STATE 25U
#define HUB_FEAT_BH_PORT_RESET     28U
#define HUB_FEAT_C_BH_PORT_RESET   29U

/* 16-byte xHCI Transfer Request Block (TRB) */
struct xhci_trb {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;
};

/* 16-byte Event Ring Segment Table (ERST) entry */
struct xhci_erst_entry {
    uint32_t base_lo;
    uint32_t base_hi;
    uint32_t size;
    uint32_t reserved;
};

/* Transfer Ring wrapper with enqueue index and cycle state */
struct xhci_ring {
    volatile struct xhci_trb *trbs;
    uint32_t                  enq_idx;
    uint32_t                  ccs;
};

/* Per-device state */
#define USB_DEV_NONE    0
#define USB_DEV_KBD     1
#define USB_DEV_MOUSE   2
#define USB_DEV_STORAGE 3
#define USB_DEV_HUB     4

struct xhci_usb_dev {
    uint32_t         dev_type;
    uint8_t          slot_id;
    uint8_t          root_port;
    uint32_t         route_string;
    uint8_t          speed;
    uint8_t          parent_hub_slot;
    uint8_t          parent_port;
    uint8_t          hub_ports;
    uint8_t          hub_tier;
    uint8_t          hub_ttt;
    uint32_t         hub_port_conn_mask;
    uint8_t          iface_num;
    uint8_t          config_val;
    uint16_t         ep0_mps;
    uint8_t          int_in_dci;
    uint16_t         int_in_mps;
    uint8_t          bulk_in_dci;
    uint16_t         bulk_in_mps;
    uint8_t          bulk_in_burst;
    uint8_t          bulk_out_dci;
    uint16_t         bulk_out_mps;
    uint8_t          bulk_out_burst;
    uint8_t         *out_ctx;
    uint8_t         *in_ctx;
    struct xhci_ring ep0_ring;
    struct xhci_ring int_in_ring;
    struct xhci_ring bulk_in_ring;
    struct xhci_ring bulk_out_ring;
    uint8_t         *hid_buf;
    uint8_t          prev_kbd_report[8];
};

/* DMA Memory Pool */
static uint8_t  xhci_dma_pool[XHCI_DMA_POOL_SIZE];
static uint32_t xhci_dma_off = 0;

/* Controller MMIO pointers */
static volatile uint8_t  *cap_base = NULL;
static volatile uint8_t  *op_base  = NULL;
static volatile uint8_t  *rt_base  = NULL;
static volatile uint32_t *db_base  = NULL;

static uint32_t max_slots = 0;
static uint32_t max_ports = 0;
static uint32_t ctx_size  = 32;
static bool_t   xhci_ready = FALSE;

static volatile uint32_t       *dcbaa = NULL;
static struct xhci_ring         cmd_ring;
static volatile struct xhci_trb *evt_ring = NULL;
static uint32_t                 evt_deq_idx = 0;
static uint32_t                 evt_ccs = 1;
static struct xhci_erst_entry  *erst = NULL;

static struct xhci_usb_dev      usb_devs[XHCI_MAX_SLOTS];
static int                      num_usb_devs = 0;
static uint8_t                 *ctrl_dma_buf = NULL;
static uint8_t                 *msc_cbw_buf  = NULL;
static uint8_t                 *msc_csw_buf  = NULL;
static uint8_t                 *msc_data_buf = NULL;
static uint32_t                 msc_cbw_tag  = 1;

static int                      hub_dev_idx   = -1;
static int                      kbd_dev_idx   = -1;
static int                      mouse_dev_idx = -1;
static int                      msc_dev_indices[XHCI_MAX_MSC_DEVS];
static struct xhci_msc_info     msc_infos[XHCI_MAX_MSC_DEVS];
static int                      num_msc_devs  = 0;
static struct xhci_hub_info     hub_info;
static struct xhci_controller_info ctrl_info;
static uint32_t                 root_port_conn_mask = 0;
static uint32_t                 last_hotplug_tick   = 0;
static bool_t                   hotplug_pending     = FALSE;

/* Forward declarations */
static int  xhci_control_transfer(struct xhci_usb_dev *dev,
                                  uint8_t bmRequestType, uint8_t bRequest,
                                  uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                                  void *data);
static void xhci_enumerate_root_port(uint32_t port_num);
static int  xhci_enumerate_hub_port(struct xhci_usb_dev *hub_dev, uint8_t hp);
static int  xhci_enumerate_device(uint8_t root_port, uint32_t route_string,
                                  uint32_t tier, uint8_t speed,
                                  uint8_t parent_hub_slot, uint8_t parent_port);

static uint32_t mmio_read32(volatile const uint8_t *base, uint32_t offset)
{
    return *(volatile const uint32_t *)(base + offset);
}

static void mmio_write32(volatile uint8_t *base, uint32_t offset, uint32_t val)
{
    *(volatile uint32_t *)(base + offset) = val;
}

static void *dma_alloc(uint32_t size, uint32_t align)
{
    uintptr_t base_addr = (uintptr_t)&xhci_dma_pool[0];
    uintptr_t cur_addr  = base_addr + xhci_dma_off;
    uintptr_t aligned   = (cur_addr + (align - 1U)) & ~(uintptr_t)(align - 1U);

    if ((aligned & 0xFFFF0000UL) != ((aligned + size - 1U) & 0xFFFF0000UL)) {
        aligned = (aligned + 0x10000UL) & 0xFFFF0000UL;
    }

    if ((aligned + size) - base_addr > XHCI_DMA_POOL_SIZE) {
        kprintf("[XHCI] ERROR: DMA pool exhausted!\n");
        return NULL;
    }

    xhci_dma_off = (uint32_t)((aligned + size) - base_addr);
    memset((void *)aligned, 0, size);
    return (void *)aligned;
}

static void ring_init(struct xhci_ring *r)
{
    r->trbs = (volatile struct xhci_trb *)dma_alloc(XHCI_RING_TRBS * sizeof(struct xhci_trb), 64);
    r->enq_idx = 0;
    r->ccs = 1;

    r->trbs[XHCI_RING_TRBS - 1].param_lo = (uint32_t)(uintptr_t)&r->trbs[0];
    r->trbs[XHCI_RING_TRBS - 1].param_hi = 0;
    r->trbs[XHCI_RING_TRBS - 1].status   = 0;
    r->trbs[XHCI_RING_TRBS - 1].control  = (TRB_TYPE_LINK << 10) | TRB_LINK_TOGGLE_CYCLE;
}

static void ring_enqueue(struct xhci_ring *r, uint32_t param_lo, uint32_t param_hi,
                         uint32_t status, uint32_t control)
{
    if (r->enq_idx == XHCI_RING_TRBS - 1U) {
        volatile struct xhci_trb *first = &r->trbs[0];
        volatile struct xhci_trb *link  = &r->trbs[XHCI_RING_TRBS - 1U];
        uint32_t next_ccs = (r->ccs ^ 1U) & TRB_CYCLE;

        /* 1. Write the new TRB at index 0 with the toggled cycle state first */
        first->param_lo = param_lo;
        first->param_hi = param_hi;
        first->status   = status;
        cpu_memory_barrier();
        first->control  = (control & ~TRB_CYCLE) | next_ccs;
        cpu_memory_barrier();

        /* 2. Activate the Link TRB at index (XHCI_RING_TRBS - 1) so hardware traverses to index 0 */
        link->param_lo = (uint32_t)(uintptr_t)&r->trbs[0];
        link->param_hi = 0;
        link->status   = 0;
        cpu_memory_barrier();
        link->control  = (TRB_TYPE_LINK << 10) | TRB_LINK_TOGGLE_CYCLE | (r->ccs & TRB_CYCLE);
        cpu_memory_barrier();

        r->ccs     = next_ccs;
        r->enq_idx = 1U;
        return;
    }

    {
        volatile struct xhci_trb *trb = &r->trbs[r->enq_idx];

        trb->param_lo = param_lo;
        trb->param_hi = param_hi;
        trb->status   = status;
        cpu_memory_barrier();
        trb->control  = (control & ~TRB_CYCLE) | (r->ccs & TRB_CYCLE);
        cpu_memory_barrier();

        r->enq_idx++;
    }
}

static void xhci_queue_hid_int_trb(struct xhci_usb_dev *dev)
{
    uint32_t len = (dev->dev_type == USB_DEV_KBD) ? 8U : 4U;
    if (len > dev->int_in_mps && dev->int_in_mps > 0) {
        len = dev->int_in_mps;
    }
    ring_enqueue(&dev->int_in_ring,
                 (uint32_t)(uintptr_t)dev->hid_buf,
                 0,
                 len,
                 (TRB_TYPE_NORMAL << 10) | TRB_IOC);
    db_base[dev->slot_id] = dev->int_in_dci;
}

static void xhci_handle_kbd_report(struct xhci_usb_dev *dev)
{
    const uint8_t *rep = dev->hid_buf;
    uint8_t mods = rep[0];
    bool_t shift = ((mods & 0x22U) != 0) ? TRUE : FALSE;
    bool_t ctrl  = ((mods & 0x11U) != 0) ? TRUE : FALSE;
    bool_t alt   = ((mods & 0x44U) != 0) ? TRUE : FALSE;
    int i, j;

    for (i = 2; i < 8; i++) {
        uint8_t usage = rep[i];
        bool_t was_pressed = FALSE;

        if (usage < 4U) {
            continue;
        }
        for (j = 2; j < 8; j++) {
            if (dev->prev_kbd_report[j] == usage) {
                was_pressed = TRUE;
                break;
            }
        }
        if (!was_pressed) {
            kbd_inject_usb_key(usage, 0, FALSE, shift, ctrl, alt);
        }
    }

    memcpy(dev->prev_kbd_report, rep, 8);
}

static void xhci_handle_mouse_report(struct xhci_usb_dev *dev)
{
    const uint8_t *rep = dev->hid_buf;
    uint8_t buttons = (uint8_t)(rep[0] & 0x07U);
    int32_t dx = (int32_t)(int8_t)rep[1];
    int32_t dy = (int32_t)(int8_t)rep[2];

    mouse_inject_usb_report(dx, dy, buttons);
}

static int xhci_pop_event(struct xhci_trb *out)
{
    volatile struct xhci_trb *ev = &evt_ring[evt_deq_idx];
    uint32_t ctrl = ev->control;

    if ((ctrl & TRB_CYCLE) != evt_ccs) {
        return 0;
    }

    out->param_lo = ev->param_lo;
    out->param_hi = ev->param_hi;
    out->status   = ev->status;
    out->control  = ctrl;

    evt_deq_idx++;
    if (evt_deq_idx >= XHCI_RING_TRBS) {
        evt_deq_idx = 0;
        evt_ccs ^= 1U;
    }

    mmio_write32(rt_base, 0x20 + 0x18,
                 ((uint32_t)(uintptr_t)&evt_ring[evt_deq_idx]) | (1U << 3));
    mmio_write32(rt_base, 0x20 + 0x1C, 0);
    return 1;
}

static void xhci_dispatch_async_event(const struct xhci_trb *ev)
{
    uint32_t trb_type = (ev->control >> 10) & 0x3FU;
    uint8_t  slot_id  = (uint8_t)((ev->control >> 24) & 0xFFU);

    if (trb_type == TRB_TYPE_TRANSFER_EVENT) {
        if (kbd_dev_idx >= 0 && usb_devs[kbd_dev_idx].slot_id == slot_id) {
            xhci_handle_kbd_report(&usb_devs[kbd_dev_idx]);
            xhci_queue_hid_int_trb(&usb_devs[kbd_dev_idx]);
        } else if (mouse_dev_idx >= 0 && usb_devs[mouse_dev_idx].slot_id == slot_id) {
            xhci_handle_mouse_report(&usb_devs[mouse_dev_idx]);
            xhci_queue_hid_int_trb(&usb_devs[mouse_dev_idx]);
        }
    } else if (trb_type == TRB_TYPE_PORT_STATUS_CHG) {
        hotplug_pending = TRUE;
    }
}

static int xhci_wait_event(uint32_t expected_type, uint8_t expected_slot, struct xhci_trb *out)
{
    int timeout = 25000;
    struct xhci_trb ev;

    while (timeout > 0) {
        if (xhci_pop_event(&ev)) {
            uint32_t trb_type = (ev.control >> 10) & 0x3FU;
            uint8_t  slot_id  = (uint8_t)((ev.control >> 24) & 0xFFU);

            if (trb_type == expected_type &&
                (expected_slot == 0 || slot_id == expected_slot)) {
                if (out != NULL) {
                    *out = ev;
                }
                return 0;
            }
            xhci_dispatch_async_event(&ev);
        } else {
            cpu_pause();
            if ((timeout & 0x0FU) == 0) {
                pit_udelay(20U);
            }
        }
        timeout--;
    }
    return -1;
}

void xhci_poll(void)
{
    struct xhci_trb ev;
    int max_events = 16;
    uint32_t now_ticks;

    if (!xhci_ready) {
        return;
    }
    while (max_events > 0 && xhci_pop_event(&ev)) {
        xhci_dispatch_async_event(&ev);
        max_events--;
    }

    now_ticks = pit_get_ticks();
    if (hotplug_pending || (now_ticks - last_hotplug_tick) >= 250U) {
        uint32_t port;
        int d;
        hotplug_pending   = FALSE;
        last_hotplug_tick = now_ticks;

        for (port = 1; port <= max_ports && port <= 32U && num_usb_devs < XHCI_MAX_SLOTS; port++) {
            uint32_t port_off = 0x400U + (port - 1U) * 0x10U;
            uint32_t portsc   = mmio_read32(op_base, port_off);
            uint32_t bit      = (1U << (port - 1U));

            if ((portsc & 1U) == 0) {
                root_port_conn_mask &= ~bit;
            } else if ((root_port_conn_mask & bit) == 0) {
                root_port_conn_mask |= bit;
                pit_mdelay(30U);
                xhci_enumerate_root_port(port);
                ctrl_info.num_devices = (uint32_t)num_usb_devs;
            }
        }

        /* Also scan downstream ports of any enumerated external USB hubs for newly ready devices */
        for (d = 0; d < num_usb_devs && num_usb_devs < XHCI_MAX_SLOTS; d++) {
            struct xhci_usb_dev *hdev = &usb_devs[d];
            uint8_t hp;

            if (hdev->dev_type != USB_DEV_HUB || hdev->hub_ports == 0) {
                continue;
            }
            for (hp = 1; hp <= hdev->hub_ports && num_usb_devs < XHCI_MAX_SLOTS; hp++) {
                uint8_t port_sts[4];
                uint16_t wPortStatus;
                uint32_t bit = (1U << (hp - 1U));

                memset(port_sts, 0, sizeof(port_sts));
                if (xhci_control_transfer(hdev, 0xA3U, 0x00U, 0, hp, 4U, port_sts) < 0) {
                    continue;
                }
                wPortStatus = (uint16_t)((uint16_t)port_sts[0] | ((uint16_t)port_sts[1] << 8));
                if ((wPortStatus & 0x0001U) == 0) {
                    hdev->hub_port_conn_mask &= ~bit;
                } else if ((hdev->hub_port_conn_mask & bit) == 0) {
                    hdev->hub_port_conn_mask |= bit;
                    if (xhci_enumerate_hub_port(hdev, hp) == 0) {
                        hub_info.downstream_devices++;
                        ctrl_info.num_devices = (uint32_t)num_usb_devs;
                    }
                }
            }
        }
    }
}

static int xhci_send_command(uint32_t param_lo, uint32_t param_hi, uint32_t control,
                             struct xhci_trb *out_ev)
{
    ring_enqueue(&cmd_ring, param_lo, param_hi, 0, control);
    db_base[0] = 0;
    if (xhci_wait_event(TRB_TYPE_CMD_COMPLETION, 0, out_ev) < 0) {
        return -1;
    }
    if (((out_ev->status >> 24) & 0xFFU) != 1U) {
        return -1;
    }
    return 0;
}

static void xhci_recover_ep0(struct xhci_usb_dev *dev)
{
    uint32_t deq_phys;
    struct xhci_trb ev;

    if (dev->ep0_ring.enq_idx == XHCI_RING_TRBS - 1U) {
        dev->ep0_ring.enq_idx = 0U;
    }
    deq_phys = ((uint32_t)(uintptr_t)&dev->ep0_ring.trbs[dev->ep0_ring.enq_idx]) |
               (dev->ep0_ring.ccs & 1U);

    (void)xhci_send_command(0, 0,
                            (TRB_TYPE_RESET_EP << 10) |
                            (1U << 16) |
                            ((uint32_t)dev->slot_id << 24),
                            &ev);
    (void)xhci_send_command(deq_phys, 0,
                            (TRB_TYPE_SET_TR_DEQ << 10) |
                            (1U << 16) |
                            ((uint32_t)dev->slot_id << 24),
                            &ev);
}

static int xhci_control_transfer(struct xhci_usb_dev *dev,
                                 uint8_t bmRequestType, uint8_t bRequest,
                                 uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                                 void *data)
{
    uint32_t setup_lo;
    uint32_t setup_hi;
    uint32_t trt = 0;
    uint32_t status_dir;
    uint32_t status_trb_phys;
    struct xhci_trb ev;
    uint8_t cc;

    setup_lo = (uint32_t)bmRequestType |
               ((uint32_t)bRequest << 8) |
               ((uint32_t)wValue << 16);
    setup_hi = (uint32_t)wIndex |
               ((uint32_t)wLength << 16);

    if (wLength > 0) {
        trt = ((bmRequestType & 0x80U) != 0) ? (3U << 16) : (2U << 16);
    }

    ring_enqueue(&dev->ep0_ring, setup_lo, setup_hi, 8U,
                 (TRB_TYPE_SETUP_STAGE << 10) | TRB_IDT | trt);

    if (wLength > 0) {
        uint32_t dir = ((bmRequestType & 0x80U) != 0) ? TRB_DIR_IN : 0U;
        if ((bmRequestType & 0x80U) == 0 && data != NULL) {
            memcpy(ctrl_dma_buf, data, wLength);
        } else {
            memset(ctrl_dma_buf, 0, wLength);
        }
        ring_enqueue(&dev->ep0_ring,
                     (uint32_t)(uintptr_t)ctrl_dma_buf, 0,
                     wLength,
                     (TRB_TYPE_DATA_STAGE << 10) | dir);
    }

    status_dir = (wLength == 0 || (bmRequestType & 0x80U) == 0) ? TRB_DIR_IN : 0U;
    {
        uint32_t st_idx = (dev->ep0_ring.enq_idx == XHCI_RING_TRBS - 1U) ? 0U : dev->ep0_ring.enq_idx;
        status_trb_phys = (uint32_t)(uintptr_t)&dev->ep0_ring.trbs[st_idx];
    }
    ring_enqueue(&dev->ep0_ring, 0, 0, 0,
                 (TRB_TYPE_STATUS_STAGE << 10) | TRB_IOC | status_dir);

    db_base[dev->slot_id] = 1U;

    if (xhci_wait_event(TRB_TYPE_TRANSFER_EVENT, dev->slot_id, &ev) < 0) {
        return -1;
    }

    cc = (uint8_t)((ev.status >> 24) & 0xFFU);

    /* If controller generated a Short Packet event on the Data Stage TRB, also consume Status Stage */
    if (cc == 13U && ev.param_lo != status_trb_phys) {
        if (xhci_wait_event(TRB_TYPE_TRANSFER_EVENT, dev->slot_id, &ev) < 0) {
            return -1;
        }
        cc = (uint8_t)((ev.status >> 24) & 0xFFU);
    }

    if (cc == 6U || cc == 4U || cc == 3U || cc == 2U) {
        xhci_recover_ep0(dev);
        return -1;
    }
    if (cc != 1U && cc != 13U) {
        return -1;
    }

    if (wLength > 0 && (bmRequestType & 0x80U) != 0 && data != NULL) {
        memcpy(data, ctrl_dma_buf, wLength);
    }
    return 0;
}

static void xhci_clear_bulk_stall(struct xhci_usb_dev *dev, bool_t is_in)
{
    struct xhci_ring *ring = is_in ? &dev->bulk_in_ring : &dev->bulk_out_ring;
    uint8_t dci = is_in ? dev->bulk_in_dci : dev->bulk_out_dci;
    uint8_t ep_num = (uint8_t)(dci / 2U);
    uint8_t ep_addr = (uint8_t)(ep_num | (is_in ? 0x80U : 0x00U));
    uint32_t deq_phys;
    struct xhci_trb ev;

    if (ring->enq_idx == XHCI_RING_TRBS - 1U) {
        ring->enq_idx = 0U;
    }
    deq_phys = ((uint32_t)(uintptr_t)&ring->trbs[ring->enq_idx]) | (ring->ccs & 1U);

    /* 1. Reset Endpoint in xHCI */
    (void)xhci_send_command(0, 0,
                            (TRB_TYPE_RESET_EP << 10) |
                            ((uint32_t)dci << 16) |
                            ((uint32_t)dev->slot_id << 24),
                            &ev);

    /* 2. Set TR Dequeue Pointer past stalled TRB */
    (void)xhci_send_command(deq_phys, 0,
                            (TRB_TYPE_SET_TR_DEQ << 10) |
                            ((uint32_t)dci << 16) |
                            ((uint32_t)dev->slot_id << 24),
                            &ev);

    /* 3. Clear ENDPOINT_HALT feature on USB device */
    (void)xhci_control_transfer(dev, 0x02U, 0x01U, 0, ep_addr, 0, NULL);
}

static int xhci_bulk_transfer(struct xhci_usb_dev *dev, bool_t is_in,
                              void *buf, uint32_t len)
{
    struct xhci_ring *ring = is_in ? &dev->bulk_in_ring : &dev->bulk_out_ring;
    uint8_t dci = is_in ? dev->bulk_in_dci : dev->bulk_out_dci;
    struct xhci_trb ev;
    uint8_t cc;

    ring_enqueue(ring,
                 (uint32_t)(uintptr_t)buf, 0,
                 len,
                 (TRB_TYPE_NORMAL << 10) | TRB_IOC);
    db_base[dev->slot_id] = dci;

    if (xhci_wait_event(TRB_TYPE_TRANSFER_EVENT, dev->slot_id, &ev) < 0) {
        return -1;
    }
    cc = (uint8_t)((ev.status >> 24) & 0xFFU);
    if (cc == 6U) {
        /* Endpoint stalled: recover endpoint state for subsequent CSW/transfers */
        xhci_clear_bulk_stall(dev, is_in);
        return -2;
    }
    if (cc != 1U && cc != 13U) {
        return -1;
    }
    return 0;
}

static void xhci_msc_bot_reset(struct xhci_usb_dev *dev)
{
    /* Bulk-Only Mass Storage Reset (class-specific request 0xFF) */
    (void)xhci_control_transfer(dev, 0x21U, 0xFFU, 0, dev->iface_num, 0, NULL);
    xhci_clear_bulk_stall(dev, TRUE);
    xhci_clear_bulk_stall(dev, FALSE);
    pit_mdelay(10U);
}

static int xhci_msc_bot_cmd(struct xhci_usb_dev *dev,
                            const uint8_t *cdb, uint8_t cdb_len,
                            bool_t dir_in, void *data_buf, uint32_t data_len)
{
    uint32_t tag = msc_cbw_tag++;
    int data_rc = 0;
    int csw_rc;

    memset(msc_cbw_buf, 0, 31);
    msc_cbw_buf[0] = 0x55;
    msc_cbw_buf[1] = 0x53;
    msc_cbw_buf[2] = 0x42;
    msc_cbw_buf[3] = 0x43;
    msc_cbw_buf[4] = (uint8_t)(tag & 0xFFU);
    msc_cbw_buf[5] = (uint8_t)((tag >> 8) & 0xFFU);
    msc_cbw_buf[6] = (uint8_t)((tag >> 16) & 0xFFU);
    msc_cbw_buf[7] = (uint8_t)((tag >> 24) & 0xFFU);
    msc_cbw_buf[8]  = (uint8_t)(data_len & 0xFFU);
    msc_cbw_buf[9]  = (uint8_t)((data_len >> 8) & 0xFFU);
    msc_cbw_buf[10] = (uint8_t)((data_len >> 16) & 0xFFU);
    msc_cbw_buf[11] = (uint8_t)((data_len >> 24) & 0xFFU);
    msc_cbw_buf[12] = dir_in ? 0x80U : 0x00U;
    msc_cbw_buf[13] = 0;
    msc_cbw_buf[14] = cdb_len;
    memcpy(&msc_cbw_buf[15], cdb, cdb_len);

    if (xhci_bulk_transfer(dev, FALSE, msc_cbw_buf, 31) < 0) {
        xhci_msc_bot_reset(dev);
        return -1;
    }

    if (data_len > 0 && data_buf != NULL) {
        if (!dir_in) {
            memcpy(msc_data_buf, data_buf, data_len);
        } else {
            memset(msc_data_buf, 0, data_len);
        }
        data_rc = xhci_bulk_transfer(dev, dir_in, msc_data_buf, data_len);
        if (data_rc == -1) {
            xhci_msc_bot_reset(dev);
            return -1;
        }
        if (data_rc == 0 && dir_in) {
            memcpy(data_buf, msc_data_buf, data_len);
        }
    }

    memset(msc_csw_buf, 0, 13);
    csw_rc = xhci_bulk_transfer(dev, TRUE, msc_csw_buf, 13);
    if (csw_rc == -2) {
        /* USB BOT 1.0 Section 5.3.4: on CSW stall, clear Bulk-IN halt and retry CSW once */
        csw_rc = xhci_bulk_transfer(dev, TRUE, msc_csw_buf, 13);
    }
    if (csw_rc < 0) {
        xhci_msc_bot_reset(dev);
        return -1;
    }

    if (msc_csw_buf[0] != 0x55 || msc_csw_buf[1] != 0x53 ||
        msc_csw_buf[2] != 0x42 || msc_csw_buf[3] != 0x53) {
        xhci_msc_bot_reset(dev);
        return -1;
    }
    if (msc_csw_buf[12] == 2U) {
        xhci_msc_bot_reset(dev);
        return -1;
    }
    if (data_rc < 0 || msc_csw_buf[12] != 0) {
        return -1;
    }
    return 0;
}

static void xhci_init_msc_device(struct xhci_usb_dev *dev, int dev_idx)
{
    uint8_t cdb[16];
    uint8_t inq[36];
    uint8_t sense[18];
    uint8_t cap[8];
    uint8_t max_lun = 0;
    struct xhci_msc_info info;
    int retry;

    if (num_msc_devs >= XHCI_MAX_MSC_DEVS) {
        return;
    }
    memset(&info, 0, sizeof(info));

    pit_mdelay(10U);
    /* Query GET_MAX_LUN (optional class request 0xFE; some drives expect it after SET_CONFIGURATION) */
    (void)xhci_control_transfer(dev, 0xA1U, 0xFEU, 0, dev->iface_num, 1U, &max_lun);

    for (retry = 0; retry < 3; retry++) {
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x12;
        cdb[4] = 36;
        if (xhci_msc_bot_cmd(dev, cdb, 6, TRUE, inq, 36) == 0) {
            memcpy(info.vendor, &inq[8], 8);
            info.vendor[8] = '\0';
            memcpy(info.product, &inq[16], 16);
            info.product[16] = '\0';
            break;
        }
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x03;
        cdb[4] = 18;
        (void)xhci_msc_bot_cmd(dev, cdb, 6, TRUE, sense, 18);
        pit_mdelay(10U);
    }

    for (retry = 0; retry < 30; retry++) {
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x00;
        if (xhci_msc_bot_cmd(dev, cdb, 6, FALSE, NULL, 0) == 0) {
            break;
        }
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x03;
        cdb[4] = 18;
        (void)xhci_msc_bot_cmd(dev, cdb, 6, TRUE, sense, 18);
        pit_mdelay(20U);
    }

    for (retry = 0; retry < 5; retry++) {
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x25;
        if (xhci_msc_bot_cmd(dev, cdb, 10, TRUE, cap, 8) == 0) {
            int disk_idx = num_msc_devs;
            uint32_t last_lba = ((uint32_t)cap[0] << 24) |
                                ((uint32_t)cap[1] << 16) |
                                ((uint32_t)cap[2] << 8)  |
                                (uint32_t)cap[3];
            uint32_t blk_sz   = ((uint32_t)cap[4] << 24) |
                                ((uint32_t)cap[5] << 16) |
                                ((uint32_t)cap[6] << 8)  |
                                (uint32_t)cap[7];
            info.present     = TRUE;
            info.slot_id     = dev->slot_id;
            info.block_count = last_lba + 1U;
            info.block_size  = (blk_sz > 0) ? blk_sz : 512U;

            msc_dev_indices[disk_idx] = dev_idx;
            msc_infos[disk_idx]       = info;
            num_msc_devs++;

            kprintf("[XHCI-MSC] USB Mass Storage slot=%u (/dev/sd%c) vendor=\"%s\" product=\"%s\" blocks=%u (%u B/blk)\n",
                    (uint32_t)dev->slot_id, (char)('a' + disk_idx),
                    info.vendor, info.product,
                    info.block_count, info.block_size);
            break;
        }
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x03;
        cdb[4] = 18;
        (void)xhci_msc_bot_cmd(dev, cdb, 6, TRUE, sense, 18);
        pit_mdelay(20U);
    }
}

int32_t xhci_msc_read_sector_idx(int disk_idx, uint32_t lba, uint8_t *buf512)
{
    uint8_t cdb[16];
    uint8_t sense[18];
    int dev_idx;
    int retry;

    if (disk_idx < 0 || disk_idx >= num_msc_devs || !msc_infos[disk_idx].present || buf512 == NULL) {
        return -1;
    }
    if (lba >= msc_infos[disk_idx].block_count) {
        return 0;
    }
    dev_idx = msc_dev_indices[disk_idx];
    if (dev_idx < 0) {
        return -1;
    }

    for (retry = 0; retry < 3; retry++) {
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x28;
        cdb[2] = (uint8_t)((lba >> 24) & 0xFFU);
        cdb[3] = (uint8_t)((lba >> 16) & 0xFFU);
        cdb[4] = (uint8_t)((lba >> 8) & 0xFFU);
        cdb[5] = (uint8_t)(lba & 0xFFU);
        cdb[7] = 0x00;
        cdb[8] = 0x01;

        if (xhci_msc_bot_cmd(&usb_devs[dev_idx], cdb, 10, TRUE, buf512, 512) == 0) {
            return 512;
        }
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x03;
        cdb[4] = 18;
        (void)xhci_msc_bot_cmd(&usb_devs[dev_idx], cdb, 6, TRUE, sense, 18);
        pit_mdelay(5U);
    }
    return -1;
}

int32_t xhci_msc_write_sector_idx(int disk_idx, uint32_t lba, const uint8_t *buf512)
{
    uint8_t cdb[16];
    uint8_t sense[18];
    uint8_t tmp[512];
    int dev_idx;
    int retry;

    if (disk_idx < 0 || disk_idx >= num_msc_devs || !msc_infos[disk_idx].present || buf512 == NULL) {
        return -1;
    }
    if (lba >= msc_infos[disk_idx].block_count) {
        return 0;
    }
    dev_idx = msc_dev_indices[disk_idx];
    if (dev_idx < 0) {
        return -1;
    }
    memcpy(tmp, buf512, 512);

    for (retry = 0; retry < 3; retry++) {
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x2A;
        cdb[2] = (uint8_t)((lba >> 24) & 0xFFU);
        cdb[3] = (uint8_t)((lba >> 16) & 0xFFU);
        cdb[4] = (uint8_t)((lba >> 8) & 0xFFU);
        cdb[5] = (uint8_t)(lba & 0xFFU);
        cdb[7] = 0x00;
        cdb[8] = 0x01;

        if (xhci_msc_bot_cmd(&usb_devs[dev_idx], cdb, 10, FALSE, tmp, 512) == 0) {
            return 512;
        }
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x03;
        cdb[4] = 18;
        (void)xhci_msc_bot_cmd(&usb_devs[dev_idx], cdb, 6, TRUE, sense, 18);
        pit_mdelay(5U);
    }
    return -1;
}

int32_t xhci_msc_read_bytes_idx(int disk_idx, uint32_t offset, uint32_t size, uint8_t *buf)
{
    uint8_t sector_buf[512];
    uint32_t total_disk_bytes;
    uint32_t bytes_read = 0;

    if (disk_idx < 0 || disk_idx >= num_msc_devs || !msc_infos[disk_idx].present) {
        return -1;
    }
    total_disk_bytes = msc_infos[disk_idx].block_count * 512U;
    if (offset >= total_disk_bytes) {
        return 0;
    }
    if (offset + size > total_disk_bytes) {
        size = total_disk_bytes - offset;
    }

    while (bytes_read < size) {
        uint32_t cur_off  = offset + bytes_read;
        uint32_t lba      = cur_off / 512U;
        uint32_t sec_off  = cur_off % 512U;
        uint32_t chunk    = 512U - sec_off;

        if (chunk > size - bytes_read) {
            chunk = size - bytes_read;
        }
        if (xhci_msc_read_sector_idx(disk_idx, lba, sector_buf) != 512) {
            break;
        }
        memcpy(&buf[bytes_read], &sector_buf[sec_off], chunk);
        bytes_read += chunk;
    }
    return (int32_t)bytes_read;
}

int32_t xhci_msc_write_bytes_idx(int disk_idx, uint32_t offset, uint32_t size, const uint8_t *buf)
{
    uint8_t sector_buf[512];
    uint32_t total_disk_bytes;
    uint32_t bytes_written = 0;

    if (disk_idx < 0 || disk_idx >= num_msc_devs || !msc_infos[disk_idx].present) {
        return -1;
    }
    total_disk_bytes = msc_infos[disk_idx].block_count * 512U;
    if (offset >= total_disk_bytes) {
        return 0;
    }
    if (offset + size > total_disk_bytes) {
        size = total_disk_bytes - offset;
    }

    while (bytes_written < size) {
        uint32_t cur_off  = offset + bytes_written;
        uint32_t lba      = cur_off / 512U;
        uint32_t sec_off  = cur_off % 512U;
        uint32_t chunk    = 512U - sec_off;

        if (chunk > size - bytes_written) {
            chunk = size - bytes_written;
        }
        if (sec_off != 0 || chunk < 512U) {
            if (xhci_msc_read_sector_idx(disk_idx, lba, sector_buf) != 512) {
                break;
            }
        }
        memcpy(&sector_buf[sec_off], &buf[bytes_written], chunk);
        if (xhci_msc_write_sector_idx(disk_idx, lba, sector_buf) != 512) {
            break;
        }
        bytes_written += chunk;
    }
    return (int32_t)bytes_written;
}

int32_t xhci_msc_read_sector(uint32_t lba, uint8_t *buf512)
{
    return xhci_msc_read_sector_idx(0, lba, buf512);
}

int32_t xhci_msc_write_sector(uint32_t lba, const uint8_t *buf512)
{
    return xhci_msc_write_sector_idx(0, lba, buf512);
}

int32_t xhci_msc_read_bytes(uint32_t offset, uint32_t size, uint8_t *buf)
{
    return xhci_msc_read_bytes_idx(0, offset, size, buf);
}

int32_t xhci_msc_write_bytes(uint32_t offset, uint32_t size, const uint8_t *buf)
{
    return xhci_msc_write_bytes_idx(0, offset, size, buf);
}

static void xhci_setup_ep_ctx(uint8_t *ep_ctx, uint32_t ep_type, uint16_t mps,
                              uint8_t max_burst, uint8_t interval, uint32_t avg_trb_len,
                              const struct xhci_ring *ring)
{
    uint32_t *dw = (uint32_t *)(void *)ep_ctx;
    uint32_t ring_phys = (uint32_t)(uintptr_t)&ring->trbs[0];
    uint32_t esit = (ep_type == 7U) ? ((uint32_t)mps * (uint32_t)(max_burst + 1U)) : 0U;

    dw[0] = ((uint32_t)interval << 16);
    dw[1] = (3U << 1) | (ep_type << 3) | ((uint32_t)max_burst << 8) | ((uint32_t)mps << 16);
    dw[2] = ring_phys | 1U;
    dw[3] = 0;
    dw[4] = avg_trb_len | (esit << 16);
}

static int xhci_enumerate_hub_port(struct xhci_usb_dev *hub_dev, uint8_t hp)
{
    uint8_t port_sts[4];
    uint16_t wPortStatus = 0;
    uint16_t wPortChange = 0;
    uint8_t child_speed;
    uint32_t child_route;
    uint8_t route_nibble;
    int rst_polls;

    /* Clear C_PORT_CONNECTION */
    (void)xhci_control_transfer(hub_dev, 0x23U, 0x01U, HUB_FEAT_C_PORT_CONNECTION, hp, 0, NULL);
    if (hub_dev->speed >= 4U) {
        (void)xhci_control_transfer(hub_dev, 0x23U, 0x01U, HUB_FEAT_C_BH_PORT_RESET, hp, 0, NULL);
        (void)xhci_control_transfer(hub_dev, 0x23U, 0x01U, HUB_FEAT_C_PORT_LINK_STATE, hp, 0, NULL);
    }

    /* Reset downstream hub port (SET_PORT_FEATURE PORT_RESET = 4) */
    (void)xhci_control_transfer(hub_dev, 0x23U, 0x03U, HUB_FEAT_PORT_RESET, hp, 0, NULL);

    /* Poll port status until reset completes (C_PORT_RESET / C_BH_PORT_RESET or PORT_ENABLE active) */
    for (rst_polls = 0; rst_polls < 80; rst_polls++) {
        pit_mdelay(3U);
        memset(port_sts, 0, sizeof(port_sts));
        if (xhci_control_transfer(hub_dev, 0xA3U, 0x00U, 0, hp, 4U, port_sts) < 0) {
            break;
        }
        wPortStatus = (uint16_t)((uint16_t)port_sts[0] | ((uint16_t)port_sts[1] << 8));
        wPortChange = (uint16_t)((uint16_t)port_sts[2] | ((uint16_t)port_sts[3] << 8));
        if ((wPortChange & 0x0030U) != 0 ||
            ((wPortStatus & 0x0002U) != 0 && (wPortStatus & 0x0010U) == 0)) {
            break;
        }
    }

    /* On SuperSpeed hub, if standard reset did not enable the port, try Warm Port Reset (BH_PORT_RESET = 28) */
    if (hub_dev->speed >= 4U && (wPortStatus & 0x0002U) == 0 && (wPortStatus & 0x0001U) != 0) {
        (void)xhci_control_transfer(hub_dev, 0x23U, 0x03U, HUB_FEAT_BH_PORT_RESET, hp, 0, NULL);
        for (rst_polls = 0; rst_polls < 60; rst_polls++) {
            pit_mdelay(3U);
            memset(port_sts, 0, sizeof(port_sts));
            if (xhci_control_transfer(hub_dev, 0xA3U, 0x00U, 0, hp, 4U, port_sts) < 0) {
                break;
            }
            wPortStatus = (uint16_t)((uint16_t)port_sts[0] | ((uint16_t)port_sts[1] << 8));
            wPortChange = (uint16_t)((uint16_t)port_sts[2] | ((uint16_t)port_sts[3] << 8));
            if ((wPortChange & 0x0030U) != 0 ||
                ((wPortStatus & 0x0002U) != 0 && (wPortStatus & 0x0010U) == 0)) {
                break;
            }
        }
    }

    (void)xhci_control_transfer(hub_dev, 0x23U, 0x01U, HUB_FEAT_C_PORT_RESET, hp, 0, NULL);
    if (hub_dev->speed >= 4U) {
        (void)xhci_control_transfer(hub_dev, 0x23U, 0x01U, HUB_FEAT_C_BH_PORT_RESET, hp, 0, NULL);
        (void)xhci_control_transfer(hub_dev, 0x23U, 0x01U, HUB_FEAT_C_PORT_LINK_STATE, hp, 0, NULL);
    }
    pit_mdelay(20U); /* USB T_RSTRCY reset recovery */

    memset(port_sts, 0, sizeof(port_sts));
    if (xhci_control_transfer(hub_dev, 0xA3U, 0x00U, 0, hp, 4U, port_sts) < 0) {
        return -1;
    }
    wPortStatus = (uint16_t)((uint16_t)port_sts[0] | ((uint16_t)port_sts[1] << 8));
    if ((wPortStatus & 0x0002U) == 0) {
        return -1; /* Port not enabled */
    }

    if (hub_dev->speed >= 4U) {
        /*
         * USB 3.0 Enhanced SuperSpeed Hub wPortStatus (USB 3.0 Spec Table 10-10):
         * Bit 9 (0x0200) is PORT_POWER (NOT Low-Speed!).
         * All devices attached to a SuperSpeed hub operate at SuperSpeed (speed = 4).
         */
        child_speed = hub_dev->speed;
    } else if ((wPortStatus & 0x0200U) != 0) {
        child_speed = 2U; /* Low Speed (1.5 Mb/s) */
    } else if ((wPortStatus & 0x0400U) != 0) {
        child_speed = 3U; /* High Speed (480 Mb/s) */
    } else {
        child_speed = 1U; /* Full Speed (12 Mb/s) */
    }

    route_nibble = (hp > 15U) ? 15U : hp;
    child_route  = hub_dev->route_string | ((uint32_t)route_nibble << (hub_dev->hub_tier * 4U));

    kprintf("[XHCI-HUB] Hub slot %u port %u connected (speed=%u, route=0x%05x)\n",
            (uint32_t)hub_dev->slot_id, (uint32_t)hp,
            (uint32_t)child_speed, child_route);

    return xhci_enumerate_device(hub_dev->root_port, child_route,
                                 (uint32_t)hub_dev->hub_tier + 1U,
                                 child_speed, hub_dev->slot_id, hp);
}

/* Enumerate downstream ports of an xHCI USB Hub */
static void xhci_init_hub_device(struct xhci_usb_dev *hub_dev, uint32_t tier, uint32_t pwr_delay_ms)
{
    uint8_t num_ports = hub_dev->hub_ports;
    uint8_t hp;
    uint8_t downstream_cnt = 0;
    int wait_step;
    bool_t any_conn = FALSE;

    if (tier >= 5U || num_ports == 0) {
        return;
    }

    kprintf("[XHCI-HUB] USB Hub initialized on root port %u (slot %u, %u downstream ports, %s)\n",
            (uint32_t)hub_dev->root_port, (uint32_t)hub_dev->slot_id, (uint32_t)num_ports,
            (hub_dev->speed >= 4U) ? "USB 3.0 SuperSpeed" : "USB 2.0");

    /* Power on all downstream hub ports (SET_PORT_FEATURE PORT_POWER = 8) */
    for (hp = 1; hp <= num_ports; hp++) {
        (void)xhci_control_transfer(hub_dev, 0x23U, 0x03U, HUB_FEAT_PORT_POWER, hp, 0, NULL);
    }

    if (pwr_delay_ms < 50U) {
        pwr_delay_ms = 50U;
    }
    pit_mdelay(pwr_delay_ms);

    /* Wait up to 300 ms for downstream USB 2.0 / 3.0 devices to power up and assert PORT_CONNECTION */
    for (wait_step = 0; wait_step < 20; wait_step++) {
        for (hp = 1; hp <= num_ports; hp++) {
            uint8_t port_sts[4];
            uint16_t wPortStatus;
            memset(port_sts, 0, sizeof(port_sts));
            if (xhci_control_transfer(hub_dev, 0xA3U, 0x00U, 0, hp, 4U, port_sts) == 0) {
                wPortStatus = (uint16_t)((uint16_t)port_sts[0] | ((uint16_t)port_sts[1] << 8));
                if ((wPortStatus & 0x0001U) != 0) {
                    any_conn = TRUE;
                    break;
                }
            }
        }
        if (any_conn) {
            break;
        }
        pit_mdelay(15U);
    }
    if (any_conn) {
        pit_mdelay(40U);
    }

    /* Scan each downstream port on the hub */
    for (hp = 1; hp <= num_ports; hp++) {
        uint8_t port_sts[4];
        uint16_t wPortStatus = 0;

        memset(port_sts, 0, sizeof(port_sts));
        if (xhci_control_transfer(hub_dev, 0xA3U, 0x00U, 0, hp, 4U, port_sts) < 0) {
            continue;
        }
        wPortStatus = (uint16_t)((uint16_t)port_sts[0] | ((uint16_t)port_sts[1] << 8));
        if ((wPortStatus & 0x0001U) == 0) {
            continue; /* No device connected on this hub port */
        }

        hub_dev->hub_port_conn_mask |= (1U << (hp - 1U));
        if (xhci_enumerate_hub_port(hub_dev, hp) == 0) {
            downstream_cnt++;
        }
    }

    if (!hub_info.present) {
        hub_info.present            = TRUE;
        hub_info.slot_id            = hub_dev->slot_id;
        hub_info.root_port          = hub_dev->root_port;
        hub_info.num_ports          = num_ports;
        hub_info.downstream_devices = downstream_cnt;
    } else {
        hub_info.downstream_devices = (uint8_t)(hub_info.downstream_devices + downstream_cnt);
        if (downstream_cnt > 0) {
            hub_info.slot_id   = hub_dev->slot_id;
            hub_info.root_port = hub_dev->root_port;
            hub_info.num_ports = num_ports;
        }
    }
}

static int xhci_enumerate_device(uint8_t root_port, uint32_t route_string,
                                 uint32_t tier, uint8_t speed,
                                 uint8_t parent_hub_slot, uint8_t parent_port)
{
    struct xhci_trb ev;
    uint8_t slot_id;
    int dev_idx;
    struct xhci_usb_dev *dev;
    uint32_t *in_ctrl_dw;
    uint32_t *slot_dw;
    uint8_t dev_desc[18];
    uint8_t cfg_desc[256];
    uint16_t total_len;
    uint32_t pos;
    uint8_t max_dci = 1;
    uint32_t add_flags = (1U << 0);

    if (num_usb_devs >= XHCI_MAX_SLOTS) {
        return -1;
    }

    /* 1. Enable Slot Command */
    if (xhci_send_command(0, 0, (TRB_TYPE_ENABLE_SLOT << 10), &ev) < 0) {
        kprintf("[XHCI] Enable Slot failed (root_port=%u route=0x%x)\n",
                (uint32_t)root_port, route_string);
        return -1;
    }
    slot_id = (uint8_t)((ev.control >> 24) & 0xFFU);

    dev_idx = num_usb_devs++;
    dev = &usb_devs[dev_idx];
    memset(dev, 0, sizeof(struct xhci_usb_dev));

    dev->slot_id         = slot_id;
    dev->root_port       = root_port;
    dev->route_string    = route_string;
    dev->speed           = speed;
    dev->parent_hub_slot = parent_hub_slot;
    dev->parent_port     = parent_port;

    if (speed == 1U || speed == 2U) {
        /* Full-Speed (1) starts at 8B until bMaxPacketSize0 is read; Low-Speed (2) is 8B */
        dev->ep0_mps = 8;
    } else if (speed >= 4U) {
        dev->ep0_mps = 512;
    } else {
        dev->ep0_mps = 64;
    }

    dev->out_ctx = (uint8_t *)dma_alloc(32U * ctx_size, 64);
    dev->in_ctx  = (uint8_t *)dma_alloc(33U * ctx_size, 64);
    ring_init(&dev->ep0_ring);

    dcbaa[slot_id * 2U]      = (uint32_t)(uintptr_t)dev->out_ctx;
    dcbaa[slot_id * 2U + 1U] = 0;

    in_ctrl_dw = (uint32_t *)(void *)dev->in_ctx;
    in_ctrl_dw[0] = 0;
    in_ctrl_dw[1] = (1U << 0) | (1U << 1);

    slot_dw = (uint32_t *)(void *)(dev->in_ctx + ctx_size);
    slot_dw[0] = (route_string & 0xFFFFFUL) | ((uint32_t)speed << 20) | (1U << 27);
    slot_dw[1] = ((uint32_t)root_port << 16);
    if (parent_hub_slot != 0 && (speed == 1U || speed == 2U)) {
        slot_dw[2] = (uint32_t)parent_hub_slot | ((uint32_t)parent_port << 8);
    }

    xhci_setup_ep_ctx(dev->in_ctx + (2U * ctx_size), 4U, dev->ep0_mps, 0U, 0U, 8U, &dev->ep0_ring);

    /* 2. Address Device Command (with BSR=1 fallback for strict Full/Low-Speed devices) */
    if (xhci_send_command((uint32_t)(uintptr_t)dev->in_ctx, 0,
                          (TRB_TYPE_ADDRESS_DEVICE << 10) | ((uint32_t)slot_id << 24),
                          &ev) < 0) {
        /* Try BSR=1 (Block Set Address Request, bit 9) first, read 8-byte Device Descriptor, then BSR=0 */
        if (xhci_send_command((uint32_t)(uintptr_t)dev->in_ctx, 0,
                              (TRB_TYPE_ADDRESS_DEVICE << 10) | (1U << 9) | ((uint32_t)slot_id << 24),
                              &ev) < 0) {
            kprintf("[XHCI] Address Device failed on slot %u\n", (uint32_t)slot_id);
            (void)xhci_send_command(0, 0,
                                    (TRB_TYPE_DISABLE_SLOT << 10) | ((uint32_t)slot_id << 24),
                                    &ev);
            dcbaa[slot_id * 2U] = 0;
            num_usb_devs--;
            return -1;
        }
        memset(dev_desc, 0, sizeof(dev_desc));
        if (xhci_control_transfer(dev, 0x80U, 0x06U, 0x0100U, 0, 8U, dev_desc) == 0 &&
            (dev_desc[7] == 8U || dev_desc[7] == 16U || dev_desc[7] == 32U || dev_desc[7] == 64U)) {
            dev->ep0_mps = dev_desc[7];
            xhci_setup_ep_ctx(dev->in_ctx + (2U * ctx_size), 4U, dev->ep0_mps, 0U, 0U, 8U, &dev->ep0_ring);
        }
        if (xhci_send_command((uint32_t)(uintptr_t)dev->in_ctx, 0,
                              (TRB_TYPE_ADDRESS_DEVICE << 10) | ((uint32_t)slot_id << 24),
                              &ev) < 0) {
            kprintf("[XHCI] Address Device (post-BSR) failed on slot %u\n", (uint32_t)slot_id);
            (void)xhci_send_command(0, 0,
                                    (TRB_TYPE_DISABLE_SLOT << 10) | ((uint32_t)slot_id << 24),
                                    &ev);
            dcbaa[slot_id * 2U] = 0;
            num_usb_devs--;
            return -1;
        }
    }

    /* USB 2.0 Spec 9.2.6.3: Wait T_SETADDR (minimum 2 ms) after SET_ADDRESS */
    pit_mdelay(5U);

    /* For Full-Speed devices, probe first 8 bytes of Device Descriptor to learn bMaxPacketSize0 */
    if (speed == 1U || speed == 2U) {
        memset(dev_desc, 0, sizeof(dev_desc));
        if (xhci_control_transfer(dev, 0x80U, 0x06U, 0x0100U, 0, 8U, dev_desc) == 0) {
            uint16_t actual_mps = dev_desc[7];
            if ((actual_mps == 8U || actual_mps == 16U || actual_mps == 32U || actual_mps == 64U) &&
                actual_mps != dev->ep0_mps) {
                uint32_t *ep0_dw;
                dev->ep0_mps = actual_mps;
                memset(dev->in_ctx, 0, 33U * ctx_size);
                in_ctrl_dw = (uint32_t *)(void *)dev->in_ctx;
                in_ctrl_dw[1] = (1U << 1); /* Evaluate EP0 Context */
                ep0_dw = (uint32_t *)(void *)(dev->in_ctx + (2U * ctx_size));
                ep0_dw[1] = (3U << 1) | (4U << 3) | ((uint32_t)dev->ep0_mps << 16);
                (void)xhci_send_command((uint32_t)(uintptr_t)dev->in_ctx, 0,
                                        (TRB_TYPE_EVALUATE_CTX << 10) | ((uint32_t)slot_id << 24),
                                        &ev);
            }
        }
    }

    /* 3. Read 18-byte USB Device Descriptor */
    memset(dev_desc, 0, sizeof(dev_desc));
    if (xhci_control_transfer(dev, 0x80U, 0x06U, 0x0100U, 0, 18U, dev_desc) < 0) {
        kprintf("[XHCI] Failed to read Device Descriptor on slot %u\n", (uint32_t)slot_id);
        (void)xhci_send_command(0, 0,
                                (TRB_TYPE_DISABLE_SLOT << 10) | ((uint32_t)slot_id << 24),
                                &ev);
        dcbaa[slot_id * 2U] = 0;
        num_usb_devs--;
        return -1;
    }

    if (dev_desc[4] == 0x09U) {
        dev->dev_type = USB_DEV_HUB;
    }

    /* 4. Read Configuration Descriptor */
    memset(cfg_desc, 0, sizeof(cfg_desc));
    if (xhci_control_transfer(dev, 0x80U, 0x06U, 0x0200U, 0, 9U, cfg_desc) < 0) {
        (void)xhci_send_command(0, 0,
                                (TRB_TYPE_DISABLE_SLOT << 10) | ((uint32_t)slot_id << 24),
                                &ev);
        dcbaa[slot_id * 2U] = 0;
        num_usb_devs--;
        return -1;
    }
    total_len = (uint16_t)((uint16_t)cfg_desc[2] | ((uint16_t)cfg_desc[3] << 8));
    if (total_len > sizeof(cfg_desc)) {
        total_len = sizeof(cfg_desc);
    }
    if (xhci_control_transfer(dev, 0x80U, 0x06U, 0x0200U, 0, total_len, cfg_desc) < 0) {
        (void)xhci_send_command(0, 0,
                                (TRB_TYPE_DISABLE_SLOT << 10) | ((uint32_t)slot_id << 24),
                                &ev);
        dcbaa[slot_id * 2U] = 0;
        num_usb_devs--;
        return -1;
    }

    dev->config_val = cfg_desc[5];

    {
        bool_t  capture_eps  = (dev->dev_type == USB_DEV_HUB) ? TRUE : FALSE;
        bool_t  iface_locked = FALSE;
        uint8_t last_ep_dci  = 0;

        pos = 0;
        while (pos + 2U <= total_len) {
            uint8_t bLength = cfg_desc[pos];
            uint8_t bDescriptorType = cfg_desc[pos + 1U];

            if (bLength == 0) {
                break;
            }
            if (bDescriptorType == 4U && pos + 9U <= total_len) {
                uint8_t iface_num   = cfg_desc[pos + 2U];
                uint8_t alt_setting = cfg_desc[pos + 3U];
                uint8_t iface_class = cfg_desc[pos + 5U];
                uint8_t iface_sub   = cfg_desc[pos + 6U];
                uint8_t iface_proto = cfg_desc[pos + 7U];
                uint32_t cand_type  = USB_DEV_NONE;

                last_ep_dci = 0;

                /* Check if previous matched interface already captured all required endpoints */
                if ((dev->dev_type == USB_DEV_KBD && dev->int_in_dci != 0) ||
                    (dev->dev_type == USB_DEV_MOUSE && dev->int_in_dci != 0) ||
                    (dev->dev_type == USB_DEV_STORAGE && dev->bulk_in_dci != 0 && dev->bulk_out_dci != 0) ||
                    (dev->dev_type == USB_DEV_HUB && dev->int_in_dci != 0)) {
                    iface_locked = TRUE;
                }

                /* If a keyboard is already registered and a second composite device has a mouse interface, allow it */
                if (iface_locked && dev->dev_type == USB_DEV_KBD &&
                    kbd_dev_idx >= 0 && mouse_dev_idx < 0 &&
                    alt_setting == 0U && iface_class == 0x03U && iface_proto == 0x02U) {
                    iface_locked = FALSE;
                }

                if (iface_locked || alt_setting != 0U) {
                    capture_eps = FALSE;
                } else {
                    if (iface_class == 0x09U) {
                        cand_type = USB_DEV_HUB;
                    } else if (iface_class == 0x03U && iface_proto == 0x01U) {
                        cand_type = USB_DEV_KBD;
                    } else if (iface_class == 0x03U && iface_proto == 0x02U) {
                        cand_type = USB_DEV_MOUSE;
                    } else if (iface_class == 0x08U && iface_proto == 0x50U &&
                               (iface_sub == 0x06U || iface_sub == 0x02U ||
                                iface_sub == 0x05U || iface_sub == 0x01U ||
                                iface_sub == 0x04U || iface_sub == 0x00U)) {
                        cand_type = USB_DEV_STORAGE;
                    } else if (iface_class == 0x03U && iface_proto == 0x00U &&
                               dev->dev_type == USB_DEV_NONE) {
                        cand_type = (kbd_dev_idx < 0) ? USB_DEV_KBD : USB_DEV_MOUSE;
                    }

                    if (cand_type != USB_DEV_NONE) {
                        dev->dev_type       = cand_type;
                        dev->iface_num      = iface_num;
                        dev->int_in_dci     = 0;
                        dev->int_in_mps     = 0;
                        dev->bulk_in_dci    = 0;
                        dev->bulk_in_mps    = 0;
                        dev->bulk_in_burst  = 0;
                        dev->bulk_out_dci   = 0;
                        dev->bulk_out_mps   = 0;
                        dev->bulk_out_burst = 0;
                        capture_eps         = TRUE;
                    } else {
                        capture_eps = FALSE;
                    }
                }
            } else if (bDescriptorType == 5U && pos + 7U <= total_len && capture_eps) {
                uint8_t  ep_addr = cfg_desc[pos + 2U];
                uint8_t  ep_attr = (uint8_t)(cfg_desc[pos + 3U] & 0x03U);
                uint16_t ep_mps  = (uint16_t)(((uint16_t)cfg_desc[pos + 4U] |
                                              ((uint16_t)cfg_desc[pos + 5U] << 8)) & 0x07FFU);
                uint8_t  ep_num  = (uint8_t)(ep_addr & 0x0FU);
                uint8_t  is_in   = ((ep_addr & 0x80U) != 0) ? 1U : 0U;
                uint8_t  dci     = (uint8_t)(ep_num * 2U + is_in);

                last_ep_dci = 0;
                if (ep_attr == 3U && is_in && dev->int_in_dci == 0) {
                    dev->int_in_dci = dci;
                    dev->int_in_mps = (ep_mps > 0) ? ep_mps : 8U;
                    last_ep_dci     = dci;
                    if (dci > max_dci) {
                        max_dci = dci;
                    }
                } else if (ep_attr == 2U && dev->dev_type == USB_DEV_STORAGE) {
                    uint16_t def_mps = (speed >= 4U) ? 1024U : ((speed == 3U) ? 512U : 64U);
                    if (is_in && dev->bulk_in_dci == 0) {
                        dev->bulk_in_dci = dci;
                        dev->bulk_in_mps = (ep_mps > 0) ? ep_mps : def_mps;
                        last_ep_dci      = dci;
                    } else if (!is_in && dev->bulk_out_dci == 0) {
                        dev->bulk_out_dci = dci;
                        dev->bulk_out_mps = (ep_mps > 0) ? ep_mps : def_mps;
                        last_ep_dci       = dci;
                    }
                    if (dci > max_dci) {
                        max_dci = dci;
                    }
                }
            } else if (bDescriptorType == 0x30U && pos + 3U <= total_len && capture_eps) {
                /* SuperSpeed Endpoint Companion Descriptor (USB 3.0 Spec Section 9.6.7) */
                uint8_t max_burst = (uint8_t)(cfg_desc[pos + 2U] & 0x0FU);
                if (last_ep_dci != 0) {
                    if (last_ep_dci == dev->bulk_in_dci) {
                        dev->bulk_in_burst = max_burst;
                    } else if (last_ep_dci == dev->bulk_out_dci) {
                        dev->bulk_out_burst = max_burst;
                    }
                }
            }
            pos += bLength;
        }
    }

    if (dev->dev_type == USB_DEV_NONE) {
        (void)xhci_send_command(0, 0,
                                (TRB_TYPE_DISABLE_SLOT << 10) | ((uint32_t)slot_id << 24),
                                &ev);
        dcbaa[slot_id * 2U] = 0;
        num_usb_devs--;
        return -1;
    }

    /* 5. Send USB SET_CONFIGURATION on EP0 before configuring xHCI endpoints/hub context */
    (void)xhci_control_transfer(dev, 0x00U, 0x09U, dev->config_val, 0, 0, NULL);

    /* For USB Hubs, read the Hub Descriptor and set Hub Depth now so CONFIGURE_EP gets Number of Ports > 0 */
    {
        uint32_t hub_pwr_delay_ms = 50U;
        if (dev->dev_type == USB_DEV_HUB) {
            uint8_t hub_desc[16];
            uint8_t num_ports = 0;

            memset(hub_desc, 0, sizeof(hub_desc));
            if (speed >= 4U) {
                /* SuperSpeed Hub Descriptor (type 0x2A, 12 bytes) */
                if (xhci_control_transfer(dev, 0xA0U, 0x06U, 0x2A00U, 0, 12U, hub_desc) < 0 ||
                    hub_desc[2] == 0) {
                    (void)xhci_control_transfer(dev, 0xA0U, 0x06U, 0x2900U, 0, 9U, hub_desc);
                }
                /* USB 3.0 Spec 10.14.2.10: Set Hub Depth (bRequest = 0x0C) so SuperSpeed hub routes packets */
                (void)xhci_control_transfer(dev, 0x20U, 0x0CU, (uint16_t)tier, 0, 0, NULL);
            } else {
                /* USB 2.0 Hub Descriptor (type 0x29, 9 bytes) */
                if (xhci_control_transfer(dev, 0xA0U, 0x06U, 0x2900U, 0, 9U, hub_desc) < 0 ||
                    hub_desc[2] == 0) {
                    (void)xhci_control_transfer(dev, 0xA0U, 0x06U, 0x2A00U, 0, 12U, hub_desc);
                }
            }

            num_ports = hub_desc[2];
            if (num_ports == 0 || num_ports > 15U) {
                num_ports = 4U;
            }
            dev->hub_ports = num_ports;
            dev->hub_tier  = (uint8_t)tier;
            dev->hub_ttt   = (uint8_t)((hub_desc[3] >> 5) & 0x03U);
            hub_pwr_delay_ms = (uint32_t)hub_desc[5] * 2U;
            if (hub_pwr_delay_ms < 50U) {
                hub_pwr_delay_ms = 50U;
            }
        }

        /* 6. Configure Endpoint(s) and Hub Slot Context in xHCI */
        memset(dev->in_ctx, 0, 33U * ctx_size);
        memcpy(dev->in_ctx + ctx_size, dev->out_ctx, ctx_size);
        slot_dw = (uint32_t *)(void *)(dev->in_ctx + ctx_size);
        slot_dw[0] = (slot_dw[0] & 0x07FFFFFFUL) | ((uint32_t)max_dci << 27);
        if (dev->dev_type == USB_DEV_HUB) {
            slot_dw[0] |= (1U << 26); /* Set Hub = 1 in Slot Context */
            slot_dw[1] = (slot_dw[1] & 0x00FFFFFFUL) | ((uint32_t)dev->hub_ports << 24);
            if (speed == 3U) {
                slot_dw[2] = (slot_dw[2] & ~0x00030000UL) | ((uint32_t)dev->hub_ttt << 16);
            }
        }

        if (dev->int_in_dci != 0) {
            ring_init(&dev->int_in_ring);
            dev->hid_buf = (uint8_t *)dma_alloc(64, 64);
            xhci_setup_ep_ctx(dev->in_ctx + ((uint32_t)(dev->int_in_dci + 1U) * ctx_size),
                              7U, dev->int_in_mps, 0U, 6U, 8U, &dev->int_in_ring);
            add_flags |= (1U << dev->int_in_dci);
        }

        if (dev->bulk_in_dci != 0) {
            ring_init(&dev->bulk_in_ring);
            xhci_setup_ep_ctx(dev->in_ctx + ((uint32_t)(dev->bulk_in_dci + 1U) * ctx_size),
                              6U, dev->bulk_in_mps, dev->bulk_in_burst, 0U, 512U, &dev->bulk_in_ring);
            add_flags |= (1U << dev->bulk_in_dci);
        }

        if (dev->bulk_out_dci != 0) {
            ring_init(&dev->bulk_out_ring);
            xhci_setup_ep_ctx(dev->in_ctx + ((uint32_t)(dev->bulk_out_dci + 1U) * ctx_size),
                              2U, dev->bulk_out_mps, dev->bulk_out_burst, 0U, 512U, &dev->bulk_out_ring);
            add_flags |= (1U << dev->bulk_out_dci);
        }

        in_ctrl_dw = (uint32_t *)(void *)dev->in_ctx;
        in_ctrl_dw[0] = 0;
        in_ctrl_dw[1] = add_flags;

        if (xhci_send_command((uint32_t)(uintptr_t)dev->in_ctx, 0,
                              (TRB_TYPE_CONFIGURE_EP << 10) | ((uint32_t)slot_id << 24),
                              &ev) < 0) {
            kprintf("[XHCI] Configure Endpoint failed on slot %u\n", (uint32_t)slot_id);
            return -1;
        }

        if (dev->dev_type == USB_DEV_HUB) {
            if (hub_dev_idx < 0) {
                hub_dev_idx = dev_idx;
            }
            xhci_init_hub_device(dev, tier, hub_pwr_delay_ms);
        }
    }

    if (dev->dev_type == USB_DEV_KBD) {
        (void)xhci_control_transfer(dev, 0x21U, 0x0BU, 0, dev->iface_num, 0, NULL);
        (void)xhci_control_transfer(dev, 0x21U, 0x0AU, 0, dev->iface_num, 0, NULL);
        kbd_dev_idx = dev_idx;
        kprintf("[XHCI] USB Keyboard initialized (slot %u, root_port %u, route 0x%x, EP DCI %u)\n",
                (uint32_t)slot_id, (uint32_t)root_port, route_string, (uint32_t)dev->int_in_dci);
        xhci_queue_hid_int_trb(dev);
    } else if (dev->dev_type == USB_DEV_MOUSE) {
        (void)xhci_control_transfer(dev, 0x21U, 0x0BU, 0, dev->iface_num, 0, NULL);
        mouse_dev_idx = dev_idx;
        kprintf("[XHCI] USB Mouse initialized (slot %u, root_port %u, route 0x%x, EP DCI %u)\n",
                (uint32_t)slot_id, (uint32_t)root_port, route_string, (uint32_t)dev->int_in_dci);
        xhci_queue_hid_int_trb(dev);
    } else if (dev->dev_type == USB_DEV_STORAGE) {
        kprintf("[XHCI] USB Mass Storage initialized (slot %u, root_port %u, route 0x%x, IN=%u OUT=%u)\n",
                (uint32_t)slot_id, (uint32_t)root_port, route_string,
                (uint32_t)dev->bulk_in_dci, (uint32_t)dev->bulk_out_dci);
        xhci_init_msc_device(dev, dev_idx);
    }

    return 0;
}

static void xhci_reset_root_port(uint32_t port_off)
{
    uint32_t portsc = mmio_read32(op_base, port_off);
    int timeout_ms = 250;

    mmio_write32(op_base, port_off, ((portsc & ~PORTSC_RW1C_MASK) | (1U << 9) | (1U << 4)));
    while (timeout_ms > 0) {
        portsc = mmio_read32(op_base, port_off);
        if ((portsc & (1U << 1)) != 0 && (portsc & (1U << 4)) == 0) {
            break;
        }
        pit_mdelay(1U);
        timeout_ms--;
    }

    /* If standard port reset did not enable the port, try Warm Port Reset (WPR, bit 31) */
    portsc = mmio_read32(op_base, port_off);
    if ((portsc & (1U << 1)) == 0 && (portsc & 1U) != 0) {
        timeout_ms = 150;
        mmio_write32(op_base, port_off, ((portsc & ~PORTSC_RW1C_MASK) | (1U << 9) | (1U << 31)));
        while (timeout_ms > 0) {
            portsc = mmio_read32(op_base, port_off);
            if ((portsc & (1U << 1)) != 0 && (portsc & (1U << 4)) == 0) {
                break;
            }
            pit_mdelay(1U);
            timeout_ms--;
        }
    }

    /* Clear Port Reset Change (PRC, bit 21) and all RW1C status change bits */
    portsc = mmio_read32(op_base, port_off);
    mmio_write32(op_base, port_off,
                 (portsc & ~PORTSC_RW1C_MASK) | (1U << 9) | (0x7FU << 17));
    pit_mdelay(20U); /* USB reset recovery time (T_RSTRCY) */
}

static void xhci_enumerate_root_port(uint32_t port_num)
{
    uint32_t port_off = 0x400U + (port_num - 1U) * 0x10U;
    uint32_t portsc   = mmio_read32(op_base, port_off);
    bool_t   was_reset = FALSE;
    uint8_t  speed;

    /* Ensure Port Power (PP, bit 9) is enabled */
    if ((portsc & (1U << 9)) == 0) {
        mmio_write32(op_base, port_off, (portsc & ~PORTSC_RW1C_MASK) | (1U << 9));
        pit_mdelay(20U);
        portsc = mmio_read32(op_base, port_off);
    }

    /* Clear any pending status change bits while preserving PED */
    if ((portsc & (0x7FU << 17)) != 0) {
        mmio_write32(op_base, port_off, (portsc & ~PORTSC_RW1C_MASK) | (0x7FU << 17));
        portsc = mmio_read32(op_base, port_off);
    }

    /* If port is connected (CCS=1) but not yet enabled (PED=0), issue Port Reset (PR=1) */
    if ((portsc & (1U << 1)) == 0) {
        xhci_reset_root_port(port_off);
        was_reset = TRUE;
    }

    portsc = mmio_read32(op_base, port_off);
    if ((portsc & 0x03U) != 0x03U) {
        return;
    }

    speed = (uint8_t)((portsc >> 10) & 0x0FU);
    if (xhci_enumerate_device((uint8_t)port_num, 0U, 0U, speed, 0U, 0U) < 0 && !was_reset) {
        /* If pre-enabled port failed enumeration, force a port reset and retry once */
        xhci_reset_root_port(port_off);
        portsc = mmio_read32(op_base, port_off);
        if ((portsc & 0x03U) == 0x03U) {
            speed = (uint8_t)((portsc >> 10) & 0x0FU);
            (void)xhci_enumerate_device((uint8_t)port_num, 0U, 0U, speed, 0U, 0U);
        }
    }
}

/* Perform xHCI Extended Capability BIOS-to-OS Handoff (USBLEGSUP, Cap ID = 1) */
static void xhci_bios_handoff(uint32_t hccparams1)
{
    uint32_t xecp_dwords = (hccparams1 >> 16) & 0xFFFFU;
    uint32_t ext_off     = xecp_dwords * 4U;
    int      max_caps    = 64;

    while (ext_off != 0 && max_caps > 0) {
        uint32_t reg    = mmio_read32(cap_base, ext_off);
        uint8_t  cap_id = (uint8_t)(reg & 0xFFU);
        uint8_t  next   = (uint8_t)((reg >> 8) & 0xFFU);

        if (reg == 0xFFFFFFFFUL) {
            break;
        }

        if (cap_id == 1U) {
            /* USB Legacy Support Capability (USBLEGSUP) */
            if ((reg & (1U << 16)) != 0) {
                int wait_ms = 1000;
                kprintf("[XHCI] Requesting BIOS-to-OS handoff (USBLEGSUP=0x%08x)...\n", reg);
                mmio_write32(cap_base, ext_off, reg | (1U << 24));
                while (wait_ms > 0) {
                    reg = mmio_read32(cap_base, ext_off);
                    if ((reg & (1U << 16)) == 0) {
                        break;
                    }
                    pit_mdelay(1U);
                    wait_ms--;
                }
                /* Force clear BIOS ownership bit if firmware timed out */
                if ((reg & (1U << 16)) != 0) {
                    mmio_write32(cap_base, ext_off, (reg & ~(1U << 16)) | (1U << 24));
                }
            }
            /* Disable all Legacy SMI enables in USBLEGCTLSTS (ext_off + 4) and clear RW1C status */
            {
                uint32_t ctlsts = mmio_read32(cap_base, ext_off + 4U);
                ctlsts &= ~((1U << 0) | (1U << 4) | (1U << 13) | (1U << 14) | (1U << 15));
                ctlsts |= (7U << 29);
                mmio_write32(cap_base, ext_off + 4U, ctlsts);
            }
            kprintf("[XHCI] BIOS-to-OS handoff complete\n");
            break;
        }

        if (next == 0) {
            break;
        }
        ext_off += (uint32_t)next * 4U;
        max_caps--;
    }
}

/* Route USB 2.0 / 3.0 ports from EHCI to xHCI on Intel 7/8/9-series & BayTrail/Braswell chipsets */
static void xhci_intel_port_switchover(const struct pci_device *pci_dev)
{
    if (pci_dev->vendor_id == 0x8086U &&
        (pci_dev->device_id == 0x1E31U || pci_dev->device_id == 0x8C31U ||
         pci_dev->device_id == 0x8CB1U || pci_dev->device_id == 0x9C31U ||
         pci_dev->device_id == 0x9CB1U || pci_dev->device_id == 0x0F35U ||
         pci_dev->device_id == 0x22B5U)) {
        uint32_t usb3_mask = pci_read32(pci_dev->bus, pci_dev->slot, pci_dev->func, 0xDC);
        uint32_t usb2_mask = pci_read32(pci_dev->bus, pci_dev->slot, pci_dev->func, 0xD4);
        pci_write32(pci_dev->bus, pci_dev->slot, pci_dev->func, 0xD8, usb3_mask);
        pci_write32(pci_dev->bus, pci_dev->slot, pci_dev->func, 0xD0, usb2_mask);
        kprintf("[XHCI] Intel EHCI->xHCI port switchover applied (USB2=0x%x, USB3=0x%x)\n",
                usb2_mask, usb3_mask);
    }
}

/* Allocate xHCI Scratchpad Buffers required by real hardware controllers (HCSPARAMS2) */
static void xhci_setup_scratchpad(uint32_t hcsparams2)
{
    uint32_t num_sp = (((hcsparams2 >> 21) & 0x1FU) << 5) | ((hcsparams2 >> 27) & 0x1FU);
    uint32_t *sp_arr;
    uint32_t i;

    if (num_sp == 0) {
        return;
    }
    if (num_sp > 64U) {
        num_sp = 64U;
    }

    sp_arr = (uint32_t *)dma_alloc(num_sp * 8U, 64U);
    if (sp_arr == NULL) {
        return;
    }

    for (i = 0; i < num_sp; i++) {
        void *page = dma_alloc(4096U, 4096U);
        if (page == NULL) {
            break;
        }
        sp_arr[i * 2U]      = (uint32_t)(uintptr_t)page;
        sp_arr[i * 2U + 1U] = 0;
    }

    dcbaa[0] = (uint32_t)(uintptr_t)sp_arr;
    dcbaa[1] = 0;
    kprintf("[XHCI] Allocated %u hardware scratchpad buffers (4 KiB aligned)\n", num_sp);
}

static int xhci_init_controller(const struct pci_device *pci_dev)
{
    uint32_t cap0;
    uint8_t  cap_len;
    uint32_t hcsparams1;
    uint32_t hcsparams2;
    uint32_t hccparams1;
    uint32_t dboff;
    uint32_t rtsoff;
    uint32_t usbcmd;
    int      timeout;
    uint32_t port;
    int      i;
    bool_t   any_connected = FALSE;

    xhci_ready          = FALSE;
    xhci_dma_off        = 0;
    num_usb_devs        = 0;
    hub_dev_idx         = -1;
    kbd_dev_idx         = -1;
    mouse_dev_idx       = -1;
    num_msc_devs        = 0;
    root_port_conn_mask = 0;
    hotplug_pending     = FALSE;
    for (i = 0; i < XHCI_MAX_MSC_DEVS; i++) {
        msc_dev_indices[i] = -1;
    }
    memset(&hub_info, 0, sizeof(hub_info));
    memset(msc_infos, 0, sizeof(msc_infos));

    xhci_intel_port_switchover(pci_dev);

    cap_base   = (volatile uint8_t *)(uintptr_t)pci_dev->bar0_mmio;
    cap0       = mmio_read32(cap_base, 0x00);
    if (cap0 == 0xFFFFFFFFUL || (cap0 & 0xFFU) == 0U) {
        kprintf("[XHCI] BAR0 MMIO at 0x%08x inaccessible (CAPLENGTH=0x%08x)\n",
                pci_dev->bar0_mmio, cap0);
        return -1;
    }
    cap_len    = (uint8_t)(cap0 & 0xFFU);
    hcsparams1 = mmio_read32(cap_base, 0x04);
    hcsparams2 = mmio_read32(cap_base, 0x08);
    hccparams1 = mmio_read32(cap_base, 0x10);
    dboff      = mmio_read32(cap_base, 0x14) & ~0x03UL;
    rtsoff     = mmio_read32(cap_base, 0x18) & ~0x1FUL;

    op_base = cap_base + cap_len;
    rt_base = cap_base + rtsoff;
    db_base = (volatile uint32_t *)(void *)(cap_base + dboff);

    max_slots = hcsparams1 & 0xFFU;
    max_ports = (hcsparams1 >> 24) & 0xFFU;
    ctx_size  = ((hccparams1 & 0x04U) != 0) ? 64U : 32U;

    if (max_slots > XHCI_MAX_SLOTS) {
        max_slots = XHCI_MAX_SLOTS;
    }

    /* Perform BIOS-to-OS handoff before stopping or resetting the controller */
    xhci_bios_handoff(hccparams1);

    timeout = 1000;
    while ((mmio_read32(op_base, 0x04) & (1U << 11)) != 0 && timeout > 0) {
        pit_mdelay(1U);
        timeout--;
    }

    usbcmd = mmio_read32(op_base, 0x00);
    mmio_write32(op_base, 0x00, usbcmd & ~1U);
    timeout = 500;
    while ((mmio_read32(op_base, 0x04) & 1U) == 0 && timeout > 0) {
        pit_mdelay(1U);
        timeout--;
    }

    mmio_write32(op_base, 0x00, (1U << 1));
    timeout = 1000;
    while ((mmio_read32(op_base, 0x00) & (1U << 1)) != 0 && timeout > 0) {
        pit_mdelay(1U);
        timeout--;
    }
    timeout = 1000;
    while ((mmio_read32(op_base, 0x04) & (1U << 11)) != 0 && timeout > 0) {
        pit_mdelay(1U);
        timeout--;
    }

    /* Re-apply Intel EHCI->xHCI port routing after HCRST */
    xhci_intel_port_switchover(pci_dev);

    mmio_write32(op_base, 0x38, max_slots);

    dcbaa = (volatile uint32_t *)dma_alloc(256U * 8U, 64);
    xhci_setup_scratchpad(hcsparams2);
    mmio_write32(op_base, 0x30, (uint32_t)(uintptr_t)dcbaa);
    mmio_write32(op_base, 0x34, 0);

    ring_init(&cmd_ring);
    mmio_write32(op_base, 0x18, ((uint32_t)(uintptr_t)&cmd_ring.trbs[0]) | 1U);
    mmio_write32(op_base, 0x1C, 0);

    evt_ring    = (volatile struct xhci_trb *)dma_alloc(XHCI_RING_TRBS * sizeof(struct xhci_trb), 64);
    evt_deq_idx = 0;
    evt_ccs     = 1;

    erst = (struct xhci_erst_entry *)dma_alloc(sizeof(struct xhci_erst_entry), 64);
    erst[0].base_lo  = (uint32_t)(uintptr_t)&evt_ring[0];
    erst[0].base_hi  = 0;
    erst[0].size     = XHCI_RING_TRBS;
    erst[0].reserved = 0;

    mmio_write32(rt_base, 0x20 + 0x08, 1U);
    mmio_write32(rt_base, 0x20 + 0x18, ((uint32_t)(uintptr_t)&evt_ring[0]) | (1U << 3));
    mmio_write32(rt_base, 0x20 + 0x1C, 0);
    mmio_write32(rt_base, 0x20 + 0x10, (uint32_t)(uintptr_t)erst);
    mmio_write32(rt_base, 0x20 + 0x14, 0);

    ctrl_dma_buf = (uint8_t *)dma_alloc(512, 64);
    msc_cbw_buf  = (uint8_t *)dma_alloc(64, 64);
    msc_csw_buf  = (uint8_t *)dma_alloc(64, 64);
    msc_data_buf = (uint8_t *)dma_alloc(4096, 64);

    mmio_write32(op_base, 0x00, 1U);
    timeout = 500;
    while ((mmio_read32(op_base, 0x04) & 1U) != 0 && timeout > 0) {
        pit_mdelay(1U);
        timeout--;
    }

    xhci_ready = TRUE;
    kprintf("[XHCI] Host controller running (ports=%u, max_slots=%u, ctx=%uB)\n",
            max_ports, max_slots, ctx_size);

    /* Ensure Port Power (PP, bit 9) is enabled on all root ports after HCRST */
    for (port = 1; port <= max_ports; port++) {
        uint32_t port_off = 0x400U + (port - 1U) * 0x10U;
        uint32_t portsc   = mmio_read32(op_base, port_off);
        if ((portsc & (1U << 9)) == 0) {
            mmio_write32(op_base, port_off, (portsc & ~PORTSC_RW1C_MASK) | (1U << 9));
        }
    }

    /* Wait for physical root ports to power up and complete link training (CCS=1) */
    pit_mdelay(25U);
    for (timeout = 0; timeout < 20; timeout++) {
        for (port = 1; port <= max_ports; port++) {
            uint32_t portsc = mmio_read32(op_base, 0x400U + (port - 1U) * 0x10U);
            if ((portsc & 1U) != 0) {
                any_connected = TRUE;
                break;
            }
        }
        if (any_connected) {
            break;
        }
        pit_mdelay(10U);
    }
    if (any_connected) {
        /* Debounce delay so all sibling USB 2.0 / 3.0 ports also assert CCS=1 */
        pit_mdelay(30U);
    }

    for (port = 1; port <= max_ports && num_usb_devs < XHCI_MAX_SLOTS; port++) {
        uint32_t portsc = mmio_read32(op_base, 0x400U + (port - 1U) * 0x10U);
        if ((portsc & 1U) != 0) {
            if (port <= 32U) {
                root_port_conn_mask |= (1U << (port - 1U));
            }
            xhci_enumerate_root_port(port);
        }
    }

    last_hotplug_tick     = pit_get_ticks();
    ctrl_info.present     = TRUE;
    ctrl_info.bus         = pci_dev->bus;
    ctrl_info.slot        = pci_dev->slot;
    ctrl_info.func        = pci_dev->func;
    ctrl_info.vendor_id   = pci_dev->vendor_id;
    ctrl_info.device_id   = pci_dev->device_id;
    ctrl_info.bar0_mmio   = pci_dev->bar0_mmio;
    ctrl_info.max_ports   = max_ports;
    ctrl_info.max_slots   = max_slots;
    ctrl_info.num_devices = (uint32_t)num_usb_devs;

    return 0;
}

int xhci_init(void)
{
    struct pci_device pci_dev;
    int ctrl_idx;
    int initialized_any = 0;

    memset(&ctrl_info, 0, sizeof(ctrl_info));

    for (ctrl_idx = 0; ctrl_idx < 4; ctrl_idx++) {
        if (pci_find_class_nth(0x0CU, 0x03U, 0x30U, ctrl_idx, &pci_dev) < 0) {
            break;
        }
        if (xhci_init_controller(&pci_dev) == 0) {
            initialized_any = 1;
            if (num_usb_devs > 0) {
                break;
            }
        }
    }

    if (!initialized_any) {
        kprintf("[XHCI] No PCI xHCI controller detected\n");
        return -1;
    }
    return 0;
}

bool_t xhci_is_initialized(void)
{
    return xhci_ready;
}

bool_t xhci_has_usb_hub(void)
{
    return hub_info.present;
}

bool_t xhci_has_usb_kbd(void)
{
    return (kbd_dev_idx >= 0) ? TRUE : FALSE;
}

bool_t xhci_has_usb_mouse(void)
{
    return (mouse_dev_idx >= 0) ? TRUE : FALSE;
}

bool_t xhci_has_usb_storage(void)
{
    return (num_msc_devs > 0) ? TRUE : FALSE;
}

int xhci_get_msc_count(void)
{
    return num_msc_devs;
}

int xhci_get_msc_info_idx(int disk_idx, struct xhci_msc_info *out)
{
    if (disk_idx < 0 || disk_idx >= num_msc_devs || !msc_infos[disk_idx].present) {
        if (out != NULL) {
            memset(out, 0, sizeof(*out));
        }
        return -1;
    }
    if (out != NULL) {
        *out = msc_infos[disk_idx];
    }
    return 0;
}

void xhci_get_controller_info(struct xhci_controller_info *out)
{
    if (out != NULL) {
        *out = ctrl_info;
    }
}

void xhci_get_hub_info(struct xhci_hub_info *out)
{
    if (out != NULL) {
        *out = hub_info;
    }
}

void xhci_get_msc_info(struct xhci_msc_info *out)
{
    (void)xhci_get_msc_info_idx(0, out);
}
