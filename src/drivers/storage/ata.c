// ATA PIO driver for the legacy IDE ports
// https://wiki.osdev.org/ATA_PIO_Mode
// https://wiki.osdev.org/MBR_(x86)
//
// The drive's registers are I/O PORTS, so they are read and written with inb / outb
// (not through pointers to memory addresses). Every command is polled, no IRQs are used.
//
// PIO ("programmed I/O") means the CPU moves every byte itself: the drive collects a sector in its
// internal buffer, raises DRQ in its status register, and we copy the 512 bytes through the 16 bit
// data port with "rep insw" / "rep outsw". It's slow compared with DMA, but very simple.
//
// Every command follows the same pattern:
//   1. wait until the drive is not busy (BSY clear)
//   2. select master or slave in the drive register and give it ~400ns to switch over
//   3. fill in the sector count and the LBA, then write the command number to the command register
//   4. for each sector: wait for BSY clear and DRQ set (or ERR set if it went wrong), then move the
//      256 words of data
//
// Each bus has two blocks of ports:
//   I/O base (0x1F0 primary, 0x170 secondary), 8 registers:
//     +0 data (16 bit)     +1 error / features   +2 sector count   +3 LBA bits 0-7
//     +4 LBA bits 8-15     +5 LBA bits 16-23     +6 drive / head   +7 status / command
//   control base (0x3F6 primary, 0x376 secondary), 1 register:
//     read = "alternate status" (a copy of status that has no side effects)
//     write = "device control" (interrupt disable, software reset)
// The drive / head register: bit 4 picks the slave, bits 5 and 7 must always be 1, bit 6 = LBA
// addressing, and in LBA mode bits 0-3 carry LBA bits 24-27.

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
// BSY  = the drive is working on something, and while it is set none of the other bits mean anything
// DF   = drive fault (a failure that isn't reported through ERR)
// DRQ  = data request: the drive is ready to send or receive a sector through the data port
// ERR  = the last command failed (the error register at +1 says why)
#define ATA_SR_BSY 0x80
#define ATA_SR_DF  0x20
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

// control register bit that stops the drive raising IRQ 14 / 15, the driver polls instead
#define ATA_CTRL_NIEN 0x02

// command numbers written to the command register
#define ATA_CMD_READ_PIO        0x20 // READ SECTORS (28 bit LBA)
#define ATA_CMD_WRITE_PIO       0x30 // WRITE SECTORS (28 bit LBA)
#define ATA_CMD_CACHE_FLUSH     0xE7 // FLUSH CACHE: write anything still in the drive's cache to the disk
#define ATA_CMD_IDENTIFY        0xEC // IDENTIFY DEVICE: 512 bytes describing an ATA drive
#define ATA_CMD_IDENTIFY_PACKET 0xA1 // IDENTIFY PACKET DEVICE: the same for ATAPI (CD) drives

// the highest sector number 28 bit LBA can address (2^28 - 1)
#define ATA_LBA28_MAX 0x0FFFFFFF
#define ATA_TIMEOUT   1000000 // status polls before giving up on a drive
// a sector moves through the 16 bit data port as 256 words
#define SECTOR_WORDS  (SECTOR_SIZE / 2)

// turn a drive_type into text for the terminal's drive listing
// type is an int rather than the enum so any value can be passed, unknown ones get "unknown device"
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
// after a drive is selected (or a command sent) it can take up to 400ns before the status register
// shows the new drive's real state. Each port read takes at least ~100ns on the bus, so 4 reads
// cover it. The alternate status register is used because reading it has no side effects.
// ctrl: the bus's control port
static void ata_delay(uint16_t ctrl) {
    for (int i = 0; i < 4; i++) {
        inb(ctrl);
    }
}

// poll the status register until BSY clears
// io: the bus's I/O base. Returns false if the drive is still busy after ATA_TIMEOUT polls
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
// (0xFF has DF set too, so callers checking ERR | DF also see a timeout as a failure)
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
// id: room for SECTOR_WORDS words. Returns false if the drive reported an error (or timed out),
// which is what an ATAPI drive does when sent the plain ATA IDENTIFY
static bool ata_read_identify(uint16_t io, uint16_t* id) {
    if (ata_wait_data(io) & ATA_SR_ERR) {
        return false;
    }
    insw(io + ATA_REG_DATA, id, SECTOR_WORDS);
    return true;
}

