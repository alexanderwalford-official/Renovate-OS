/*
    e1000.c - the Intel PRO/1000 (e1000) network card (see e1000.h)

    HOW THE CARD WORKS

    Its registers are memory mapped: BAR 0 gives an address, and reading or writing the 32 bit
    words there talks to the card (AerOS has no paging, so the address can be used as it is).

    Frames go in and out through two "rings" in ordinary memory, each an array of descriptors
    that the card reads and writes by itself (DMA). A descriptor says where a buffer is, and
    the card fills in how long the frame was and a "done" bit:

        receive ring:   the card fills the buffers  ->  we take frames out of them
        send ring:      we fill the buffers          ->  the card sends them

    Each ring has a head (the card's place) and a tail (ours). The card works from the head up
    to the tail and then waits, so moving the tail on is how we say "there is more":

        receive: descriptors from head to tail are empty buffers it may fill. We hand each one
                 back, by moving the tail onto it, once we have copied its frame out
        send:    descriptors from head to tail are frames to send. We add one and move the
                 tail past it, then wait for its "done" bit

    The rings and buffers (about 84 KB) are allocated by e1000_init, and given back by
    e1000_stop, which app mode does for a program that doesn't use the network (sys/appmode.h).
    Starting again doesn't reset the card (that would drop the link for a while). One thing to
    know: QEMU's e1000 holds arriving frames back for a second after receiving is switched on,
    then hands them over, so just after a restart answers take a second to show up.
*/

#include "e1000.h"
#include "../pci/pci.h"
#include "../../sys/sys_clck.h"
#include "../../lib/aer_stdlib.h"
#include "../../lib/aer_string.h"
#include "../../lib/aer_stdio.h"

// ---- registers (offsets from the start of the card's memory) -------------------------------------

#define REG_CTRL     0x0000  // device control
#define REG_STATUS   0x0008  // device status: bit 1 is "link up"
#define REG_EERD     0x0014  // EEPROM read: where the MAC address is stored, if not in RAL / RAH
#define REG_ICR      0x00C0  // interrupt cause (reading it clears it)
#define REG_IMC      0x00D8  // interrupt mask clear: writing 1s turns interrupts off
#define REG_RCTL     0x0100  // receive control
#define REG_TCTL     0x0400  // transmit control
#define REG_TIPG     0x0410  // the gaps between sent frames
#define REG_RDBAL    0x2800  // receive ring: address (low 32 bits)
#define REG_RDBAH    0x2804  //               address (high 32 bits)
#define REG_RDLEN    0x2808  //               length in bytes
#define REG_RDH      0x2810  //               head (the card's place)
#define REG_RDT      0x2818  //               tail (ours)
#define REG_TDBAL    0x3800  // send ring, the same five
#define REG_TDBAH    0x3804
#define REG_TDLEN    0x3808
#define REG_TDH      0x3810
#define REG_TDT      0x3818
#define REG_MTA      0x5200  // multicast table: 128 words, all 0 (we join no multicast groups)
#define REG_RAL      0x5400  // the MAC address we answer to: low 4 bytes
#define REG_RAH      0x5404  //                               high 2 bytes, and "valid"

#define CTRL_SLU     (1u << 6)     // set link up
#define CTRL_RST     (1u << 26)    // reset the card
#define STATUS_LU    (1u << 1)     // link up
#define RAH_VALID    (1u << 31)    // RAL / RAH hold an address

#define RCTL_EN      (1u << 1)     // receive
#define RCTL_BAM     (1u << 15)    // accept broadcast frames (ARP needs them)
#define RCTL_SECRC   (1u << 26)    // strip the CRC, so frames arrive without it
                                   // (buffer size bits left 0: 2048 byte buffers)
#define TCTL_EN      (1u << 1)     // send
#define TCTL_PSP     (1u << 3)     // pad short frames to the 64 byte minimum
#define TCTL_CT      (0x0Fu << 4)  // collision threshold, the value the manual recommends
#define TCTL_COLD    (0x40u << 12) // collision distance, for full duplex
#define TIPG_VALUE   0x0060200A    // the gaps the manual gives for copper

#define EERD_START   1u            // read EEPROM word: start...
#define EERD_DONE    (1u << 4)     // ...and done (on the 82540, which QEMU is)

// ---- descriptors -----------------------------------------------------------------------------------

typedef struct __attribute__((packed)) {
    uint64_t address;   // the buffer the card fills
    uint16_t length;    // set by the card: the frame's length
    uint16_t checksum;
    uint8_t status;     // bit 0, "done": a frame is in the buffer
    uint8_t errors;
    uint16_t special;
} rx_descriptor_t;

typedef struct __attribute__((packed)) {
    uint64_t address;   // the frame to send
    uint16_t length;
    uint8_t cso;
    uint8_t command;    // what to do with it, see CMD_ below
    uint8_t status;     // bit 0, "done": it has been sent
    uint8_t css;
    uint16_t special;
} tx_descriptor_t;

