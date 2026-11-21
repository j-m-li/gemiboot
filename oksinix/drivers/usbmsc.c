/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - USB Mass Storage (BOT/SCSI) Driver
 */

#include "xhci_internal.h"
#include "pit.h"
#include "serial.h"
#include "string.h"

static uint8_t                 *msc_cbw_buf  = NULL;
static uint8_t                 *msc_csw_buf  = NULL;
static uint8_t                 *msc_data_buf = NULL;
static uint32_t                 msc_cbw_tag  = 1;

static struct xhci_usb_dev     *msc_devs[XHCI_MAX_MSC_DEVS];
static struct xhci_msc_info     msc_infos[XHCI_MAX_MSC_DEVS];
static int                      num_msc_devs = 0;

void usbmsc_reset(void)
{
    int i;

    msc_cbw_buf  = NULL;
    msc_csw_buf  = NULL;
    msc_data_buf = NULL;
    msc_cbw_tag  = 1;
    num_msc_devs = 0;
    for (i = 0; i < XHCI_MAX_MSC_DEVS; i++) {
        msc_devs[i] = NULL;
    }
    memset(msc_infos, 0, sizeof(msc_infos));
}

static void usbmsc_bot_reset(struct xhci_usb_dev *dev)
{
    /* Bulk-Only Mass Storage Reset (class-specific request 0xFF) */
    (void)xhci_control_transfer(dev, 0x21U, 0xFFU, 0, dev->iface_num, 0, NULL);
    xhci_clear_bulk_stall(dev, TRUE);
    xhci_clear_bulk_stall(dev, FALSE);
    pit_mdelay(10U);
}

static int usbmsc_bot_cmd(struct xhci_usb_dev *dev,
                          const uint8_t *cdb, uint8_t cdb_len,
                          bool_t dir_in, void *data_buf, uint32_t data_len)
{
    uint32_t tag = msc_cbw_tag++;
    int data_rc = 0;
    int csw_rc;

    memset(msc_cbw_buf, 0, 31);
    msc_cbw_buf[0]  = 0x55;
    msc_cbw_buf[1]  = 0x53;
    msc_cbw_buf[2]  = 0x42;
    msc_cbw_buf[3]  = 0x43;
    msc_cbw_buf[4]  = (uint8_t)(tag & 0xFFU);
    msc_cbw_buf[5]  = (uint8_t)((tag >> 8) & 0xFFU);
    msc_cbw_buf[6]  = (uint8_t)((tag >> 16) & 0xFFU);
    msc_cbw_buf[7]  = (uint8_t)((tag >> 24) & 0xFFU);
    msc_cbw_buf[8]  = (uint8_t)(data_len & 0xFFU);
    msc_cbw_buf[9]  = (uint8_t)((data_len >> 8) & 0xFFU);
    msc_cbw_buf[10] = (uint8_t)((data_len >> 16) & 0xFFU);
    msc_cbw_buf[11] = (uint8_t)((data_len >> 24) & 0xFFU);
    msc_cbw_buf[12] = dir_in ? 0x80U : 0x00U;
    msc_cbw_buf[13] = 0;
    msc_cbw_buf[14] = cdb_len;
    memcpy(&msc_cbw_buf[15], cdb, cdb_len);

    if (xhci_bulk_transfer(dev, FALSE, msc_cbw_buf, 31) < 0) {
        usbmsc_bot_reset(dev);
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
            usbmsc_bot_reset(dev);
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
        usbmsc_bot_reset(dev);
        return -1;
    }

    if (msc_csw_buf[0] != 0x55 || msc_csw_buf[1] != 0x53 ||
        msc_csw_buf[2] != 0x42 || msc_csw_buf[3] != 0x53) {
        usbmsc_bot_reset(dev);
        return -1;
    }
    if (msc_csw_buf[12] == 2U) {
        usbmsc_bot_reset(dev);
        return -1;
    }
    if (data_rc < 0 || msc_csw_buf[12] != 0) {
        return -1;
    }
    return 0;
}

