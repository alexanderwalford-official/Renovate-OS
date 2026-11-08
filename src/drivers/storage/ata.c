// ATA PIO driver for the legacy IDE ports
// https://wiki.osdev.org/ATA_PIO_Mode
// https://wiki.osdev.org/MBR_(x86)
//
// The drive's registers are I/O PORTS, so they are read and written with inb / outb
// (not through pointers to memory addresses). Every command is polled, no IRQs are used.

#include "ata.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../input/GIO.h"

// I/O and control ports of bus 0 (primary) and bus 1 (secondary)
#define ATA_IO(bus)   ((uint16_t)((bus) ? 0x170 : 0x1F0))
#define ATA_CTRL(bus) ((uint16_t)((bus) ? 0x376 : 0x3F6))

// register offsets from the I/O base
#define ATA_REG_DATA     0
#define ATA_REG_FEATURES 1
#define ATA_REG_SECCOUNT 2
#define ATA_REG_LBA_LO   3
#define ATA_REG_LBA_MID  4
#define ATA_REG_LBA_HI   5
#define ATA_REG_DRIVE    6
#define ATA_REG_STATUS   7 // read
#define ATA_REG_COMMAND  7 // write

// status register bits
#define ATA_SR_BSY 0x80
#define ATA_SR_DF  0x20
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

// control register bit that stops the drive raising IRQ 14 / 15, the driver polls instead
#define ATA_CTRL_NIEN 0x02

#define ATA_CMD_READ_PIO        0x20
#define ATA_CMD_WRITE_PIO       0x30
#define ATA_CMD_CACHE_FLUSH     0xE7
#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1

#define ATA_LBA28_MAX 0x0FFFFFFF
#define ATA_TIMEOUT   1000000 // status polls before giving up on a drive
#define SECTOR_WORDS  (SECTOR_SIZE / 2)

const char* drive_type_name(int type) {
    switch (type) {
        case DRIVE_NONE: return "no drive";
        case DRIVE_ATA: return "ATA hard drive";
        case DRIVE_ATAPI: return "ATAPI (CD/DVD)";
        case DRIVE_SATA: return "SATA drive";
        case DRIVE_SATAPI: return "SATAPI drive";
        default: return "unknown device";
    }
}

// ~400ns delay: reading the alternate status register 4 times
static void ata_delay(uint16_t ctrl) {
    for (int i = 0; i < 4; i++) {
        inb(ctrl);
    }
}

static bool ata_wait_not_busy(uint16_t io) {
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        if ((inb(io + ATA_REG_STATUS) & ATA_SR_BSY) == 0) {
            return true;
        }
    }
    return false;
}

// wait for data (DRQ) or an error and return the status
// a timeout returns 0xFF, which has the error bit set just like a failed command
static uint8_t ata_wait_data(uint16_t io) {
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t status = inb(io + ATA_REG_STATUS);
        if (status & (ATA_SR_DRQ | ATA_SR_ERR)) {
            return status;
        }
    }
    return 0xFF;
}

// read the 256 word answer to an IDENTIFY command
static bool ata_read_identify(uint16_t io, uint16_t* id) {
    if (ata_wait_data(io) & ATA_SR_ERR) {
        return false;
    }
    insw(io + ATA_REG_DATA, id, SECTOR_WORDS);
    return true;
}

// CD drives abort the plain IDENTIFY and answer IDENTIFY PACKET DEVICE instead
static bool ata_identify_packet(uint16_t io, uint16_t ctrl, uint16_t* id) {
    outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
    ata_delay(ctrl);
    return ata_wait_not_busy(io) && ata_read_identify(io, id);
}

// the model name is in words 27-46, two characters per word (high byte first), padded at the end
static void ata_copy_model(const uint16_t* id, char* model, size_t model_size) {
    size_t n = 0;
    for (int w = 27; w <= 46 && n + 2 < model_size; w++) {
        model[n++] = (char)(id[w] >> 8);
        model[n++] = (char)(id[w] & 0xFF);
    }
    while (n > 0 && (model[n - 1] == ' ' || model[n - 1] == '\0')) {
        n--;
    }
    model[n] = '\0';
}

int drive_probe_info(int bus, int slave, char* model, size_t model_size, uint32_t* sectors) {
    uint16_t io = ATA_IO(bus);
    uint16_t ctrl = ATA_CTRL(bus);
    uint16_t id[SECTOR_WORDS];

    if (model && model_size) {
        model[0] = '\0';
    }
    if (sectors) {
        *sectors = 0;
    }

    // a floating bus (no controller / no drives) reads as 0xFF
    if (inb(io + ATA_REG_STATUS) == 0xFF) {
        return DRIVE_NONE;
    }

    outb(ctrl, ATA_CTRL_NIEN);
    outb(io + ATA_REG_DRIVE, (uint8_t)(0xA0 | (slave ? 0x10 : 0x00)));
    ata_delay(ctrl);

    outb(io + ATA_REG_SECCOUNT, 0);
    outb(io + ATA_REG_LBA_LO, 0);
    outb(io + ATA_REG_LBA_MID, 0);
    outb(io + ATA_REG_LBA_HI, 0);
    outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay(ctrl);

    uint8_t status = inb(io + ATA_REG_STATUS);
    if (status == 0 || status == 0xFF) {
        return DRIVE_NONE;
    }
    if (!ata_wait_not_busy(io)) {
        return DRIVE_UNKNOWN;
    }

    // a non-ATA device aborts IDENTIFY and leaves a signature in the LBA mid / high registers
    uint8_t mid = inb(io + ATA_REG_LBA_MID);
    uint8_t hi = inb(io + ATA_REG_LBA_HI);
    int type;
    bool ok;

    if (mid == 0 && hi == 0) {
        type = DRIVE_ATA;
        ok = ata_read_identify(io, id);
    } else if (mid == 0x14 && hi == 0xEB) {
        type = DRIVE_ATAPI;
        ok = ata_identify_packet(io, ctrl, id);
    } else if (mid == 0x69 && hi == 0x96) {
        type = DRIVE_SATAPI;
        ok = ata_identify_packet(io, ctrl, id);
    } else if (mid == 0x3C && hi == 0xC3) {
        return DRIVE_SATA;
    } else {
        return DRIVE_UNKNOWN;
    }

    if (!ok) {
        return type;
    }
    if (model && model_size > 1) {
        ata_copy_model(id, model, model_size);
    }
    // words 60-61 = number of sectors reachable with 28 bit LBA addressing
    if (sectors && type == DRIVE_ATA) {
        *sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    }
    return type;
}

