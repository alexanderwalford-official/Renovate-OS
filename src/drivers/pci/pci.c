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

// the two I/O ports for configuration space: say where in 0xCF8, then read or write 0xCFC
#define PCI_ADDRESS 0xCF8
#define PCI_DATA    0xCFC

// bits in the command register (offset 0x04)
#define PCI_COMMAND_IO         0x0001
#define PCI_COMMAND_MEMORY     0x0002
#define PCI_COMMAND_BUS_MASTER 0x0004

// Read one 32 bit word of a device's configuration space: write its address to 0xCF8 (built
// as in the table above), and the word appears at 0xCFC. The offset's low 2 bits are cleared
// because only whole words can be read; the caller picks the bytes it wants out of the word
static uint32_t pci_read (uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    outl(PCI_ADDRESS, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                      ((uint32_t)function << 8) | (offset & 0xFC));
    return inl(PCI_DATA);
}

// write one 32 bit word of a device's configuration space, the same way pci_read reads one
static void pci_write (const pci_device_t* dev, uint8_t offset, uint32_t value) {
    outl(PCI_ADDRESS, 0x80000000u | ((uint32_t)dev->bus << 16) | ((uint32_t)dev->slot << 11) |
                      ((uint32_t)dev->function << 8) | (offset & 0xFC));
    outl(PCI_DATA, value);
}

// visit every device on every bus, until fn returns true. Returns true if fn stopped it early
bool pci_each (bool (*fn)(const pci_device_t* dev, void* ctx), void* ctx) {
    // Every possible place, the simple way: 256 buses x 32 slots x up to 8 functions. Empty
    // slots answer at once, so even all of that is quick, and it finds devices behind
    // bridges without having to follow the bridges
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int function = 0; function < 8; function++) {
                // nothing answers an empty place, and reading "nothing" gives all 1s, so
                // vendor 0xFFFF (not a real vendor) means no device
                uint32_t id = pci_read(bus, slot, function, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (function == 0) {
                        break; // no device in this slot at all
                    }
                    continue;
                }
                // the vendor is the low half of word 0x00 and the device the high half; the
                // class and subclass are the top two bytes of word 0x08
                uint32_t class_word = pci_read(bus, slot, function, 0x08);
                pci_device_t dev = {
                    (uint8_t)bus, (uint8_t)slot, (uint8_t)function, (uint16_t)id, (uint16_t)(id >> 16),
                    (uint8_t)(class_word >> 24), (uint8_t)(class_word >> 16)
                };
                if (fn(&dev, ctx)) {
                    return true;
                }
                // a single function device only answers as function 0
                //
                // (Some answer as every function, the same device eight times over, so the
                // other seven are only asked about when the device says there are more:
                // bit 7 of the header type, byte 2 of word 0x0C, which is bit 23 of the word)
                if (function == 0 && !(pci_read(bus, slot, 0, 0x0C) & 0x00800000)) {
                    break;
                }
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

// pci_each calls this for each device: copy the first one of the wanted class out, and stop
static bool match_class (const pci_device_t* dev, void* ctx) {
    find_class_t* want = ctx;
    if (dev->class_code != want->class_code || dev->subclass != want->subclass) {
        return false;
    }
    *want->out = *dev;
    return true;
}

// the first device of this class and subclass (0x04 0x01 is a sound card, say), into out
bool pci_find_class (uint8_t class_code, uint8_t subclass, pci_device_t* out) {
    // pci_each passes one pointer along to match_class, so what it needs goes in one struct
    find_class_t want = { class_code, subclass, out };
    return pci_each(match_class, &want);
}

// the I/O port a base address register gives, or 0 if that BAR is memory instead
uint16_t pci_io_bar (const pci_device_t* dev, int bar) {
    // BAR 0 is at 0x10, BAR 1 at 0x14, and so on
    uint32_t value = pci_read(dev->bus, dev->slot, dev->function, (uint8_t)(0x10 + bar * 4));
    // bit 0 set means I/O ports, the low 2 bits are flags and not part of the address
    return (value & 1) ? (uint16_t)(value & 0xFFFC) : 0;
}

// let a device answer on its I/O ports, and read and write memory by itself (bus mastering,
// which DMA needs)
void pci_enable_io_and_dma (const pci_device_t* dev) {
    // word 0x04 holds the command register in its low half: read it, so only the two bits
    // are added and the device's other settings are kept
    uint32_t command = pci_read(dev->bus, dev->slot, dev->function, 0x04);
    // the top 16 bits are the status register, writing 1s there would clear its flags, so leave them 0
    pci_write(dev, 0x04, (command & 0xFFFF) | PCI_COMMAND_IO | PCI_COMMAND_BUS_MASTER);
}

// the memory address a base address register gives, or 0 if it is I/O ports or a 64 bit one
uint32_t pci_mem_bar (const pci_device_t* dev, int bar) {
    // BAR 0 is at 0x10, BAR 1 at 0x14, and so on
    uint32_t value = pci_read(dev->bus, dev->slot, dev->function, (uint8_t)(0x10 + bar * 4));
    // bit 0 clear means memory; bits 1-2 say how wide the address is (0 = 32 bits), and the low
    // 4 bits are flags, not part of the address
    bool memory = (value & 1) == 0;
    bool bits32 = ((value >> 1) & 3) == 0;
    return memory && bits32 ? (value & 0xFFFFFFF0u) : 0;
}

// let a device answer at its memory address, and read and write memory by itself (bus mastering)
void pci_enable_memory_and_dma (const pci_device_t* dev) {
    // the same as pci_enable_io_and_dma, with the memory bit instead of the I/O one (and again
    // leaving the status register's half 0)
    uint32_t command = pci_read(dev->bus, dev->slot, dev->function, 0x04);
    pci_write(dev, 0x04, (command & 0xFFFF) | PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);
}
