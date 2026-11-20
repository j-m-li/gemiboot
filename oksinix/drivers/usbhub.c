/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - USB Hub Class Driver (USB 2.0 & 3.0 SuperSpeed)
 */

#include "xhci_internal.h"
#include "pit.h"
#include "serial.h"
#include "string.h"

/* USB Hub Class Feature Selectors */
#define HUB_FEAT_PORT_RESET        4U
#define HUB_FEAT_PORT_POWER        8U
#define HUB_FEAT_C_PORT_CONNECTION 16U
#define HUB_FEAT_C_PORT_RESET      20U
#define HUB_FEAT_C_PORT_LINK_STATE 25U
#define HUB_FEAT_BH_PORT_RESET     28U
#define HUB_FEAT_C_BH_PORT_RESET   29U

static struct xhci_hub_info  hub_info;
static struct xhci_usb_dev  *hub_devs[XHCI_MAX_SLOTS];
static int                   num_hub_devs = 0;

void usbhub_reset(void)
{
    int i;

    num_hub_devs = 0;
    for (i = 0; i < XHCI_MAX_SLOTS; i++) {
        hub_devs[i] = NULL;
    }
    memset(&hub_info, 0, sizeof(hub_info));
}

uint32_t usbhub_read_descriptor(struct xhci_usb_dev *hub_dev, uint32_t tier)
{
    uint8_t hub_desc[16];
    uint8_t num_ports = 0;
    uint32_t hub_pwr_delay_ms = 50U;

    memset(hub_desc, 0, sizeof(hub_desc));
    if (hub_dev->speed >= 4U) {
        /* SuperSpeed Hub Descriptor (type 0x2A, 12 bytes) */
        if (xhci_control_transfer(hub_dev, 0xA0U, 0x06U, 0x2A00U, 0, 12U, hub_desc) < 0 ||
            hub_desc[2] == 0) {
            (void)xhci_control_transfer(hub_dev, 0xA0U, 0x06U, 0x2900U, 0, 9U, hub_desc);
        }
        /* USB 3.0 Spec 10.14.2.10: Set Hub Depth (bRequest = 0x0C) so SuperSpeed hub routes packets */
        (void)xhci_control_transfer(hub_dev, 0x20U, 0x0CU, (uint16_t)tier, 0, 0, NULL);
    } else {
        /* USB 2.0 Hub Descriptor (type 0x29, 9 bytes) */
        if (xhci_control_transfer(hub_dev, 0xA0U, 0x06U, 0x2900U, 0, 9U, hub_desc) < 0 ||
            hub_desc[2] == 0) {
            (void)xhci_control_transfer(hub_dev, 0xA0U, 0x06U, 0x2A00U, 0, 12U, hub_desc);
        }
    }

    num_ports = hub_desc[2];
    if (num_ports == 0 || num_ports > 15U) {
        num_ports = 4U;
    }
    hub_dev->hub_ports = num_ports;
    hub_dev->hub_tier  = (uint8_t)tier;
    hub_dev->hub_ttt   = (uint8_t)((hub_desc[3] >> 5) & 0x03U);
    hub_pwr_delay_ms   = (uint32_t)hub_desc[5] * 2U;
    if (hub_pwr_delay_ms < 50U) {
        hub_pwr_delay_ms = 50U;
    }

    return hub_pwr_delay_ms;
}

static int usbhub_enumerate_port(struct xhci_usb_dev *hub_dev, uint8_t hp)
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
void usbhub_init_device(struct xhci_usb_dev *hub_dev, uint32_t tier, uint32_t pwr_delay_ms)
{
    uint8_t num_ports = hub_dev->hub_ports;
    uint8_t hp;
    uint8_t downstream_cnt = 0;
    int wait_step;
    bool_t any_conn = FALSE;

    if (tier >= 5U || num_ports == 0) {
        return;
    }

    if (num_hub_devs < XHCI_MAX_SLOTS) {
        hub_devs[num_hub_devs++] = hub_dev;
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
        if (usbhub_enumerate_port(hub_dev, hp) == 0) {
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

void usbhub_poll_ports(void)
{
    int d;

    for (d = 0; d < num_hub_devs && xhci_has_free_slot(); d++) {
        struct xhci_usb_dev *hdev = hub_devs[d];
        uint8_t hp;

        if (hdev == NULL || hdev->hub_ports == 0) {
            continue;
        }
        for (hp = 1; hp <= hdev->hub_ports && xhci_has_free_slot(); hp++) {
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
                if (usbhub_enumerate_port(hdev, hp) == 0) {
                    hub_info.downstream_devices++;
                }
            }
        }
    }
}

bool_t xhci_has_usb_hub(void)
{
    return hub_info.present;
}

void xhci_get_hub_info(struct xhci_hub_info *out)
{
    if (out != NULL) {
        *out = hub_info;
    }
}