// CD drives abort the plain IDENTIFY and answer IDENTIFY PACKET DEVICE instead
// the drive is still selected from the first attempt, so we only need to send the new command
static bool ata_identify_packet(uint16_t io, uint16_t ctrl, uint16_t* id) {
    outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
    ata_delay(ctrl);
    return ata_wait_not_busy(io) && ata_read_identify(io, id);
}

// the model name is in words 27-46, two characters per word (high byte first), padded at the end
// so "QEMU HARDDISK" arrives as the words 'QE' 'MU' ' H' ... and has to be unpacked byte by byte
// model: output buffer, always NUL terminated (needs model_size >= 1), with the padding trimmed off
static void ata_copy_model(const uint16_t* id, char* model, size_t model_size) {
    size_t n = 0;
    // n + 2 < model_size: only copy a word if both its characters and the final NUL still fit
    for (int w = 27; w <= 46 && n + 2 < model_size; w++) {
        model[n++] = (char)(id[w] >> 8);
        model[n++] = (char)(id[w] & 0xFF);
    }
    // strip the trailing space (or NUL) padding
    while (n > 0 && (model[n - 1] == ' ' || model[n - 1] == '\0')) {
        n--;
    }
    model[n] = '\0';
}

// find out what is attached at bus / slave by sending IDENTIFY
// model / model_size: optional buffer for the drive's model name ("" if there isn't one)
// sectors: optional, receives the size of an ATA drive in sectors (0 otherwise)
// returns a drive_type. Note the type can be known even when the IDENTIFY data couldn't be read,
// in which case model and sectors are just left empty.
int drive_probe_info(int bus, int slave, char* model, size_t model_size, uint32_t* sectors) {
    uint16_t io = ATA_IO(bus);
    uint16_t ctrl = ATA_CTRL(bus);
    // the 512 byte IDENTIFY answer, on the stack
    uint16_t id[SECTOR_WORDS];

    // give the outputs safe values first so every early return leaves them valid
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

    // turn off the drive's interrupts (the device control register reaches both drives on the bus),
    // then select the drive: 0xA0 = the two always-1 bits, plus 0x10 for the slave
    outb(ctrl, ATA_CTRL_NIEN);
    outb(io + ATA_REG_DRIVE, (uint8_t)(0xA0 | (slave ? 0x10 : 0x00)));
    ata_delay(ctrl);

    // IDENTIFY wants sector count and the LBA registers zeroed. That also matters for detection:
    // the drive overwrites LBA mid / high with its signature, so any non-zero value we read back
    // later really came from the drive
    outb(io + ATA_REG_SECCOUNT, 0);
    outb(io + ATA_REG_LBA_LO, 0);
    outb(io + ATA_REG_LBA_MID, 0);
    outb(io + ATA_REG_LBA_HI, 0);
    outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay(ctrl);

    // status 0 = there is no drive at this position (the other drive on the bus may still exist),
    // 0xFF = nothing on the bus at all
    uint8_t status = inb(io + ATA_REG_STATUS);
    if (status == 0 || status == 0xFF) {
        return DRIVE_NONE;
    }
    // something answered but never finished: count it as there, but of an unknown kind
    if (!ata_wait_not_busy(io)) {
        return DRIVE_UNKNOWN;
    }

    // a non-ATA device aborts IDENTIFY and leaves a signature in the LBA mid / high registers
    //     0x00 0x00 = ATA          0x14 0xEB = ATAPI
    //     0x69 0x96 = SATAPI       0x3C 0xC3 = SATA
    uint8_t mid = inb(io + ATA_REG_LBA_MID);
    uint8_t hi = inb(io + ATA_REG_LBA_HI);
    int type;
    bool ok;

    // an ATA drive accepted IDENTIFY, so its answer is already waiting. The packet (CD) drives
    // need the second command first. A SATA drive in this legacy mode can't be identified or used
    // through these ports, so we only report that it is there
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

    // the type is known even if the IDENTIFY data never came, so report it anyway
    if (!ok) {
        return type;
    }
    if (model && model_size > 1) {
        ata_copy_model(id, model, model_size);
    }
    // words 60-61 = number of sectors reachable with 28 bit LBA addressing
    // (low word first; ATAPI drives don't fill these in, a CD's size has to be asked for with a
    // READ CAPACITY packet command, which this driver doesn't send)
    if (sectors && type == DRIVE_ATA) {
        *sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    }
    return type;
}

// drive_probe_info without the size
int drive_probe(int bus, int slave, char* model, size_t model_size) {
    return drive_probe_info(bus, slave, model, model_size, NULL);
}

