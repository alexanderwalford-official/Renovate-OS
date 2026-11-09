/*
    pci.h
    Finding devices on the PCI bus.

    Every PCI device (sound card, network card, disk controller...) has a small block of
    "configuration space" that says what it is and where its registers are. It is read through
    two I/O ports: write the device's address to 0xCF8, then read or write 0xCFC.

    A device is found by its class code, e.g. class 0x04 subclass 0x01 is an audio device.

    https://wiki.osdev.org/PCI
*/
#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <stdbool.h>

#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_SUBCLASS_AUDIO   0x01 // AC'97 style audio (0x03 would be HD Audio)

typedef struct {
    uint8_t bus, slot, function;  // where it is
    uint16_t vendor, device;      // who made it and which product (e.g. 8086:2415, Intel AC'97)
    uint8_t class_code, subclass; // what kind of device it is (e.g. 04:01, audio)
} pci_device_t;

// call fn for every device on the bus, stopping early if it returns true (and then returning true)
bool pci_each (bool (*fn)(const pci_device_t* dev, void* ctx), void* ctx);

// find the first device of this class and subclass, false if there is none
bool pci_find_class (uint8_t class_code, uint8_t subclass, pci_device_t* out);

// the I/O port a device's base address register (BAR 0 to 5) points at, 0 if it isn't an I/O BAR
uint16_t pci_io_bar (const pci_device_t* dev, int bar);

// let the device answer on its I/O ports and read / write memory by itself (bus mastering, for DMA)
void pci_enable_io_and_dma (const pci_device_t* dev);

#endif /* PCI_H */