int drive_probe(int bus, int slave, char* model, size_t model_size) {
    return drive_probe_info(bus, slave, model, model_size, NULL);
}

// select the drive and send a read / write command for count (1-255) sectors
static bool ata_pio_command(uint16_t io, uint16_t ctrl, int slave, uint32_t lba, uint8_t count, uint8_t command) {
    if (!ata_wait_not_busy(io)) {
        return false;
    }
    outb(ctrl, ATA_CTRL_NIEN);
    outb(io + ATA_REG_DRIVE, (uint8_t)(0xE0 | (slave ? 0x10 : 0x00) | ((lba >> 24) & 0x0F)));
    ata_delay(ctrl);
    outb(io + ATA_REG_FEATURES, 0);
    outb(io + ATA_REG_SECCOUNT, count);
    outb(io + ATA_REG_LBA_LO, (uint8_t)lba);
    outb(io + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    outb(io + ATA_REG_LBA_HI, (uint8_t)(lba >> 16));
    outb(io + ATA_REG_COMMAND, command);
    return true;
}

// wait until the drive is ready to transfer the next sector
static bool ata_pio_ready(uint16_t io, uint16_t ctrl) {
    ata_delay(ctrl);
    return ata_wait_not_busy(io) && (ata_wait_data(io) & (ATA_SR_ERR | ATA_SR_DF)) == 0;
}

// move count sectors between data and the drive, at most 255 per command
static bool ata_pio_transfer(int bus, int slave, uint32_t lba, uint32_t count, uint8_t* data, bool write) {
    uint16_t io = ATA_IO(bus);
    uint16_t ctrl = ATA_CTRL(bus);

    if (count == 0 || lba > ATA_LBA28_MAX || count > ATA_LBA28_MAX - lba + 1) {
        return false;
    }
    while (count > 0) {
        uint8_t n = (count > 255) ? 255 : (uint8_t)count;
        if (!ata_pio_command(io, ctrl, slave, lba, n, write ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO)) {
            return false;
        }
        for (uint8_t s = 0; s < n; s++, data += SECTOR_SIZE) {
            if (!ata_pio_ready(io, ctrl)) {
                return false;
            }
            if (write) {
                outsw(io + ATA_REG_DATA, data, SECTOR_WORDS);
            } else {
                insw(io + ATA_REG_DATA, data, SECTOR_WORDS);
            }
        }
        lba += n;
        count -= n;
    }
    return true;
}

bool drive_read(int bus, int slave, uint32_t lba, uint32_t count, void* buffer) {
    return ata_pio_transfer(bus, slave, lba, count, buffer, false);
}

bool drive_write(int bus, int slave, uint32_t lba, uint32_t count, const void* buffer) {
    uint16_t io = ATA_IO(bus);
    uint16_t ctrl = ATA_CTRL(bus);

    // the buffer is only read from when writing
    if (!ata_pio_transfer(bus, slave, lba, count, (uint8_t*)buffer, true)) {
        return false;
    }

    // make sure the data has left the drive's cache before we report success
    // (the drive ignores commands while it is still busy with the last sector)
    if (!ata_wait_not_busy(io)) {
        return false;
    }
    outb(io + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_delay(ctrl);
    if (!ata_wait_not_busy(io)) {
        return false;
    }
    return (inb(io + ATA_REG_STATUS) & (ATA_SR_ERR | ATA_SR_DF)) == 0;
}

bool drive_is_fat_boot_sector(const uint8_t* sector) {
    uint16_t bytes_per_sector = (uint16_t)(sector[11] | (sector[12] << 8));
    uint8_t sectors_per_cluster = sector[13];
    uint16_t reserved = (uint16_t)(sector[14] | (sector[15] << 8));
    uint8_t fats = sector[16];

    if (sector[0] != 0xEB && sector[0] != 0xE9) {
        return false; // boot sectors start with a jump instruction
    }
    // sectors per cluster has to be a power of two
    return bytes_per_sector == SECTOR_SIZE && sectors_per_cluster != 0 &&
           (sectors_per_cluster & (sectors_per_cluster - 1)) == 0 &&
           reserved != 0 && fats != 0 && fats <= 4;
}

// MBR partition types that hold a FAT file system
static bool drive_is_fat_partition(uint8_t type) {
    return type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0B || type == 0x0C || type == 0x0E;
}

int drive_parse(int bus, int slave, uint32_t* volume_lba) {
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
        uint32_t start = (uint32_t)entry[8] | ((uint32_t)entry[9] << 8) |
                         ((uint32_t)entry[10] << 16) | ((uint32_t)entry[11] << 24);
        if (drive_is_fat_partition(entry[4]) && start != 0) {
            *volume_lba = start;
            return 1;
        }
    }
    return 0;
}