// select the drive and send a read / write command for count (1-255) sectors
// count 0 is avoided on purpose: to the drive a sector count of 0 means 256 sectors
// returns false if the drive was still busy with something else
static bool ata_pio_command(uint16_t io, uint16_t ctrl, int slave, uint32_t lba, uint8_t count, uint8_t command) {
    // the drive selection can't be changed while the bus is busy
    if (!ata_wait_not_busy(io)) {
        return false;
    }
    // keep the drive's IRQ off, we poll the status register instead
    outb(ctrl, ATA_CTRL_NIEN);
    // 0xE0 = the always-1 bits plus bit 6 (LBA mode), 0x10 for the slave, and the top 4 bits of the
    // 28 bit LBA go in the low nibble
    outb(io + ATA_REG_DRIVE, (uint8_t)(0xE0 | (slave ? 0x10 : 0x00) | ((lba >> 24) & 0x0F)));
    ata_delay(ctrl);
    // READ / WRITE SECTORS take no features, 0 is the safe value
    outb(io + ATA_REG_FEATURES, 0);
    outb(io + ATA_REG_SECCOUNT, count);
    // the remaining 24 bits of the LBA, one byte per register
    outb(io + ATA_REG_LBA_LO, (uint8_t)lba);
    outb(io + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    outb(io + ATA_REG_LBA_HI, (uint8_t)(lba >> 16));
    // writing the command register is what starts the command, so it always goes last
    outb(io + ATA_REG_COMMAND, command);
    return true;
}

// wait until the drive is ready to transfer the next sector
// true = DRQ is set and neither ERR nor DF is, so exactly one sector can now go through the data port
static bool ata_pio_ready(uint16_t io, uint16_t ctrl) {
    // give the status register its ~400ns to catch up with the last command / sector first
    ata_delay(ctrl);
    return ata_wait_not_busy(io) && (ata_wait_data(io) & (ATA_SR_ERR | ATA_SR_DF)) == 0;
}

// move count sectors between data and the drive, at most 255 per command
// write: true = data -> drive (data is only read from), false = drive -> data
// returns false for an empty or out of range request and for any drive error / timeout. A failure
// part way through can leave some sectors already transferred.
static bool ata_pio_transfer(int bus, int slave, uint32_t lba, uint32_t count, uint8_t* data, bool write) {
    uint16_t io = ATA_IO(bus);
    uint16_t ctrl = ATA_CTRL(bus);

    // the last sector (lba + count - 1) must still fit in 28 bits. Written as
    // count > MAX - lba + 1 so nothing can overflow (lba <= MAX was checked first)
    if (count == 0 || lba > ATA_LBA28_MAX || count > ATA_LBA28_MAX - lba + 1) {
        return false;
    }
    // big requests are split into commands of up to 255 sectors
    while (count > 0) {
        uint8_t n = (count > 255) ? 255 : (uint8_t)count;
        if (!ata_pio_command(io, ctrl, slave, lba, n, write ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO)) {
            return false;
        }
        // the drive hands over (or takes) one sector at a time, raising DRQ before each one
        for (uint8_t s = 0; s < n; s++, data += SECTOR_SIZE) {
            if (!ata_pio_ready(io, ctrl)) {
                return false;
            }
            // 256 16 bit words = one 512 byte sector, moved with rep outsw / rep insw
            if (write) {
                outsw(io + ATA_REG_DATA, data, SECTOR_WORDS);
            } else {
                insw(io + ATA_REG_DATA, data, SECTOR_WORDS);
            }
        }
        // move on to the next chunk (data has already been stepped forward by the loop above)
        lba += n;
        count -= n;
    }
    return true;
}

// read count sectors starting at lba into buffer (count * 512 bytes)
bool drive_read(int bus, int slave, uint32_t lba, uint32_t count, void* buffer) {
    return ata_pio_transfer(bus, slave, lba, count, buffer, false);
}

// write count sectors from buffer starting at lba, then flush the drive's cache
// only returns true once the drive says the data is really on the disk
bool drive_write(int bus, int slave, uint32_t lba, uint32_t count, const void* buffer) {
    uint16_t io = ATA_IO(bus);
    uint16_t ctrl = ATA_CTRL(bus);

    // the buffer is only read from when writing
    // (so casting the const away for the shared transfer helper is safe)
    if (!ata_pio_transfer(bus, slave, lba, count, (uint8_t*)buffer, true)) {
        return false;
    }

    // make sure the data has left the drive's cache before we report success
    // (the drive ignores commands while it is still busy with the last sector)
    // without the flush a drive may keep the data in its RAM cache and lose it if the power goes
    if (!ata_wait_not_busy(io)) {
        return false;
    }
    outb(io + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_delay(ctrl);
    // the flush keeps the drive busy until the cache is empty, then the status says how it went
    if (!ata_wait_not_busy(io)) {
        return false;
    }
    return (inb(io + ATA_REG_STATUS) & (ATA_SR_ERR | ATA_SR_DF)) == 0;
}

// a FAT volume starts with a boot sector holding the BIOS Parameter Block (BPB), which describes
// the file system. The fields checked here (all little endian):
//     offset 0      a jump over the BPB: 0xEB (short jump) or 0xE9 (near jump)
//     offset 11-12  bytes per sector, has to be 512 because that's all this driver handles
//     offset 13     sectors per cluster, a power of two
//     offset 14-15  reserved sectors before the first FAT (at least 1: the boot sector itself)
//     offset 16     number of FAT copies (normally 2)
// Real boot sectors pass easily, random data (or a non-FAT boot sector) almost never does.
// The 0x55 0xAA signature is not checked here, drive_parse does that before calling us.
bool drive_is_fat_boot_sector(const uint8_t* sector) {
    // the 16 bit fields are little endian: low byte first, so the second byte is shifted up by 8
    uint16_t bytes_per_sector = (uint16_t)(sector[11] | (sector[12] << 8));
    uint8_t sectors_per_cluster = sector[13];
    uint16_t reserved = (uint16_t)(sector[14] | (sector[15] << 8));
    uint8_t fats = sector[16];

    if (sector[0] != 0xEB && sector[0] != 0xE9) {
        return false; // boot sectors start with a jump instruction
    }
    // sectors per cluster has to be a power of two
    // (a power of two has exactly one bit set, x - 1 flips it and sets all the bits below,
    // so x & (x - 1) is 0 only for powers of two)
    return bytes_per_sector == SECTOR_SIZE && sectors_per_cluster != 0 &&
           (sectors_per_cluster & (sectors_per_cluster - 1)) == 0 &&
           reserved != 0 && fats != 0 && fats <= 4;
}

// MBR partition types that hold a FAT file system
// 0x01 = FAT12, 0x04 = FAT16 under 32MB, 0x06 = FAT16 of 32MB and over, 0x0B = FAT32 (CHS),
// 0x0C = FAT32 (LBA), 0x0E = FAT16 (LBA)
static bool drive_is_fat_partition(uint8_t type) {
    return type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0B || type == 0x0C || type == 0x0E;
}

// work out where the FAT volume on a drive starts by looking at sector 0
// volume_lba: receives the first sector of the volume when 1 is returned
// sector 0 is either the boot sector of a whole-disk FAT volume, or a Master Boot Record:
//     bytes 0-445    boot loader code
//     bytes 446-509  the partition table, four 16 byte entries
//     bytes 510-511  the signature 0x55 0xAA (shared with FAT boot sectors)
// Only the four primary partitions are looked at (extended / logical partitions aren't followed),
// and the first FAT one wins.
int drive_parse(int bus, int slave, uint32_t* volume_lba) {
    uint8_t sector[SECTOR_SIZE];

    // -1 = the drive couldn't even be read, which is different from "readable but not FAT"
    if (!drive_read(bus, slave, 0, 1, sector)) {
        return -1;
    }
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        return 0; // blank or unknown
    }

    // a drive formatted without a partition table ("superfloppy")
    // checked first because the bytes at 446 of a FAT boot sector are boot code, not a partition
    // table. The BPB check also keeps us from mistaking an MBR whose code starts with a jump for this.
    if (drive_is_fat_boot_sector(sector)) {
        *volume_lba = 0;
        return 1;
    }

    // MBR: four 16 byte partition entries at offset 446 (GPT disks show a single 0xEE entry and are skipped)
    // inside an entry: byte 0 = bootable flag (0x80), 1-3 = start as CHS (ignored), byte 4 = type,
    // 5-7 = end as CHS, bytes 8-11 = first sector as a little endian LBA, 12-15 = sector count
    for (int i = 0; i < 4; i++) {
        const uint8_t* entry = sector + 446 + i * 16;
        uint32_t start = (uint32_t)entry[8] | ((uint32_t)entry[9] << 8) |
                         ((uint32_t)entry[10] << 16) | ((uint32_t)entry[11] << 24);
        // a start of 0 would point back at the MBR itself, so treat that entry as broken
        if (drive_is_fat_partition(entry[4]) && start != 0) {
            *volume_lba = start;
            return 1;
        }
    }
    return 0;
}
