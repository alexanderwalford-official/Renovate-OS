/*
    pci.c - finding devices on the PCI bus (see pci.h)

    The address written to port 0xCF8 is built like this:

        bit 31      enable (always 1)
        bits 16-23  bus        (0 to 255)
        bits 11-15  slot       (0 to 31)
        bits 8-10   function   (0 to 7, most devices only have function 0)
        bits 2-7    register   (which 32 bit word of configuration space)

    Configuration space offsets used here:
        0x00  vendor ID (0xFFFF = nothing there) and device ID
        0x04  command register (bit 0 = I/O ports on, bit 2 = bus master)
        0x08  revision, then the class code in the top byte and the subclass below it
        0x0C  header type in byte 2 (bit 7 = the device has more than one function)
        0x10  base address registers 0 to 5, 4 bytes each
*/

#include "pci.h"
#include "../input/GIO.h"

#define PCI_ADDRESS 0xCF8
#define PCI_DATA    0xCFC

#define PCI_COMMAND_IO         0x0001
#define PCI_COMMAND_BUS_MASTER 0x0004

static uint32_t pci_read (uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    outl(PCI_ADDRESS, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                      ((uint32_t)function << 8) | (offset & 0xFC));
    return inl(PCI_DATA);
}

static void pci_write (const pci_device_t* dev, uint8_t offset, uint32_t value) {
    outl(PCI_ADDRESS, 0x80000000u | ((uint32_t)dev->bus << 16) | ((uint32_t)dev->slot << 11) |
                      ((uint32_t)dev->function << 8) | (offset & 0xFC));
    outl(PCI_DATA, value);
}

// visit every device on every bus, until fn returns true. Returns true if fn stopped it early
bool pci_each (bool (*fn)(const pci_device_t* dev, void* ctx), void* ctx) {
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int function = 0; function < 8; function++) {
                uint32_t id = pci_read(bus, slot, function, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (function == 0) break; // no device in this slot at all
                    continue;
                }
                uint32_t class_word = pci_read(bus, slot, function, 0x08);
                pci_device_t dev = {
                    (uint8_t)bus, (uint8_t)slot, (uint8_t)function, (uint16_t)id, (uint16_t)(id >> 16),
                    (uint8_t)(class_word >> 24), (uint8_t)(class_word >> 16)
                };
                if (fn(&dev, ctx)) return true;
                // a single function device only answers as function 0
                if (function == 0 && !(pci_read(bus, slot, 0, 0x0C) & 0x00800000)) break;
            }
        }
    }
    return false;
}

// pci_find_class: stop at the first device whose class matches
typedef struct {
    uint8_t class_code, subclass;
    pci_device_t* out;
} find_class_t;

static bool match_class (const pci_device_t* dev, void* ctx) {
    find_class_t* want = ctx;
    if (dev->class_code != want->class_code || dev->subclass != want->subclass) return false;
    *want->out = *dev;
    return true;
}

bool pci_find_class (uint8_t class_code, uint8_t subclass, pci_device_t* out) {
    find_class_t want = { class_code, subclass, out };
    return pci_each(match_class, &want);
}

uint16_t pci_io_bar (const pci_device_t* dev, int bar) {
    uint32_t value = pci_read(dev->bus, dev->slot, dev->function, (uint8_t)(0x10 + bar * 4));
    // bit 0 set means I/O ports, the low 2 bits are flags and not part of the address
    return (value & 1) ? (uint16_t)(value & 0xFFFC) : 0;
}

void pci_enable_io_and_dma (const pci_device_t* dev) {
    uint32_t command = pci_read(dev->bus, dev->slot, dev->function, 0x04);
    // the top 16 bits are the status register, writing 1s there would clear its flags, so leave them 0
    pci_write(dev, 0x04, (command & 0xFFFF) | PCI_COMMAND_IO | PCI_COMMAND_BUS_MASTER);
}
