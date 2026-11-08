// ATA PIO driver for the legacy IDE ports
// https://wiki.osdev.org/ATA_PIO_Mode
// https://wiki.osdev.org/SATA
// https://wiki.osdev.org/MBR_(x86)
//
// Detects drives with the IDENTIFY command, reads and writes 512 byte sectors (28 bit LBA) and
// finds the FAT volume on a drive. This covers IDE (PATA) drives, CD drives (ATAPI) and SATA
// drives running in IDE compatibility mode. Native SATA (AHCI) controllers need a PCI driver,
// which does not exist yet.

#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SECTOR_SIZE 512

enum drive_type {
    DRIVE_NONE = 0,
    DRIVE_ATA,
    DRIVE_ATAPI,
    DRIVE_SATA,
    DRIVE_SATAPI,
    DRIVE_UNKNOWN
};

const char* drive_type_name(int type);

// detect the drive at bus (0 = primary, 1 = secondary) / slave (0 = master, 1 = slave), returns a drive_type
// model receives the drive's model name if it reports one, sectors (optional) its size for ATA drives
int drive_probe(int bus, int slave, char* model, size_t model_size);
int drive_probe_info(int bus, int slave, char* model, size_t model_size, uint32_t* sectors);

// read / write count sectors starting at lba, buffer holds count * SECTOR_SIZE bytes
bool drive_read(int bus, int slave, uint32_t lba, uint32_t count, void* buffer);
bool drive_write(int bus, int slave, uint32_t lba, uint32_t count, const void* buffer);

// does this sector look like a FAT boot sector?
bool drive_is_fat_boot_sector(const uint8_t* sector);

// find the FAT volume on a drive: 1 = found (volume_lba = its first sector), 0 = none, -1 = read error
int drive_parse(int bus, int slave, uint32_t* volume_lba);

#endif /* ATA_H */