#define CMD_EOP      (1u << 0)     // end of packet: this buffer is the whole frame
#define CMD_IFCS     (1u << 1)     // add the CRC
#define CMD_RS       (1u << 3)     // report status: set "done" once it is sent
#define DESC_DONE    (1u << 0)

#define RX_COUNT     32            // receive descriptors (the ring must be a multiple of 128 bytes)
#define TX_COUNT     8             // send descriptors
#define BUFFER_SIZE  2048          // one frame each

// ---- the card --------------------------------------------------------------------------------------

static bool ready = false;
static pci_device_t card;
static volatile uint32_t* registers;   // the card's memory
static uint8_t mac[6];

static volatile rx_descriptor_t* rx_ring;
static volatile tx_descriptor_t* tx_ring;
static uint8_t* rx_buffers[RX_COUNT];
static uint8_t* tx_buffers[TX_COUNT];
static int rx_next = 0;                // the next receive descriptor to look at
static int tx_next = 0;                // the next send descriptor to use

static uint32_t read_register (uint32_t offset) {
    return registers[offset / 4];
}

static void write_register (uint32_t offset, uint32_t value) {
    registers[offset / 4] = value;
}

// everything card_memory has allocated, as malloc gave it, so e1000_stop can give it back
static void* allocations[RX_COUNT + TX_COUNT + 2];
static int allocation_count = 0;

// memory that the card reads or writes itself, `align` bytes aligned (malloc's 16 aren't enough
// for the rings, which the manual wants on 128 byte boundaries). Kept until e1000_stop
static void* card_memory (size_t size, size_t align) {
    uint8_t* raw = malloc(size + align);
    if (raw == NULL) {
        return NULL;
    }
    allocations[allocation_count++] = raw;
    uintptr_t aligned = ((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1);
    memset((void*)aligned, 0, size);
    return (void*)aligned;
}

// The cards this driver knows: all of them work the same way for what AerOS needs
static bool is_e1000 (const pci_device_t* dev, void* ctx) {
    static const uint16_t devices[] = {
        0x100E,   // 82540EM, QEMU's and VirtualBox's "Intel PRO/1000 MT Desktop"
        0x100F,   // 82545EM, VirtualBox's "PRO/1000 MT Server"
        0x1004,   // 82543GC, VirtualBox's "PRO/1000 T Server"
    };
    if (dev->vendor != 0x8086) {
        return false;
    }
    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        if (dev->device == devices[i]) {
            *(pci_device_t*)ctx = *dev;
            return true;
        }
    }
    return false;
}

// the MAC address: in RAL / RAH if the card has put it there, otherwise in the EEPROM
static void read_mac (void) {
    uint32_t low = read_register(REG_RAL);
    uint32_t high = read_register(REG_RAH);
    if (high & RAH_VALID) {
        for (int i = 0; i < 4; i++) {
            mac[i] = (uint8_t)(low >> (i * 8));
        }
        mac[4] = (uint8_t)high;
        mac[5] = (uint8_t)(high >> 8);
        return;
    }
    // EEPROM words 0, 1 and 2 hold the six bytes, low byte first
    for (int word = 0; word < 3; word++) {
        write_register(REG_EERD, ((uint32_t)word << 8) | EERD_START);
        uint32_t value = 0;
        for (int tries = 0; tries < 10000 && !((value = read_register(REG_EERD)) & EERD_DONE); tries++) {
        }
        mac[word * 2] = (uint8_t)(value >> 16);
        mac[word * 2 + 1] = (uint8_t)(value >> 24);
    }
    write_register(REG_RAL, mac[0] | (mac[1] << 8) | (mac[2] << 16) | ((uint32_t)mac[3] << 24));
    write_register(REG_RAH, mac[4] | (mac[5] << 8) | RAH_VALID);
}

