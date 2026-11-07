// For SATA interfaces only
// https://wiki.osdev.org/SATA
// https://wiki.osdev.org/ATA_PIO_Mode
//
// Drive detection using the ATA IDENTIFY command over the legacy IDE I/O ports.
// This finds IDE (PATA) drives, CD drives (ATAPI) and SATA drives running in IDE compatibility
// mode. Native SATA (AHCI) controllers need a PCI driver, which does not exist yet.
//
// The drive's registers are I/O PORTS, so they are read and written with inb / outb
// (not through pointers to memory addresses).

#ifndef SATA_SCANNING_H
#define SATA_SCANNING_H

#include <stdint.h>
#include <stdbool.h>
#include "../../input/GIO.h"
#include "../../../lib/primary_definitions.h"

// primary ATA bus I/O ports
#define ATA_PRIMARY_IO     0x1F0
#define ATA_PRIMARY_CTRL   0x3F6
#define ATA_SECONDARY_IO   0x170
#define ATA_SECONDARY_CTRL 0x376

// register offsets from the I/O base
#define ATA_REG_DATA     0
#define ATA_REG_ERROR    1
#define ATA_REG_SECCOUNT 2
#define ATA_REG_LBA_LO   3
#define ATA_REG_LBA_MID  4 // 0x1F4 on the primary bus
#define ATA_REG_LBA_HI   5 // 0x1F5 on the primary bus
#define ATA_REG_DRIVE    6
#define ATA_REG_STATUS   7 // read
#define ATA_REG_COMMAND  7 // write

// status register bits
#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1

enum drive_type {
    DRIVE_NONE = 0,
    DRIVE_ATA,
    DRIVE_ATAPI,
    DRIVE_SATA,
    DRIVE_SATAPI,
    DRIVE_UNKNOWN
};

const char* drive_type_name (int type) {
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
void ata_delay (uint16_t ctrl) {
    for (int i = 0; i < 4; i++) {
        inb(ctrl);
    }
}

bool ata_wait_not_busy (uint16_t io) {
    for (int i = 0; i < 1000000; i++) {
        if ((inb(io + ATA_REG_STATUS) & ATA_SR_BSY) == 0) {
            return true;
        }
    }
    return false;
}

// wait for data (DRQ) or an error, returns the final status (0xFF = timed out)
uint8_t ata_wait_data (uint16_t io) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t status = inb(io + ATA_REG_STATUS);
        if (status & (ATA_SR_DRQ | ATA_SR_ERR)) {
            return status;
        }
    }
    return 0xFF;
}

// send an IDENTIFY style command and read the 256 word answer, returns false on error
bool ata_identify (uint16_t io, uint16_t ctrl, uint8_t command, uint16_t* data) {
    outb(io + ATA_REG_COMMAND, command);
    ata_delay(ctrl);
    if (!ata_wait_not_busy(io)) {
        return false;
    }
    uint8_t status = ata_wait_data(io);
    if (status == 0xFF || (status & ATA_SR_ERR)) {
        return false;
    }
    for (int i = 0; i < 256; i++) {
        data[i] = inw(io + ATA_REG_DATA);
    }
    return true;
}

// detect the drive at bus (0 = primary, 1 = secondary) / slave (0 = master, 1 = slave)
// model receives the drive's model name if it reports one
int drive_probe (int bus, int slave, char* model, size_t model_size) {
    uint16_t io = bus ? ATA_SECONDARY_IO : ATA_PRIMARY_IO;
    uint16_t ctrl = bus ? ATA_SECONDARY_CTRL : ATA_PRIMARY_CTRL;
    uint16_t data[256];

    if (model && model_size) {
        model[0] = '\0';
    }

    // a floating bus (no controller / no drives) reads as 0xFF
    if (inb(io + ATA_REG_STATUS) == 0xFF) {
        return DRIVE_NONE;
    }

    // select the drive
    outb(io + ATA_REG_DRIVE, (uint8_t)(0xA0 | (slave ? 0x10 : 0x00)));
    ata_delay(ctrl);

    // IDENTIFY COMMAND
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
        status = ata_wait_data(io);
        ok = (status != 0xFF) && !(status & ATA_SR_ERR);
        if (ok) {
            for (int i = 0; i < 256; i++) {
                data[i] = inw(io + ATA_REG_DATA);
            }
        }
    } else if (mid == 0x14 && hi == 0xEB) {
        type = DRIVE_ATAPI;
        ok = ata_identify(io, ctrl, ATA_CMD_IDENTIFY_PACKET, data);
    } else if (mid == 0x69 && hi == 0x96) {
        type = DRIVE_SATAPI;
        ok = ata_identify(io, ctrl, ATA_CMD_IDENTIFY_PACKET, data);
    } else if (mid == 0x3C && hi == 0xC3) {
        type = DRIVE_SATA;
        ok = false;
    } else {
        return DRIVE_UNKNOWN;
    }

    // the model name is in words 27-46, two characters per word (high byte first)
    if (ok && model && model_size > 1) {
        size_t n = 0;
        for (int w = 27; w <= 46 && n + 2 < model_size; w++) {
            model[n++] = (char)(data[w] >> 8);
            model[n++] = (char)(data[w] & 0xFF);
        }
        model[n] = '\0';
        StrTrim(model);
    }
    return type;
}

char drive_response[64];

// check the primary master drive, returns a short description (or the error)
char* drive_check_err () {
    char model[41];
    int type = drive_probe(0, 0, model, sizeof(model));
    snprintf(drive_response, sizeof(drive_response), "%s %s", drive_type_name(type), model);
    return drive_response;
}

int drive_init (char arg) {
    if (arg == 'E') {
        drive_check_err();
    }
    return 0;
}

#endif /* SATA_SCANNING_H */
