/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Hardware/OS Control Syscall (OKSINIX_SYS_ctl)
 * Services privileged kernel diagnostics & device control requested by Ring-3 utilities.
 */

#include "syscall.h"
#include "vm.h"
#include "sched.h"
#include "acpi.h"
#include "net.h"
#include "vfs.h"
#include "fat32.h"
#include "xhci.h"
#include "console.h"
#include "ps2.h"
#include "string.h"

#define CTL_EFAULT (-14)
#define CTL_EINVAL (-22)
#define CTL_ENOENT (-2)

static void ctl_puts(const char *s)
{
    (void)vfs_write(1, s, (uint32_t)strlen(s));
}

static void ctl_putc(char c)
{
    (void)vfs_write(1, &c, 1U);
}

static void ctl_print_uint(uint32_t val)
{
    char buf[16];
    int i = 0;
    if (val == 0U) {
        ctl_putc('0');
        return;
    }
    while (val > 0U) {
        buf[i++] = (char)('0' + (val % 10U));
        val /= 10U;
    }
    while (i > 0) {
        ctl_putc(buf[--i]);
    }
}

static void ctl_print_hex32(uint32_t val)
{
    const char *hex = "0123456789ABCDEF";
    int shift;
    ctl_puts("0x");
    for (shift = 28; shift >= 0; shift -= 4) {
        ctl_putc(hex[(val >> shift) & 0x0FU]);
    }
}

static void ctl_print_hex8(uint8_t val)
{
    const char *hex = "0123456789abcdef";
    ctl_putc(hex[(val >> 4) & 0x0FU]);
    ctl_putc(hex[val & 0x0FU]);
}

static void ctl_print_mac(const uint8_t *mac)
{
    int i;
    for (i = 0; i < 6; i++) {
        if (i > 0) {
            ctl_putc(':');
        }
        ctl_print_hex8(mac[i]);
    }
}

static void ctl_print_ipv4(uint32_t ip_be)
{
    uint32_t ip = net_ntohl(ip_be);
    ctl_print_uint((ip >> 24) & 0xFFU);
    ctl_putc('.');
    ctl_print_uint((ip >> 16) & 0xFFU);
    ctl_putc('.');
    ctl_print_uint((ip >> 8) & 0xFFU);
    ctl_putc('.');
    ctl_print_uint(ip & 0xFFU);
}