void usbmsc_init_device(struct xhci_usb_dev *dev)
{
    uint8_t cdb[16];
    uint8_t inq[36];
    uint8_t sense[18];
    uint8_t cap[8];
    uint8_t max_lun = 0;
    struct xhci_msc_info info;
    int retry;
    int disk_idx = -1;
    int i;

    kprintf("[XHCI] USB Mass Storage initialized (slot %u, root_port %u, route 0x%x, IN=%u OUT=%u)\n",
            (uint32_t)dev->slot_id, (uint32_t)dev->root_port, dev->route_string,
            (uint32_t)dev->bulk_in_dci, (uint32_t)dev->bulk_out_dci);

    for (i = 0; i < XHCI_MAX_MSC_DEVS; i++) {
        if (!msc_infos[i].present) {
            disk_idx = i;
            break;
        }
    }
    if (disk_idx < 0) {
        return;
    }
    if (msc_cbw_buf == NULL) {
        msc_cbw_buf  = (uint8_t *)xhci_dma_alloc(64, 64);
        msc_csw_buf  = (uint8_t *)xhci_dma_alloc(64, 64);
        msc_data_buf = (uint8_t *)xhci_dma_alloc(4096, 64);
    }
    if (msc_cbw_buf == NULL || msc_csw_buf == NULL || msc_data_buf == NULL) {
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
        if (usbmsc_bot_cmd(dev, cdb, 6, TRUE, inq, 36) == 0) {
            memcpy(info.vendor, &inq[8], 8);
            info.vendor[8] = '\0';
            memcpy(info.product, &inq[16], 16);
            info.product[16] = '\0';
            break;
        }
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x03;
        cdb[4] = 18;
        (void)usbmsc_bot_cmd(dev, cdb, 6, TRUE, sense, 18);
        pit_mdelay(10U);
    }

    for (retry = 0; retry < 30; retry++) {
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x00;
        if (usbmsc_bot_cmd(dev, cdb, 6, FALSE, NULL, 0) == 0) {
            break;
        }
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x03;
        cdb[4] = 18;
        (void)usbmsc_bot_cmd(dev, cdb, 6, TRUE, sense, 18);
        pit_mdelay(20U);
    }

    for (retry = 0; retry < 5; retry++) {
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x25;
        if (usbmsc_bot_cmd(dev, cdb, 10, TRUE, cap, 8) == 0) {
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

            msc_devs[disk_idx]  = dev;
            msc_infos[disk_idx] = info;
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
        (void)usbmsc_bot_cmd(dev, cdb, 6, TRUE, sense, 18);
        pit_mdelay(20U);
    }
}

void usbmsc_remove_device(struct xhci_usb_dev *dev)
{
    int i;

    for (i = 0; i < XHCI_MAX_MSC_DEVS; i++) {
        if (msc_devs[i] == dev && msc_infos[i].present) {
            kprintf("[XHCI-MSC] USB Mass Storage disconnected slot=%u (/dev/sd%c)\n",
                    (uint32_t)dev->slot_id, (char)('a' + i));
            msc_devs[i] = NULL;
            memset(&msc_infos[i], 0, sizeof(msc_infos[i]));
            if (num_msc_devs > 0) {
                num_msc_devs--;
            }
            break;
        }
    }
}

int32_t xhci_msc_read_sector_idx(int disk_idx, uint32_t lba, uint8_t *buf512)
{
    uint8_t cdb[16];
    uint8_t sense[18];
    struct xhci_usb_dev *dev;
    int retry;

    if (disk_idx < 0 || disk_idx >= XHCI_MAX_MSC_DEVS || !msc_infos[disk_idx].present || buf512 == NULL) {
        return -1;
    }
    if (lba >= msc_infos[disk_idx].block_count) {
        return 0;
    }
    dev = msc_devs[disk_idx];
    if (dev == NULL) {
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

        if (usbmsc_bot_cmd(dev, cdb, 10, TRUE, buf512, 512) == 0) {
            return 512;
        }
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x03;
        cdb[4] = 18;
        (void)usbmsc_bot_cmd(dev, cdb, 6, TRUE, sense, 18);
        pit_mdelay(5U);
    }
    return -1;
}

int32_t xhci_msc_write_sector_idx(int disk_idx, uint32_t lba, const uint8_t *buf512)
{
    uint8_t cdb[16];
    uint8_t sense[18];
    uint8_t tmp[512];
    struct xhci_usb_dev *dev;
    int retry;

    if (disk_idx < 0 || disk_idx >= XHCI_MAX_MSC_DEVS || !msc_infos[disk_idx].present || buf512 == NULL) {
        return -1;
    }
    if (lba >= msc_infos[disk_idx].block_count) {
        return 0;
    }
    dev = msc_devs[disk_idx];
    if (dev == NULL) {
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

        if (usbmsc_bot_cmd(dev, cdb, 10, FALSE, tmp, 512) == 0) {
            return 512;
        }
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x03;
        cdb[4] = 18;
        (void)usbmsc_bot_cmd(dev, cdb, 6, TRUE, sense, 18);
        pit_mdelay(5U);
    }
    return -1;
}

int32_t xhci_msc_read_bytes_idx(int disk_idx, uint32_t offset, uint32_t size, uint8_t *buf)
{
    uint8_t sector_buf[512];
    uint32_t total_disk_bytes;
    uint32_t bytes_read = 0;

    if (disk_idx < 0 || disk_idx >= XHCI_MAX_MSC_DEVS || !msc_infos[disk_idx].present) {
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

    if (disk_idx < 0 || disk_idx >= XHCI_MAX_MSC_DEVS || !msc_infos[disk_idx].present) {
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
    if (disk_idx < 0 || disk_idx >= XHCI_MAX_MSC_DEVS || !msc_infos[disk_idx].present) {
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

void xhci_get_msc_info(struct xhci_msc_info *out)
{
    (void)xhci_get_msc_info_idx(0, out);
}
