// For SATA interfaces only
// https://wiki.osdev.org/SATA
// https://wiki.osdev.org/ATA_PIO_Mode
// https://wiki.osdev.org/MBR_(x86)
//
// Reading and writing 512 byte sectors with ATA PIO (28 bit LBA), and finding the FAT volume
// on a drive (either a partition in the MBR, or a whole drive formatted without a partition table).

#ifndef SATA_FS_PARSE_H
#define SATA_FS_PARSE_H

#include <stdint.h>
#include <stdbool.h>
#include "../../input/GIO.h"
#include "SATA_scanning.h"

#define SECTOR_SIZE        512
#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30
#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_LBA28_MAX      0x0FFFFFFF

// select the drive and send a read / write command for count (1-255) sectors
bool ata_pio_command (uint16_t io, uint16_t ctrl, int slave, uint32_t lba, uint8_t count, uint8_t command) {
    if (!ata_wait_not_busy(io)) {
        return false;
    }
    outb(ctrl, 0x02); // no interrupts, the driver polls
    outb(io + ATA_REG_DRIVE, (uint8_t)(0xE0 | (slave ? 0x10 : 0x00) | ((lba >> 24) & 0x0F)));
    ata_delay(ctrl);
    outb(io + ATA_REG_ERROR, 0);
    outb(io + ATA_REG_SECCOUNT, count);
    outb(io + ATA_REG_LBA_LO, (uint8_t)(lba & 0xFF));
    outb(io + ATA_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(io + ATA_REG_LBA_HI, (uint8_t)((lba >> 16) & 0xFF));
    outb(io + ATA_REG_COMMAND, command);
    return true;
}

// wait until the drive is ready to transfer the next sector
bool ata_pio_ready (uint16_t io, uint16_t ctrl) {
    ata_delay(ctrl);
    if (!ata_wait_not_busy(io)) {
        return false;
    }
    uint8_t status = inb(io + ATA_REG_STATUS);
    if (status & (ATA_SR_ERR | ATA_SR_DF)) {
        return false;
    }
    if (!(status & ATA_SR_DRQ)) {
        status = ata_wait_data(io);
        if (status == 0xFF || (status & (ATA_SR_ERR | ATA_SR_DF))) {
            return false;
        }
    }
    return true;
}

// read count sectors starting at lba into buffer (count * 512 bytes)
bool drive_read (int bus, int slave, uint32_t lba, uint32_t count, void* buffer) {
    uint16_t io = bus ? ATA_SECONDARY_IO : ATA_PRIMARY_IO;
    uint16_t ctrl = bus ? ATA_SECONDARY_CTRL : ATA_PRIMARY_CTRL;
    uint16_t* words = (uint16_t*)buffer;

    if (count == 0 || lba > ATA_LBA28_MAX || count > ATA_LBA28_MAX - lba + 1) {
        return false;
    }
    while (count > 0) {
        uint8_t n = (count > 255) ? 255 : (uint8_t)count;
        if (!ata_pio_command(io, ctrl, slave, lba, n, ATA_CMD_READ_PIO)) {
            return false;
        }
        for (uint8_t s = 0; s < n; s++) {
            if (!ata_pio_ready(io, ctrl)) {
                return false;
            }
            for (int i = 0; i < 256; i++) {
                *words++ = inw(io + ATA_REG_DATA);
            }
        }
        lba += n;
        count -= n;
    }
    return true;
}

// write count sectors starting at lba from buffer (count * 512 bytes)
bool drive_write (int bus, int slave, uint32_t lba, uint32_t count, const void* buffer) {
    uint16_t io = bus ? ATA_SECONDARY_IO : ATA_PRIMARY_IO;
    uint16_t ctrl = bus ? ATA_SECONDARY_CTRL : ATA_PRIMARY_CTRL;
    const uint16_t* words = (const uint16_t*)buffer;

    if (count == 0 || lba > ATA_LBA28_MAX || count > ATA_LBA28_MAX - lba + 1) {
        return false;
    }
    while (count > 0) {
        uint8_t n = (count > 255) ? 255 : (uint8_t)count;
        if (!ata_pio_command(io, ctrl, slave, lba, n, ATA_CMD_WRITE_PIO)) {
            return false;
        }
        for (uint8_t s = 0; s < n; s++) {
            if (!ata_pio_ready(io, ctrl)) {
                return false;
            }
            for (int i = 0; i < 256; i++) {
                outw(io + ATA_REG_DATA, *words++);
            }
        }
        lba += n;
        count -= n;
    }

    // make sure the data has left the drive's cache before we report success
    outb(io + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_delay(ctrl);
    if (!ata_wait_not_busy(io)) {
        return false;
    }
    return (inb(io + ATA_REG_STATUS) & (ATA_SR_ERR | ATA_SR_DF)) == 0;
}

// does this sector look like a FAT boot sector?
bool drive_is_fat_boot_sector (const uint8_t* sector) {
    uint16_t bytes_per_sector = (uint16_t)(sector[11] | (sector[12] << 8));
    uint8_t sectors_per_cluster = sector[13];
    uint16_t reserved = (uint16_t)(sector[14] | (sector[15] << 8));
    uint8_t fats = sector[16];

    if (sector[0] != 0xEB && sector[0] != 0xE9) {
        return false; // boot sectors start with a jump instruction
    }
    if (bytes_per_sector != 512 || sectors_per_cluster == 0 ||
        (sectors_per_cluster & (sectors_per_cluster - 1)) != 0 || reserved == 0 || fats == 0 || fats > 4) {
        return false;
    }
    return true;
}

// MBR partition types that hold a FAT file system
bool drive_is_fat_partition (uint8_t type) {
    return type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0B || type == 0x0C || type == 0x0E;
}

// find the FAT volume on a drive: 1 = found (volume_lba = its first sector), 0 = none, -1 = read error
int drive_parse (int bus, int slave, uint32_t* volume_lba) {
    uint8_t sector[SECTOR_SIZE];

    if (!drive_read(bus, slave, 0, 1, sector)) {
        return -1;
    }
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        return 0; // blank or unknown
    }

    // a drive formatted without a partition table ("superfloppy")
    if (drive_is_fat_boot_sector(sector)) {
        *volume_lba = 0;
        return 1;
    }

    // MBR: four 16 byte partition entries at offset 446 (GPT disks show a single 0xEE entry and are skipped)
    for (int i = 0; i < 4; i++) {
        const uint8_t* entry = sector + 446 + i * 16;
        uint32_t start = (uint32_t)entry[8] | ((uint32_t)entry[9] << 8) | ((uint32_t)entry[10] << 16) | ((uint32_t)entry[11] << 24);
        if (drive_is_fat_partition(entry[4]) && start != 0) {
            *volume_lba = start;
            return 1;
        }
    }
    return 0;
}

#endif /* SATA_FS_PARSE_H */
