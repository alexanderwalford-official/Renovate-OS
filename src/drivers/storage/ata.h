// ATA PIO driver for the legacy IDE ports
// https://wiki.osdev.org/ATA_PIO_Mode
// https://wiki.osdev.org/SATA
// https://wiki.osdev.org/MBR_(x86)
//
// Detects drives with the IDENTIFY command, reads and writes 512 byte sectors (28 bit LBA) and
// finds the FAT volume on a drive. This covers IDE (PATA) drives, CD drives (ATAPI) and SATA
// drives running in IDE compatibility mode. Native SATA (AHCI) controllers need a PCI driver,
// which does not exist yet.
//
// A PC has (up to) two legacy IDE buses, primary and secondary, and each bus can hold two drives,
// the "master" and the "slave". Every function here names a drive by that pair:
//     bus 0 slave 0 = primary master     (QEMU's -hda)
//     bus 0 slave 1 = primary slave      (-hdb)
//     bus 1 slave 0 = secondary master   (-hdc, or the -cdrom drive)
//     bus 1 slave 1 = secondary slave    (-hdd)
// A disk is addressed as a flat array of 512 byte sectors numbered from 0, the Logical Block
// Address (LBA). With 28 bit LBA that reaches 2^28 sectors = 128 GiB.
// The FAT file system code in sys/fat.c sits on top of drive_read / drive_write / drive_parse.

#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// bytes per disk sector, the unit every read and write works in
#define SECTOR_SIZE 512

// what drive_probe found at a bus / slave position
enum drive_type {
    DRIVE_NONE = 0,  // nothing answered
    DRIVE_ATA,       // a normal (PATA, or SATA in IDE mode) hard drive: the only kind we can read and write
    DRIVE_ATAPI,     // a CD / DVD drive (talks in SCSI style packets, not supported for reading)
    DRIVE_SATA,      // a drive that reports the SATA signature, it is not identified any further
    DRIVE_SATAPI,    // a SATA CD / DVD drive
    DRIVE_UNKNOWN    // something answered, but stayed busy or gave a signature we don't know
};

// human readable name for a drive_type, e.g. "ATA hard drive" (never NULL)
const char* drive_type_name(int type);

// detect the drive at bus (0 = primary, 1 = secondary) / slave (0 = master, 1 = slave), returns a drive_type
// model receives the drive's model name if it reports one, sectors (optional) its size for ATA drives
// model may be NULL (or model_size 0) when the name isn't wanted
int drive_probe(int bus, int slave, char* model, size_t model_size);

// same as drive_probe, and also stores the drive's size in sectors in *sectors (0 if unknown)
int drive_probe_info(int bus, int slave, char* model, size_t model_size, uint32_t* sectors);

// read / write count sectors starting at lba, buffer holds count * SECTOR_SIZE bytes
// both return true on success and false on a drive error, timeout or out of range request.
// drive_write also flushes the drive's write cache before returning
bool drive_read(int bus, int slave, uint32_t lba, uint32_t count, void* buffer);
bool drive_write(int bus, int slave, uint32_t lba, uint32_t count, const void* buffer);

// does this sector look like a FAT boot sector?
// (checks the jump instruction and that the BIOS parameter block values are sensible)
bool drive_is_fat_boot_sector(const uint8_t* sector);

// find the FAT volume on a drive: 1 = found (volume_lba = its first sector), 0 = none, -1 = read error
// understands an unpartitioned "superfloppy" disk and the four primary MBR partitions
int drive_parse(int bus, int slave, uint32_t* volume_lba);

#endif /* ATA_H */