static uint16_t ctl_checksum16(const uint8_t *data, uint32_t len)
{
    uint32_t sum = 0U;
    uint32_t i;
    for (i = 0U; i + 1U < len; i += 2U) {
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

static void ctl_cmd_net(int argc, char **argv)
{
    struct xhci_net_info ni;

    devfs_sync_usb_storage();
    if (!xhci_has_usb_net()) {
        ctl_puts("No USB Ethernet (RNDIS / CDC-ECM) device detected.\n");
        return;
    }
    xhci_get_net_info(&ni);

    if (argc < 2) {
        ctl_puts("eth0: USB ");
        ctl_puts((ni.protocol == XHCI_NET_PROTO_RNDIS) ? "RNDIS" : "CDC-ECM");
        ctl_puts("  MAC ");
        ctl_print_mac(ni.mac);
        ctl_puts("  MTU ");
        ctl_print_uint(ni.mtu);
        ctl_puts("\n      IP ");
        ctl_print_ipv4(net_get_local_ip());
        ctl_puts("  Mask ");
        ctl_print_ipv4(net_get_netmask());
        ctl_puts("  GW ");
        ctl_print_ipv4(net_get_gateway_ip());
        ctl_puts("  DNS ");
        ctl_print_ipv4(net_get_dns_ip());
        ctl_puts("\n      TX packets=");
        ctl_print_uint(ni.tx_packets);
        ctl_puts(" (");
        ctl_print_uint(ni.tx_bytes);
        ctl_puts(" B)  RX packets=");
        ctl_print_uint(ni.rx_packets);
        ctl_puts(" (");
        ctl_print_uint(ni.rx_bytes);
        ctl_puts(" B)\n");
        return;
    }

    if (strcmp(argv[1], "dhcp") == 0) {
        ctl_puts("DHCP: discovering lease on eth0...\n");
        console_refresh();
        if (net_dhcp() < 0) {
            ctl_puts("DHCP: timeout (keeping current IP config)\n");
        } else {
            ctl_puts("DHCP: leased IP ");
            ctl_print_ipv4(net_get_local_ip());
            ctl_puts("  Mask ");
            ctl_print_ipv4(net_get_netmask());
            ctl_puts("  GW ");
            ctl_print_ipv4(net_get_gateway_ip());
            ctl_puts("  DNS ");
            ctl_print_ipv4(net_get_dns_ip());
            ctl_putc('\n');
        }
        return;
    }

    if (strcmp(argv[1], "config") == 0) {
        uint32_t ip, mask, gw, dns = 0UL;
        if (argc < 5 ||
            net_parse_ipv4(argv[2], &ip) < 0 ||
            net_parse_ipv4(argv[3], &mask) < 0 ||
            net_parse_ipv4(argv[4], &gw) < 0 ||
            (argc >= 6 && net_parse_ipv4(argv[5], &dns) < 0)) {
            ctl_puts("usage: net config <ip> <mask> <gw> [dns]\n");
            return;
        }
        net_set_config(ip, mask, gw, dns);
        ctl_puts("Configured eth0 IP ");
        ctl_print_ipv4(net_get_local_ip());
        ctl_puts(" GW ");
        ctl_print_ipv4(net_get_gateway_ip());
        ctl_putc('\n');
        return;
    }

    if (strcmp(argv[1], "ping") == 0) {
        uint32_t target_ip = net_get_gateway_ip();
        uint32_t next_hop;
        uint8_t hop_mac[6];
        struct sockaddr_in dst;
        uint8_t icmp_tx[8];
        uint8_t icmp_rx[64];
        uint16_t ic_ck;
        int sock;
        int32_t n;

        if (argc >= 3 && net_resolve_host(argv[2], &target_ip) < 0) {
            ctl_puts("net ping: cannot resolve ");
            ctl_puts(argv[2]);
            ctl_putc('\n');
            return;
        }

        if (((uint8_t)(target_ip & 0xFFU)) != 127U && target_ip != net_get_local_ip()) {
            if ((target_ip & net_get_netmask()) == (net_get_local_ip() & net_get_netmask())) {
                next_hop = target_ip;
            } else {
                next_hop = net_get_gateway_ip();
            }
            if (net_arp_resolve(next_hop, hop_mac) < 0) {
                ctl_puts("net ping: ARP timeout for ");
                ctl_print_ipv4(next_hop);
                ctl_putc('\n');
                return;
            }
            ctl_puts("ARP reply: ");
            ctl_print_ipv4(next_hop);
            ctl_puts(" is at ");
            ctl_print_mac(hop_mac);
            ctl_putc('\n');
        }

        sock = net_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
        if (sock < 0) {
            ctl_puts("net ping: socket() failed\n");
            return;
        }

        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_addr   = target_ip;
        if (net_connect(sock, (const struct sockaddr *)(const void *)&dst, (uint32_t)sizeof(dst)) < 0) {
            (void)net_close(sock);
            ctl_puts("net ping: connect() failed\n");
            return;
        }

        memset(icmp_tx, 0, sizeof(icmp_tx));
        icmp_tx[0] = 8U;
        icmp_tx[4] = 0x12U; icmp_tx[5] = 0x34U;
        icmp_tx[6] = 0x00U; icmp_tx[7] = 0x01U;
        ic_ck = ctl_checksum16(icmp_tx, 8U);
        icmp_tx[2] = (uint8_t)(ic_ck >> 8);
        icmp_tx[3] = (uint8_t)(ic_ck & 0xFFU);

        if (net_send(sock, icmp_tx, 8U, 0U) < 0) {
            (void)net_close(sock);
            ctl_puts("net ping: send() failed\n");
            return;
        }

        n = net_recv(sock, icmp_rx, (uint32_t)sizeof(icmp_rx), 0U);
        (void)net_close(sock);

        if (n >= 8 && icmp_rx[0] == 0U && icmp_rx[1] == 0U &&
            icmp_rx[4] == 0x12U && icmp_rx[5] == 0x34U &&
            icmp_rx[6] == 0x00U && icmp_rx[7] == 0x01U &&
            ctl_checksum16(icmp_rx, (uint32_t)n) == 0U) {
            ctl_puts("ICMP echo reply from ");
            ctl_print_ipv4(target_ip);
            ctl_puts(": seq=1 OK\n");
        } else {
            ctl_puts("net ping: ICMP timeout from ");
            ctl_print_ipv4(target_ip);
            ctl_putc('\n');
        }
        return;
    }

    ctl_puts("usage: net [dhcp | config <ip> <mask> <gw> [dns] | ping [host]]\n");
}

static void ctl_cmd_lsusb(void)
{
    if (!xhci_is_initialized()) {
        ctl_puts("No USB host controller found.\n");
        return;
    }
    {
        struct xhci_controller_info ci;
        struct xhci_hub_info hub;
        const char *hci_name = "xHCI USB 3.0";
        int i;
        devfs_sync_usb_storage();
        xhci_get_controller_info(&ci);
        xhci_get_hub_info(&hub);
        if (ci.hci_type == USB_HCI_EHCI) {
            hci_name = "EHCI USB 2.0";
        } else if (ci.hci_type == USB_HCI_OHCI) {
            hci_name = "OHCI USB 1.1";
        } else if (ci.hci_type == USB_HCI_UHCI) {
            hci_name = "UHCI USB 1.1";
        }
        ctl_puts("Bus 001: ");
        ctl_puts(hci_name);
        ctl_puts(" Host Controller (ports=");
        ctl_print_uint(ci.max_ports);
        ctl_puts(", devices=");
        ctl_print_uint(ci.num_devices);
        ctl_puts(")\n");
        if (hub.present) {
            ctl_puts("  - USB Hub (slot ");
            ctl_print_uint(hub.slot_id);
            ctl_puts(", ");
            ctl_print_uint(hub.num_ports);
            ctl_puts(" ports, ");
            ctl_print_uint(hub.downstream_devices);
            ctl_puts(" downstream)\n");
        }
        if (xhci_has_usb_kbd()) {
            ctl_puts("  - USB HID Boot Keyboard (/dev/kbd)\n");
        }
        if (xhci_has_usb_mouse()) {
            ctl_puts("  - USB HID Boot Mouse (/dev/mouse)\n");
        }
        if (xhci_has_usb_net()) {
            struct xhci_net_info ni;
            xhci_get_net_info(&ni);
            ctl_puts("  - USB Ethernet (");
            ctl_puts((ni.protocol == XHCI_NET_PROTO_RNDIS) ? "RNDIS" : "CDC-ECM");
            ctl_puts("): MAC ");
            ctl_print_mac(ni.mac);
            ctl_puts(" -> /dev/eth0\n");
        }
        for (i = 0; i < XHCI_MAX_MSC_DEVS; i++) {
            struct xhci_msc_info msc;
            if (xhci_get_msc_info_idx(i, &msc) == 0 && msc.present) {
                char sd_name[12];
                memcpy(sd_name, "/dev/sda", 9);
                sd_name[7] = (char)('a' + i);
                ctl_puts("  - USB Mass Storage (BOT/SCSI): ");
                ctl_puts(msc.vendor);
                ctl_putc(' ');
                ctl_puts(msc.product);
                ctl_puts(" -> ");
                ctl_puts(sd_name);
                ctl_puts(" (");
                ctl_print_uint(msc.block_count * msc.block_size);
                ctl_puts(" B)\n");
            }
        }
    }
}

static void ctl_cmd_memmap(void)
{
    int i;
    int tcnt = acpi_get_table_count();
    int mcnt = acpi_get_mmap_count();

    if (acpi_has_rsdp()) {
        ctl_puts("ACPI RSDP: ");
        ctl_print_hex32(acpi_get_rsdp_addr());
        ctl_puts(" RSDT: ");
        ctl_print_hex32(acpi_get_rsdt_addr());
        ctl_puts(" Tables:");
        for (i = 0; i < tcnt; i++) {
            struct acpi_table_desc td;
            if (acpi_get_table_desc(i, &td) == 0) {
                ctl_putc(' ');
                ctl_puts(td.signature);
            }
        }
        ctl_putc('\n');
    }

    for (i = 0; i < mcnt; i++) {
        struct acpi_mmap_entry me;
        if (acpi_get_mmap_entry(i, &me) == 0) {
            uint32_t end = (me.length_lo > 0) ? (me.base_lo + me.length_lo - 1U) : me.base_lo;
            ctl_puts("  ");
            ctl_print_hex32(me.base_lo);
            ctl_puts(" - ");
            ctl_print_hex32(end);
            ctl_puts(" (");
            ctl_print_uint(me.length_lo / 1024U);
            ctl_puts(" KiB) [");
            ctl_puts(acpi_mmap_type_name(me.type));
            ctl_puts("]\n");
        }
    }
    ctl_puts("Total Usable RAM: ");
    ctl_print_uint(acpi_get_total_usable_ram() / (1024U * 1024U));
    ctl_puts(" MiB\n");
}

static void ctl_cmd_keymap(const char *arg)
{
    if (arg == NULL || arg[0] == '\0') {
        ctl_puts("Current keymap: ");
        ctl_puts(kbd_get_keymap_name());
        if (kbd_get_keymap() == KBD_KEYMAP_FR_CH) {
            ctl_puts(" (Swiss French QWERTZ)\n");
        } else {
            ctl_puts(" (US QWERTY)\n");
        }
    } else if (strcmp(arg, "fr_CH") == 0 || strcmp(arg, "ch_fr") == 0 ||
               strcmp(arg, "ch") == 0 || strcmp(arg, "swiss") == 0) {
        int fd;
        kbd_set_keymap(KBD_KEYMAP_FR_CH);
        fd = vfs_open("/etc/keymap", VFS_O_WRONLY | VFS_O_TRUNC);
        if (fd >= 0) {
            (void)vfs_write(fd, "fr_CH\n", 6);
            (void)vfs_close(fd);
        }
        ctl_puts("Keymap switched to fr_CH (Swiss French QWERTZ)\n");
    } else if (strcmp(arg, "us") == 0) {
        int fd;
        kbd_set_keymap(KBD_KEYMAP_US);
        fd = vfs_open("/etc/keymap", VFS_O_WRONLY | VFS_O_TRUNC);
        if (fd >= 0) {
            (void)vfs_write(fd, "us\n", 3);
            (void)vfs_close(fd);
        }
        ctl_puts("Keymap switched to us (US QWERTY)\n");
    } else {
        ctl_puts("usage: keymap [fr_CH|us]\n");
    }
}

static void ctl_cmd_mount_list(void)
{
    int m;
    devfs_sync_usb_storage();
    ctl_puts("rootfs on / type ramfs (rw)\n");
    ctl_puts("devfs on /dev type devfs (rw)\n");
    for (m = 0; m < FAT32_MAX_MOUNTS; m++) {
        struct fat32_fs_info fi;
        if (fat32_get_info_idx(m, &fi) == 0 && fi.mounted) {
            char dev_part[16];
            memcpy(dev_part, "/dev/sda1", 10);
            dev_part[7] = (char)('a' + fi.disk_idx);
            ctl_puts(dev_part);
            ctl_puts(" on ");
            ctl_puts(fi.mount_point);
            ctl_puts(" type fat32 (rw, label=");
            ctl_puts(fi.volume_label);
            ctl_puts(", lba=");
            ctl_print_uint(fi.part_lba);
            ctl_puts(", clusters=");
            ctl_print_uint(fi.total_clusters);
            ctl_puts(")\n");
        }
    }
}

static void ctl_cmd_ps(void)
{
    int slot;
    struct sched_task_info ti;

    ctl_puts("  PID RING STAT   TICKS    SW  CMD\n");
    for (slot = 0; slot < SCHED_MAX_TASKS; slot++) {
        const char *st = "?";
        if (sched_get_task_info(slot, &ti) != 0) {
            continue;
        }
        if (ti.state == TASK_STATE_RUNNING) {
            st = "RUN";
        } else if (ti.state == TASK_STATE_READY) {
            st = "RDY";
        } else if (ti.state == TASK_STATE_SLEEPING) {
            st = "SLP";
        } else if (ti.state == TASK_STATE_ZOMBIE) {
            st = "ZOM";
        }
        ctl_puts("    ");
        ctl_print_uint((uint32_t)ti.pid);
        ctl_puts(ti.is_user ? "   R3  " : "   R0  ");
        ctl_puts(st);
        ctl_puts("    ");
        ctl_print_uint(ti.ticks);
        ctl_puts("    ");
        ctl_print_uint(ti.switches);
        ctl_puts("  ");
        ctl_puts(ti.name);
        ctl_putc('\n');
    }
    ctl_puts("Total context switches: ");
    ctl_print_uint(sched_get_total_switches());
    ctl_putc('\n');
}

static volatile uint32_t ctl_bg_count = 0U;

static void ctl_bg_counter_task(void *arg)
{
    volatile uint32_t *cnt = (volatile uint32_t *)arg;
    for (;;) {
        (*cnt)++;
    }
}

static void ctl_bg_sleeper_task(void *arg)
{
    volatile uint32_t *cnt = (volatile uint32_t *)arg;
    for (;;) {
        (*cnt)++;
        sched_sleep_ms(50U);
    }
}

int32_t oksinix_ctl_dispatch(uint32_t *pgdir, const char *cwd, uint32_t op,
                             uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)cwd;

    switch (op) {
    case OKS_CTL_SYNC_USB:
        if (xhci_is_initialized()) {
            xhci_poll();
            devfs_sync_usb_storage();
        }
        return 0;

    case OKS_CTL_STATUS_BAR:
        if (a1 == 0U || !vm_user_str_ok(pgdir, a1, 64U)) {
            return CTL_EFAULT;
        }
        console_set_status((const char *)(uintptr_t)a1, 0);
        return 0;

    case OKS_CTL_LSUSB:
        ctl_cmd_lsusb();
        return 0;

    case OKS_CTL_MEMMAP:
        ctl_cmd_memmap();
        return 0;

    case OKS_CTL_NET: {
        int argc = (int)a1;
        char *const *uargv = (char *const *)(uintptr_t)a2;
        char *kargv[8];
        int i;
        if (argc < 1 || argc > 8 || uargv == NULL ||
            !vm_user_range_ok(pgdir, a2, (uint32_t)argc * 4U, FALSE)) {
            return CTL_EFAULT;
        }
        for (i = 0; i < argc; i++) {
            if (uargv[i] == NULL || !vm_user_str_ok(pgdir, (uint32_t)(uintptr_t)uargv[i], 64U)) {
                return CTL_EFAULT;
            }
            kargv[i] = uargv[i];
        }
        ctl_cmd_net(argc, kargv);
        return 0;
    }

    case OKS_CTL_KEYMAP:
        if (a1 != 0U && !vm_user_str_ok(pgdir, a1, 32U)) {
            return CTL_EFAULT;
        }
        ctl_cmd_keymap((a1 != 0U) ? (const char *)(uintptr_t)a1 : NULL);
        return 0;

    case OKS_CTL_MOUNT_LIST:
        ctl_cmd_mount_list();
        return 0;

    case OKS_CTL_MOUNT_ALL:
        devfs_sync_usb_storage();
        fat32_remount_all();
        return 0;

    case OKS_CTL_PS:
        ctl_cmd_ps();
        return 0;

    case OKS_CTL_SPAWN:
        if (a1 == 1U) {
            return sched_spawn_ex("bg_sleeper", ctl_bg_sleeper_task, (void *)&ctl_bg_count, TRUE);
        }
        return sched_spawn_ex("bg_counter", ctl_bg_counter_task, (void *)&ctl_bg_count, TRUE);

    case OKS_CTL_SCHEDTEST:
        return sched_run_selftest();

    case OKS_CTL_PROTTEST:
        return sched_run_prot_selftest();

    case OKS_CTL_READDIR: {
        struct vfs_dirent de;
        struct oks_dirent_user *out = (struct oks_dirent_user *)(uintptr_t)a3;
        if (a1 == 0U || a3 == 0U ||
            !vm_user_str_ok(pgdir, a1, VFS_MAX_PATH) ||
            !vm_user_range_ok(pgdir, a3, (uint32_t)sizeof(*out), TRUE)) {
            return CTL_EFAULT;
        }
        if (vfs_readdir((const char *)(uintptr_t)a1, a2, &de) != 0) {
            return CTL_ENOENT;
        }
        memcpy(out->name, de.name, sizeof(out->name));
        out->inode      = de.inode;
        out->type       = de.type;
        out->size       = de.size;
        out->rdev_major = de.rdev_major;
        out->rdev_minor = de.rdev_minor;
        return 0;
    }

    case OKS_CTL_STAT: {
        struct vfs_dirent de;
        struct oks_dirent_user *out = (struct oks_dirent_user *)(uintptr_t)a2;
        if (a1 == 0U || a2 == 0U ||
            !vm_user_str_ok(pgdir, a1, VFS_MAX_PATH) ||
            !vm_user_range_ok(pgdir, a2, (uint32_t)sizeof(*out), TRUE)) {
            return CTL_EFAULT;
        }
        if (vfs_stat((const char *)(uintptr_t)a1, &de) < 0) {
            return CTL_ENOENT;
        }
        memcpy(out->name, de.name, sizeof(out->name));
        out->inode      = de.inode;
        out->type       = de.type;
        out->size       = de.size;
        out->rdev_major = de.rdev_major;
        out->rdev_minor = de.rdev_minor;
        return 0;
    }

    default:
        return CTL_EINVAL;
    }
}
