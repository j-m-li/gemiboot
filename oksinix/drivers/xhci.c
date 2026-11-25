/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - xHCI USB 3.0 Host Controller Driver
 * Class drivers live in usbhub.c (USB Hub), usbhid.c (HID Kbd/Mouse), and usbmsc.c (BOT/SCSI)
 */

#include "xhci_internal.h"
#include "pci.h"
#include "pit.h"
#include "io.h"
#include "serial.h"
#include "string.h"

/* xHCI DMA pool size (512 KiB for Scratchpad pages + rings/buffers) */
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
#define TRB_ISP                    (1U << 2)
#define TRB_IOC                    (1U << 5)
#define TRB_IDT                    (1U << 6)
#define TRB_DIR_IN                 (1U << 16)

/* 16-byte Event Ring Segment Table (ERST) entry */
struct xhci_erst_entry {
    uint32_t base_lo;
    uint32_t base_hi;
    uint32_t size;
    uint32_t reserved;
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

static struct xhci_controller_info ctrl_info;
static uint32_t                 root_port_conn_mask = 0;
static uint32_t                 last_hotplug_tick   = 0;
static bool_t                   hotplug_pending     = FALSE;

/* Forward declarations */
static void xhci_enumerate_root_port(uint32_t port_num);

static uint32_t mmio_read32(volatile const uint8_t *base, uint32_t offset)
{
    return *(volatile const uint32_t *)(base + offset);
}

static void mmio_write32(volatile uint8_t *base, uint32_t offset, uint32_t val)
{
    *(volatile uint32_t *)(base + offset) = val;
}

void *xhci_dma_alloc(uint32_t size, uint32_t align)
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
    if (r->trbs == NULL) {
        r->trbs = (volatile struct xhci_trb *)xhci_dma_alloc(XHCI_RING_TRBS * sizeof(struct xhci_trb), 64);
    } else {
        memset((void *)(uintptr_t)r->trbs, 0, XHCI_RING_TRBS * sizeof(struct xhci_trb));
    }
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

void xhci_interrupt_in_submit(struct xhci_usb_dev *dev, void *buf, uint32_t len)
{
    ring_enqueue(&dev->int_in_ring,
                 (uint32_t)(uintptr_t)buf,
                 0,
                 len,
                 (TRB_TYPE_NORMAL << 10) | TRB_IOC | TRB_ISP);
    db_base[dev->slot_id] = dev->int_in_dci;
}

void xhci_bulk_in_submit(struct xhci_usb_dev *dev, void *buf, uint32_t len)
{
    ring_enqueue(&dev->bulk_in_ring,
                 (uint32_t)(uintptr_t)buf,
                 0,
                 len,
                 (TRB_TYPE_NORMAL << 10) | TRB_IOC | TRB_ISP);
    db_base[dev->slot_id] = dev->bulk_in_dci;
}

bool_t xhci_has_free_slot(void)
{
    return (num_usb_devs < XHCI_MAX_SLOTS) ? TRUE : FALSE;
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
    uint8_t  ep_dci   = (uint8_t)((ev->control >> 16) & 0x1FU);

    if (trb_type == TRB_TYPE_TRANSFER_EVENT) {
        if (!usbhid_handle_transfer_event(slot_id)) {
            uint8_t  cc       = (uint8_t)((ev->status >> 24) & 0xFFU);
            uint32_t residual = ev->status & 0x00FFFFFFU;
            (void)usbnet_handle_transfer_event(slot_id, ep_dci, cc, residual);
        }
    } else if (trb_type == TRB_TYPE_PORT_STATUS_CHG) {
        hotplug_pending = TRUE;
    }
}

static int xhci_wait_event(uint32_t expected_type, uint8_t expected_slot,
                           uint8_t expected_dci, struct xhci_trb *out)
{
    int timeout = 25000;
    struct xhci_trb ev;

    while (timeout > 0) {
        if (xhci_pop_event(&ev)) {
            uint32_t trb_type = (ev.control >> 10) & 0x3FU;
            uint8_t  slot_id  = (uint8_t)((ev.control >> 24) & 0xFFU);
            uint8_t  ep_dci   = (uint8_t)((ev.control >> 16) & 0x1FU);

            if (trb_type == expected_type &&
                (expected_slot == 0 || slot_id == expected_slot) &&
                (expected_dci == 0 || ep_dci == expected_dci)) {
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

        hotplug_pending   = FALSE;
        last_hotplug_tick = now_ticks;

        for (port = 1; port <= max_ports && port <= 32U; port++) {
            uint32_t port_off = 0x400U + (port - 1U) * 0x10U;
            uint32_t portsc   = mmio_read32(op_base, port_off);
            uint32_t bit      = (1U << (port - 1U));

            if ((portsc & 1U) == 0) {
                if ((root_port_conn_mask & bit) != 0) {
                    root_port_conn_mask &= ~bit;
                    if ((portsc & (0x7FU << 17)) != 0) {
                        mmio_write32(op_base, port_off, (portsc & ~PORTSC_RW1C_MASK) | (0x7FU << 17));
                    }
                    xhci_disconnect_port((uint8_t)port, 0U, 0U);
                    ctrl_info.num_devices = (uint32_t)num_usb_devs;
                }
            } else if ((root_port_conn_mask & bit) == 0 && num_usb_devs < XHCI_MAX_SLOTS) {
                root_port_conn_mask |= bit;
                pit_mdelay(30U);
                xhci_enumerate_root_port(port);
                ctrl_info.num_devices = (uint32_t)num_usb_devs;
            }
        }

        /* Also scan downstream ports of any enumerated external USB hubs for newly ready devices */
        usbhub_poll_ports();
        ctrl_info.num_devices = (uint32_t)num_usb_devs;
    }
}

static int xhci_send_command(uint32_t param_lo, uint32_t param_hi, uint32_t control,
                             struct xhci_trb *out_ev)
{
    ring_enqueue(&cmd_ring, param_lo, param_hi, 0, control);
    db_base[0] = 0;
    if (xhci_wait_event(TRB_TYPE_CMD_COMPLETION, 0, 0, out_ev) < 0) {
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

int xhci_control_transfer(struct xhci_usb_dev *dev,
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

    if (wLength > 1024U) {
        return -1;
    }
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

    if (xhci_wait_event(TRB_TYPE_TRANSFER_EVENT, dev->slot_id, 1U, &ev) < 0) {
        return -1;
    }

    cc = (uint8_t)((ev.status >> 24) & 0xFFU);

    /* If controller generated a Short Packet event on the Data Stage TRB, also consume Status Stage */
    if (cc == 13U && ev.param_lo != status_trb_phys) {
        if (xhci_wait_event(TRB_TYPE_TRANSFER_EVENT, dev->slot_id, 1U, &ev) < 0) {
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

void xhci_clear_bulk_stall(struct xhci_usb_dev *dev, bool_t is_in)
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

int xhci_bulk_transfer(struct xhci_usb_dev *dev, bool_t is_in,
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

    if (xhci_wait_event(TRB_TYPE_TRANSFER_EVENT, dev->slot_id, dci, &ev) < 0) {
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

static void xhci_release_dev(struct xhci_usb_dev *dev)
{
    struct xhci_trb ev;
    uint8_t slot_id = dev->slot_id;

    if (slot_id != 0) {
        (void)xhci_send_command(0, 0,
                                (TRB_TYPE_DISABLE_SLOT << 10) | ((uint32_t)slot_id << 24),
                                &ev);
        dcbaa[slot_id * 2U] = 0;
        dev->slot_id = 0;
        dev->dev_type = USB_DEV_NONE;
        if (num_usb_devs > 0) {
            num_usb_devs--;
        }
    }
}

void xhci_disconnect_port(uint8_t root_port, uint8_t parent_hub_slot, uint8_t parent_port)
{
    int i;

    for (i = 0; i < XHCI_MAX_SLOTS; i++) {
        struct xhci_usb_dev *dev = &usb_devs[i];
        if (dev->slot_id == 0) {
            continue;
        }
        if (dev->root_port == root_port &&
            dev->parent_hub_slot == parent_hub_slot &&
            dev->parent_port == parent_port) {
            uint8_t hub_slot = dev->slot_id;
            if (dev->dev_type == USB_DEV_HUB) {
                int j;
                for (j = 0; j < XHCI_MAX_SLOTS; j++) {
                    if (usb_devs[j].slot_id != 0 && usb_devs[j].parent_hub_slot == hub_slot) {
                        xhci_disconnect_port(usb_devs[j].root_port, hub_slot, usb_devs[j].parent_port);
                    }
                }
                usbhub_remove_device(dev);
            } else if (dev->dev_type == USB_DEV_STORAGE) {
                usbmsc_remove_device(dev);
            } else if (dev->dev_type == USB_DEV_KBD || dev->dev_type == USB_DEV_MOUSE) {
                usbhid_remove_device(dev);
            } else if (dev->dev_type == USB_DEV_NET_RNDIS || dev->dev_type == USB_DEV_NET_ECM) {
                usbnet_remove_device(dev);
            }
            xhci_release_dev(dev);
        }
    }
}

int xhci_enumerate_device(uint8_t root_port, uint32_t route_string,
                          uint32_t tier, uint8_t speed,
                          uint8_t parent_hub_slot, uint8_t parent_port)
{
    struct xhci_trb ev;
    uint8_t slot_id;
    int dev_idx = -1;
    int i;
    struct xhci_usb_dev *dev;
    uint8_t *saved_out_ctx;
    uint8_t *saved_in_ctx;
    uint8_t *saved_hid_buf;
    volatile struct xhci_trb *saved_ep0;
    volatile struct xhci_trb *saved_int;
    volatile struct xhci_trb *saved_bin;
    volatile struct xhci_trb *saved_bout;
    uint32_t *in_ctrl_dw;
    uint32_t *slot_dw;
    uint8_t dev_desc[18];
    static uint8_t cfg_desc[1024];
    uint16_t total_len;
    uint32_t pos;
    uint8_t max_dci = 1;
    uint32_t add_flags = (1U << 0);

    for (i = 0; i < XHCI_MAX_SLOTS; i++) {
        if (usb_devs[i].slot_id == 0) {
            dev_idx = i;
            break;
        }
    }
    if (dev_idx < 0) {
        return -1;
    }

    /* 1. Enable Slot Command */
    if (xhci_send_command(0, 0, (TRB_TYPE_ENABLE_SLOT << 10), &ev) < 0) {
        kprintf("[XHCI] Enable Slot failed (root_port=%u route=0x%x)\n",
                (uint32_t)root_port, route_string);
        return -1;
    }
    slot_id = (uint8_t)((ev.control >> 24) & 0xFFU);

    num_usb_devs++;
    dev = &usb_devs[dev_idx];
    saved_out_ctx = dev->out_ctx;
    saved_in_ctx  = dev->in_ctx;
    saved_hid_buf = dev->hid_buf;
    saved_ep0     = dev->ep0_ring.trbs;
    saved_int     = dev->int_in_ring.trbs;
    saved_bin     = dev->bulk_in_ring.trbs;
    saved_bout    = dev->bulk_out_ring.trbs;
    memset(dev, 0, sizeof(struct xhci_usb_dev));
    dev->out_ctx            = saved_out_ctx;
    dev->in_ctx             = saved_in_ctx;
    dev->hid_buf            = saved_hid_buf;
    dev->ep0_ring.trbs      = saved_ep0;
    dev->int_in_ring.trbs   = saved_int;
    dev->bulk_in_ring.trbs  = saved_bin;
    dev->bulk_out_ring.trbs = saved_bout;

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

    if (dev->out_ctx == NULL) {
        dev->out_ctx = (uint8_t *)xhci_dma_alloc(32U * ctx_size, 64);
    } else {
        memset(dev->out_ctx, 0, 32U * ctx_size);
    }
    if (dev->in_ctx == NULL) {
        dev->in_ctx = (uint8_t *)xhci_dma_alloc(33U * ctx_size, 64);
    } else {
        memset(dev->in_ctx, 0, 33U * ctx_size);
    }
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
            xhci_release_dev(dev);
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
            xhci_release_dev(dev);
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
        xhci_release_dev(dev);
        return -1;
    }

    dev->vendor_id  = (uint16_t)((uint16_t)dev_desc[8] | ((uint16_t)dev_desc[9] << 8));
    dev->product_id = (uint16_t)((uint16_t)dev_desc[10] | ((uint16_t)dev_desc[11] << 8));

    if (dev_desc[4] == 0x09U) {
        dev->dev_type = USB_DEV_HUB;
    }

    /* 4. Read Configuration Descriptor(s) and match class/endpoints */
    {
        uint8_t num_cfgs = (dev_desc[17] > 0 && dev_desc[17] <= 4U) ? dev_desc[17] : 1U;
        uint8_t cfg_idx;

        for (cfg_idx = 0; cfg_idx < num_cfgs; cfg_idx++) {
            bool_t  capture_eps;
            bool_t  iface_locked = FALSE;
            uint8_t last_ep_dci  = 0;
            bool_t  iad_valid    = FALSE;
            uint8_t iad_first    = 0;
            uint8_t iad_count    = 0;
            uint32_t iad_type    = USB_DEV_NONE;
            bool_t  has_union_data = FALSE;
            uint8_t union_data_if  = 0;

            memset(cfg_desc, 0, sizeof(cfg_desc));
            if (xhci_control_transfer(dev, 0x80U, 0x06U,
                                      (uint16_t)(0x0200U | cfg_idx), 0, 9U, cfg_desc) < 0) {
                if (cfg_idx == 0) {
                    xhci_release_dev(dev);
                    return -1;
                }
                break;
            }
            total_len = (uint16_t)((uint16_t)cfg_desc[2] | ((uint16_t)cfg_desc[3] << 8));
            if (total_len > sizeof(cfg_desc)) {
                total_len = sizeof(cfg_desc);
            }
            if (xhci_control_transfer(dev, 0x80U, 0x06U,
                                      (uint16_t)(0x0200U | cfg_idx), 0, total_len, cfg_desc) < 0) {
                if (cfg_idx == 0) {
                    xhci_release_dev(dev);
                    return -1;
                }
                break;
            }

            if (dev_desc[4] != 0x09U) {
                dev->dev_type = USB_DEV_NONE;
            }
            dev->config_val      = cfg_desc[5];
            dev->iface_num       = 0;
            dev->net_data_iface  = 0;
            dev->net_data_alt    = 0;
            dev->net_mac_str_idx = 0;
            dev->int_in_dci      = 0;
            dev->int_in_mps      = 0;
            dev->int_in_interval = 6U;
            dev->bulk_in_dci     = 0;
            dev->bulk_in_mps     = 0;
            dev->bulk_in_burst   = 0;
            dev->bulk_out_dci    = 0;
            dev->bulk_out_mps    = 0;
            dev->bulk_out_burst  = 0;
            max_dci              = 1;
            capture_eps          = (dev->dev_type == USB_DEV_HUB) ? TRUE : FALSE;

            pos = 0;
            while (pos + 2U <= total_len) {
                uint8_t bLength = cfg_desc[pos];
                uint8_t bDescriptorType = cfg_desc[pos + 1U];

                if (bLength == 0) {
                    break;
                }
                if (bDescriptorType == 0x0BU && pos + 8U <= total_len) {
                    /* Interface Association Descriptor (IAD) for composite RNDIS/ECM hardware */
                    uint8_t f_cls   = cfg_desc[pos + 4U];
                    uint8_t f_sub   = cfg_desc[pos + 5U];
                    uint8_t f_proto = cfg_desc[pos + 6U];

                    iad_first = cfg_desc[pos + 2U];
                    iad_count = cfg_desc[pos + 3U];
                    iad_type  = USB_DEV_NONE;
                    if ((f_cls == 0x02U && f_sub == 0x02U && f_proto == 0xFFU) ||
                        (f_cls == 0xE0U && f_sub == 0x01U && f_proto == 0x03U) ||
                        (f_cls == 0xEFU && f_sub == 0x04U &&
                         (f_proto == 0x01U || f_proto == 0x02U || f_proto == 0x03U || f_proto == 0x04U))) {
                        iad_type = USB_DEV_NET_RNDIS;
                    } else if (f_cls == 0x02U && f_sub == 0x06U) {
                        iad_type = USB_DEV_NET_ECM;
                    }
                    iad_valid = (iad_type != USB_DEV_NONE) ? TRUE : FALSE;
                } else if (bDescriptorType == 4U && pos + 9U <= total_len) {
                    uint8_t iface_num   = cfg_desc[pos + 2U];
                    uint8_t alt_setting = cfg_desc[pos + 3U];
                    uint8_t num_eps     = cfg_desc[pos + 4U];
                    uint8_t iface_class = cfg_desc[pos + 5U];
                    uint8_t iface_sub   = cfg_desc[pos + 6U];
                    uint8_t iface_proto = cfg_desc[pos + 7U];
                    uint32_t cand_type  = USB_DEV_NONE;

                    last_ep_dci = 0;

                    /* CDC Data interface paired with an already-matched RNDIS or CDC-ECM control interface */
                    if ((dev->dev_type == USB_DEV_NET_RNDIS || dev->dev_type == USB_DEV_NET_ECM) &&
                        iface_class == 0x0AU) {
                        bool_t pair_ok = TRUE;
                        if (has_union_data && iface_num != union_data_if) {
                            pair_ok = FALSE;
                        } else if (!has_union_data && iad_valid &&
                                   (iface_num < iad_first || iface_num >= (uint8_t)(iad_first + iad_count))) {
                            pair_ok = FALSE;
                        }
                        if (pair_ok && num_eps >= 2U && (dev->bulk_in_dci == 0 || dev->bulk_out_dci == 0)) {
                            dev->net_data_iface = iface_num;
                            dev->net_data_alt   = alt_setting;
                            capture_eps         = TRUE;
                        } else {
                            capture_eps = FALSE;
                        }
                        pos += bLength;
                        continue;
                    }

                    /* Check if previous matched interface already captured all required endpoints */
                    if ((dev->dev_type == USB_DEV_KBD && dev->int_in_dci != 0) ||
                        (dev->dev_type == USB_DEV_MOUSE && dev->int_in_dci != 0) ||
                        (dev->dev_type == USB_DEV_STORAGE && dev->bulk_in_dci != 0 && dev->bulk_out_dci != 0) ||
                        (dev->dev_type == USB_DEV_HUB && dev->int_in_dci != 0) ||
                        ((dev->dev_type == USB_DEV_NET_RNDIS || dev->dev_type == USB_DEV_NET_ECM) &&
                         dev->bulk_in_dci != 0 && dev->bulk_out_dci != 0)) {
                        iface_locked = TRUE;
                    }

                    /* If a keyboard is already registered and a second composite device has a mouse interface, allow it */
                    if (iface_locked && dev->dev_type == USB_DEV_KBD &&
                        xhci_has_usb_kbd() && !xhci_has_usb_mouse() &&
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
                        } else if ((iface_class == 0x02U && iface_sub == 0x02U && iface_proto == 0xFFU) ||
                                   (iface_class == 0xE0U && iface_sub == 0x01U && iface_proto == 0x03U) ||
                                   (iface_class == 0xEFU && iface_sub == 0x04U &&
                                    (iface_proto == 0x01U || iface_proto == 0x02U ||
                                     iface_proto == 0x03U || iface_proto == 0x04U)) ||
                                   (iad_valid && iad_type == USB_DEV_NET_RNDIS &&
                                    iface_num == iad_first &&
                                    (iface_class == 0x02U || iface_class == 0xE0U ||
                                     iface_class == 0xEFU || iface_class == 0xFFU))) {
                            cand_type = USB_DEV_NET_RNDIS;
                        } else if (iface_class == 0x02U && iface_sub == 0x06U) {
                            cand_type = USB_DEV_NET_ECM;
                        } else if (iface_class == 0x03U && iface_proto == 0x00U &&
                                   dev->dev_type == USB_DEV_NONE) {
                            cand_type = (!xhci_has_usb_kbd()) ? USB_DEV_KBD : USB_DEV_MOUSE;
                        }

                        if (cand_type != USB_DEV_NONE) {
                            dev->dev_type       = cand_type;
                            dev->iface_num      = iface_num;
                            dev->int_in_dci     = 0;
                            dev->int_in_mps     = 0;
                            dev->int_in_interval = 6U;
                            dev->bulk_in_dci    = 0;
                            dev->bulk_in_mps    = 0;
                            dev->bulk_in_burst  = 0;
                            dev->bulk_out_dci   = 0;
                            dev->bulk_out_mps   = 0;
                            dev->bulk_out_burst = 0;
                            has_union_data      = FALSE;
                            capture_eps         = TRUE;
                        } else {
                            capture_eps = FALSE;
                        }
                    }
                } else if (bDescriptorType == 0x24U && pos + 4U <= total_len &&
                           (dev->dev_type == USB_DEV_NET_ECM || dev->dev_type == USB_DEV_NET_RNDIS)) {
                    if (cfg_desc[pos + 2U] == 0x06U && pos + 5U <= total_len &&
                        cfg_desc[pos + 3U] == dev->iface_num) {
                        /* CDC Union Functional Descriptor: bControlInterface + bSubordinateInterface0 */
                        union_data_if  = cfg_desc[pos + 4U];
                        has_union_data = TRUE;
                    } else if (dev->dev_type == USB_DEV_NET_ECM && cfg_desc[pos + 2U] == 0x0FU) {
                        /* CDC Ethernet Networking Functional Descriptor (subtype 0x0F -> iMACAddress) */
                        dev->net_mac_str_idx = cfg_desc[pos + 3U];
                    }
                } else if (bDescriptorType == 5U && pos + 7U <= total_len && capture_eps) {
                    uint8_t  ep_addr = cfg_desc[pos + 2U];
                    uint8_t  ep_attr = (uint8_t)(cfg_desc[pos + 3U] & 0x03U);
                    uint16_t ep_mps  = (uint16_t)(((uint16_t)cfg_desc[pos + 4U] |
                                                  ((uint16_t)cfg_desc[pos + 5U] << 8)) & 0x07FFU);
                    uint8_t  ep_bInt = cfg_desc[pos + 6U];
                    uint8_t  ep_num  = (uint8_t)(ep_addr & 0x0FU);
                    uint8_t  is_in   = ((ep_addr & 0x80U) != 0) ? 1U : 0U;
                    uint8_t  dci     = (uint8_t)(ep_num * 2U + is_in);

                    last_ep_dci = 0;
                    if (ep_attr == 3U && is_in && dev->int_in_dci == 0) {
                        uint8_t xhci_iv = 6U;
                        if (speed >= 3U) {
                            /* High-Speed / SuperSpeed: bInterval is 1..16 -> xHCI Interval = bInterval - 1 */
                            xhci_iv = (ep_bInt >= 1U && ep_bInt <= 16U) ? (uint8_t)(ep_bInt - 1U) : 6U;
                        } else if (ep_bInt >= 1U) {
                            /* Full/Low-Speed: bInterval in ms -> xHCI Interval (3..10) = floor(log2(ms * 8)) */
                            uint32_t uf = (uint32_t)ep_bInt * 8U;
                            xhci_iv = 3U;
                            while (xhci_iv < 10U && (1U << (xhci_iv + 1U)) <= uf) {
                                xhci_iv++;
                            }
                        }
                        dev->int_in_dci      = dci;
                        dev->int_in_mps      = (ep_mps > 0) ? ep_mps : 8U;
                        dev->int_in_interval = xhci_iv;
                        last_ep_dci          = dci;
                        if (dci > max_dci) {
                            max_dci = dci;
                        }
                    } else if (ep_attr == 2U &&
                               (dev->dev_type == USB_DEV_STORAGE ||
                                dev->dev_type == USB_DEV_NET_RNDIS ||
                                dev->dev_type == USB_DEV_NET_ECM)) {
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

            if (dev->dev_type != USB_DEV_NONE) {
                break;
            }
        }
    }

    if (dev->dev_type == USB_DEV_NONE) {
        xhci_release_dev(dev);
        return -1;
    }

    /* 5. Send USB SET_CONFIGURATION on EP0 before configuring xHCI endpoints/hub context */
    (void)xhci_control_transfer(dev, 0x00U, 0x09U, dev->config_val, 0, 0, NULL);

    /* For CDC-ECM (or any device using a non-zero alternate data interface), send SET_INTERFACE */
    if ((dev->dev_type == USB_DEV_NET_ECM || dev->dev_type == USB_DEV_NET_RNDIS) &&
        dev->net_data_alt != 0U) {
        (void)xhci_control_transfer(dev, 0x01U, 0x0BU,
                                    dev->net_data_alt, dev->net_data_iface, 0, NULL);
    }

    /* For USB Hubs, read the Hub Descriptor and set Hub Depth now so CONFIGURE_EP gets Number of Ports > 0 */
    {
        uint32_t hub_pwr_delay_ms = 50U;
        if (dev->dev_type == USB_DEV_HUB) {
            hub_pwr_delay_ms = usbhub_read_descriptor(dev, tier);
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
            xhci_setup_ep_ctx(dev->in_ctx + ((uint32_t)(dev->int_in_dci + 1U) * ctx_size),
                              7U, dev->int_in_mps, 0U, dev->int_in_interval,
                              (uint32_t)dev->int_in_mps, &dev->int_in_ring);
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
            xhci_release_dev(dev);
            return -1;
        }

        if (dev->dev_type == USB_DEV_HUB) {
            usbhub_init_device(dev, tier, hub_pwr_delay_ms);
        }
    }

    if (dev->dev_type == USB_DEV_KBD) {
        usbhid_init_kbd(dev);
    } else if (dev->dev_type == USB_DEV_MOUSE) {
        usbhid_init_mouse(dev);
    } else if (dev->dev_type == USB_DEV_STORAGE) {
        usbmsc_init_device(dev);
    } else if (dev->dev_type == USB_DEV_NET_RNDIS || dev->dev_type == USB_DEV_NET_ECM) {
        usbnet_init_device(dev);
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

    sp_arr = (uint32_t *)xhci_dma_alloc(num_sp * 8U, 64U);
    if (sp_arr == NULL) {
        return;
    }

    for (i = 0; i < num_sp; i++) {
        void *page = xhci_dma_alloc(4096U, 4096U);
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
    bool_t   any_connected = FALSE;

    xhci_ready          = FALSE;
    xhci_dma_off        = 0;
    num_usb_devs        = 0;
    memset(usb_devs, 0, sizeof(usb_devs));
    root_port_conn_mask = 0;
    hotplug_pending     = FALSE;

    usbhub_reset();
    usbhid_reset();
    usbmsc_reset();
    usbnet_reset();

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

    dcbaa = (volatile uint32_t *)xhci_dma_alloc(256U * 8U, 64);
    xhci_setup_scratchpad(hcsparams2);
    mmio_write32(op_base, 0x30, (uint32_t)(uintptr_t)dcbaa);
    mmio_write32(op_base, 0x34, 0);

    ring_init(&cmd_ring);
    mmio_write32(op_base, 0x18, ((uint32_t)(uintptr_t)&cmd_ring.trbs[0]) | 1U);
    mmio_write32(op_base, 0x1C, 0);

    evt_ring    = (volatile struct xhci_trb *)xhci_dma_alloc(XHCI_RING_TRBS * sizeof(struct xhci_trb), 64);
    evt_deq_idx = 0;
    evt_ccs     = 1;

    erst = (struct xhci_erst_entry *)xhci_dma_alloc(sizeof(struct xhci_erst_entry), 64);
    erst[0].base_lo  = (uint32_t)(uintptr_t)&evt_ring[0];
    erst[0].base_hi  = 0;
    erst[0].size     = XHCI_RING_TRBS;
    erst[0].reserved = 0;

    mmio_write32(rt_base, 0x20 + 0x08, 1U);
    mmio_write32(rt_base, 0x20 + 0x18, ((uint32_t)(uintptr_t)&evt_ring[0]) | (1U << 3));
    mmio_write32(rt_base, 0x20 + 0x1C, 0);
    mmio_write32(rt_base, 0x20 + 0x10, (uint32_t)(uintptr_t)erst);
    mmio_write32(rt_base, 0x20 + 0x14, 0);

    ctrl_dma_buf = (uint8_t *)xhci_dma_alloc(1024, 64);

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

void xhci_get_controller_info(struct xhci_controller_info *out)
{
    if (out != NULL) {
        *out = ctrl_info;
    }
}