static bool set_up_receiving (void) {
    rx_ring = card_memory(sizeof(rx_descriptor_t) * RX_COUNT, 128);
    if (rx_ring == NULL) {
        return false;
    }
    for (int i = 0; i < RX_COUNT; i++) {
        rx_buffers[i] = card_memory(BUFFER_SIZE, 16);
        if (rx_buffers[i] == NULL) {
            return false;
        }
        rx_ring[i].address = (uint32_t)(uintptr_t)rx_buffers[i];
        rx_ring[i].status = 0;
    }
    write_register(REG_RDBAL, (uint32_t)(uintptr_t)rx_ring);
    write_register(REG_RDBAH, 0);
    write_register(REG_RDLEN, sizeof(rx_descriptor_t) * RX_COUNT);
    write_register(REG_RDH, 0);
    write_register(REG_RDT, RX_COUNT - 1);   // every buffer but one is the card's to fill
    rx_next = 0;
    write_register(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
    return true;
}

static bool set_up_sending (void) {
    tx_ring = card_memory(sizeof(tx_descriptor_t) * TX_COUNT, 128);
    if (tx_ring == NULL) {
        return false;
    }
    for (int i = 0; i < TX_COUNT; i++) {
        tx_buffers[i] = card_memory(BUFFER_SIZE, 16);
        if (tx_buffers[i] == NULL) {
            return false;
        }
        tx_ring[i].address = (uint32_t)(uintptr_t)tx_buffers[i];
        tx_ring[i].status = DESC_DONE;   // "free to use"
    }
    write_register(REG_TDBAL, (uint32_t)(uintptr_t)tx_ring);
    write_register(REG_TDBAH, 0);
    write_register(REG_TDLEN, sizeof(tx_descriptor_t) * TX_COUNT);
    write_register(REG_TDH, 0);
    write_register(REG_TDT, 0);
    tx_next = 0;
    write_register(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    write_register(REG_TIPG, TIPG_VALUE);
    return true;
}

// the first time: find the card, reset it, and read its MAC address. A restart after
// e1000_stop skips this: a reset would take the link down for a while (half a second in QEMU),
// and anything sent meanwhile would be lost
static bool found = false;

static bool find_and_reset (void) {
    if (!pci_each(is_e1000, &card)) {
        return false;
    }
    uint32_t address = pci_mem_bar(&card, 0);
    if (address == 0) {
        return false;
    }
    pci_enable_memory_and_dma(&card);
    registers = (volatile uint32_t*)(uintptr_t)address;

    // reset it, and wait (up to 10ms) for the reset to finish
    write_register(REG_CTRL, read_register(REG_CTRL) | CTRL_RST);
    for (int i = 0; i < 10 && (read_register(REG_CTRL) & CTRL_RST); i++) {
        sleep_ms(1);
    }
    write_register(REG_IMC, 0xFFFFFFFF);   // no interrupts: AerOS asks the card instead
    read_register(REG_ICR);
    write_register(REG_CTRL, read_register(REG_CTRL) | CTRL_SLU);

    read_mac();
    for (int i = 0; i < 128; i++) {
        write_register(REG_MTA + i * 4, 0);
    }
    return true;
}

bool e1000_init (void) {
    if (ready) {
        return true;
    }
    if (!found) {
        found = find_and_reset();
        if (!found) {
            return false;
        }
    }
    // new rings and buffers (again, after e1000_stop), and sending and receiving switched on
    if (!set_up_receiving() || !set_up_sending()) {
        return false;
    }
    ready = true;
    return true;
}

void e1000_stop (void) {
    if (!ready) {
        return;
    }
    // stop the card using the rings before they go, then give the memory back
    write_register(REG_RCTL, 0);
    write_register(REG_TCTL, 0);
    sleep_ms(1);
    for (int i = 0; i < allocation_count; i++) {
        free(allocations[i]);
    }
    allocation_count = 0;
    ready = false;
}

const char* e1000_name (void) {
    static char name[48];
    sfmt(name, sizeof(name), "Intel e1000 %04x (PCI %02x:%02x.%u)", card.device, card.bus, card.slot, card.function);
    return name;
}

void e1000_mac (uint8_t out[6]) {
    memcpy(out, mac, 6);
}

bool e1000_link_up (void) {
    return ready && (read_register(REG_STATUS) & STATUS_LU);
}

bool e1000_send (const void* frame, size_t length) {
    if (!ready || length > ETHERNET_FRAME_MAX) {
        return false;
    }
    // wait (up to 10ms) for the descriptor to be free: the card has sent what was in it
    volatile tx_descriptor_t* descriptor = &tx_ring[tx_next];
    for (int i = 0; i < 10 && !(descriptor->status & DESC_DONE); i++) {
        sleep_ms(1);
    }
    if (!(descriptor->status & DESC_DONE)) {
        return false;
    }
    memcpy(tx_buffers[tx_next], frame, length);
    descriptor->length = (uint16_t)length;
    descriptor->command = CMD_EOP | CMD_IFCS | CMD_RS;
    descriptor->status = 0;

    // the descriptor must be written before the card is told about it
    __asm__ volatile ("" ::: "memory");
    tx_next = (tx_next + 1) % TX_COUNT;
    write_register(REG_TDT, (uint32_t)tx_next);
    return true;
}

size_t e1000_receive (void* buffer, size_t size) {
    if (!ready) {
        return 0;
    }
    volatile rx_descriptor_t* descriptor = &rx_ring[rx_next];
    if (!(descriptor->status & DESC_DONE)) {
        return 0; // nothing has arrived
    }
    size_t length = descriptor->length;
    bool fits = length <= size;
    if (fits) {
        memcpy(buffer, rx_buffers[rx_next], length);
    }

    // give the buffer back to the card: it becomes the new tail
    descriptor->status = 0;
    __asm__ volatile ("" ::: "memory");
    write_register(REG_RDT, (uint32_t)rx_next);
    rx_next = (rx_next + 1) % RX_COUNT;
    return fits ? length : 0;
}
