/*
    ac97.c - Intel AC'97 sound card driver (see ac97.h)

    Registers used, as offsets from the two I/O bases:

    mixer (NAM, 16 bit registers)
        0x00  reset (writing anything resets the volumes etc. to their defaults)
        0x02  master volume      0 = loudest, bit 15 = mute
        0x04  headphone volume   the same
        0x18  PCM out volume     0x0808 = no gain or loss
        0x28  extended audio ID, bit 0 = can play rates other than 48000 Hz ("VRA")
        0x2A  extended audio control, bit 0 = turn VRA on
        0x2C  PCM front DAC rate, the sample rate in Hz

    bus master (NABM), the PCM out channel
        0x10  BDBAR  (32 bit) address of the buffer descriptor list
        0x14  CIV    (8 bit)  index of the descriptor being played now
        0x15  LVI    (8 bit)  index of the last descriptor that is ready, the card stops after it
        0x16  SR     (16 bit) status, bit 0 = halted
        0x1B  CR     (8 bit)  control, bit 0 = run, bit 1 = reset this channel
        0x2C  global control (32 bit), bit 1 = take the codec out of reset
        0x30  global status (32 bit), bit 8 = the codec is ready
*/

#include "ac97.h"
#include "../pci/pci.h"
#include "../input/GIO.h"
#include "../../sys/sys_clck.h"
#include "../../lib/ren_stdio.h"

#define NAM_RESET          0x00
#define NAM_MASTER_VOLUME  0x02
#define NAM_HEADPHONE      0x04
#define NAM_PCM_VOLUME     0x18
#define NAM_EXT_ID         0x28
#define NAM_EXT_CONTROL    0x2A
#define NAM_FRONT_RATE     0x2C

#define NABM_PO_BDBAR      0x10
#define NABM_PO_CIV        0x14
#define NABM_PO_LVI        0x15
#define NABM_PO_STATUS     0x16
#define NABM_PO_CONTROL    0x1B
#define NABM_GLOBAL_CTRL   0x2C
#define NABM_GLOBAL_STATUS 0x30

#define CONTROL_RUN        0x01
#define CONTROL_RESET      0x02
#define STATUS_HALTED      0x01
#define STATUS_CLEAR_ALL   0x1E // the flags that are cleared by writing 1 to them

#define DESCRIPTORS        32   // the card's list always has 32 entries
#define CHUNK_FRAMES       4096 // stereo frames per descriptor, about 85ms at 48000 Hz
#define STALL_MS           2000 // no progress for this long means the card has stopped responding

// one buffer descriptor, the layout is fixed by the hardware
typedef struct {
    uint32_t address; // where the samples are (a physical address, the same as ours without paging)
    uint16_t samples; // how many 16 bit samples (so two per stereo frame), at most 0xFFFE
    uint16_t flags;   // bit 14 = play silence if we run out, bit 15 = raise an interrupt (unused)
} __attribute__((packed)) descriptor_t;

#define FLAG_SILENCE_IF_EMPTY 0x4000

bool ac97_ready = false;

static uint16_t nam, nabm; // the two I/O bases
static bool variable_rate = false;
static pci_device_t card;
static descriptor_t list[DESCRIPTORS] __attribute__((aligned(8)));

bool ac97_init (void) {
    if (!pci_find_class(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO, &card)) return false;
    nam = pci_io_bar(&card, 0);
    nabm = pci_io_bar(&card, 1);
    if (nam == 0 || nabm == 0) return false;
    pci_enable_io_and_dma(&card);

    // take the codec out of reset and wait (up to 100ms) for it to say it's ready
    outl(nabm + NABM_GLOBAL_CTRL, 0x02);
    for (int i = 0; i < 100 && !(inl(nabm + NABM_GLOBAL_STATUS) & 0x100); i++) {
        sleep_ms(1);
    }
    outw(nam + NAM_RESET, 0);

    // everything as loud as it goes without distorting, and unmuted
    outw(nam + NAM_MASTER_VOLUME, 0x0000);
    outw(nam + NAM_HEADPHONE, 0x0000);
    outw(nam + NAM_PCM_VOLUME, 0x0808);

    // turn on variable sample rates if the codec has them
    variable_rate = inw(nam + NAM_EXT_ID) & 1;
    if (variable_rate) {
        outw(nam + NAM_EXT_CONTROL, inw(nam + NAM_EXT_CONTROL) | 1);
    }
    ac97_ready = true;
    return true;
}

const char* ac97_name (void) {
    static char name[32];
    snprintf(name, sizeof(name), "AC'97 (PCI %02x:%02x.%u)", card.bus, card.slot, card.function);
    return name;
}

uint32_t ac97_set_rate (uint32_t hz) {
    // codecs handle 8000 to 48000 Hz, anything else has to be converted to 48000 by the caller
    if (hz < 8000 || hz > 48000) hz = 48000;
    if (!variable_rate) return 48000;
    // always written, even for 48000, so the rate a previous sound used doesn't stick
    outw(nam + NAM_FRONT_RATE, (uint16_t)hz);
    return inw(nam + NAM_FRONT_RATE); // the codec may round it to a rate it can do
}

// reset the PCM out channel: stops it and clears its registers
static void reset_channel (void) {
    outb(nabm + NABM_PO_CONTROL, CONTROL_RESET);
    for (int i = 0; i < 100 && (inb(nabm + NABM_PO_CONTROL) & CONTROL_RESET); i++) {
        sleep_ms(1);
    }
}

// point descriptor slot at piece number chunk of the sound
static void fill (int slot, const int16_t* frames, uint32_t count, uint32_t chunk) {
    uint32_t first = chunk * CHUNK_FRAMES;
    uint32_t length = count - first < CHUNK_FRAMES ? count - first : CHUNK_FRAMES;
    list[slot].address = (uint32_t)(uintptr_t)(frames + first * 2);
    list[slot].samples = (uint16_t)(length * 2);
    list[slot].flags = FLAG_SILENCE_IF_EMPTY;
}

bool ac97_play (const int16_t* frames, uint32_t count, bool (*should_stop)(void)) {
    if (!ac97_ready || count == 0) return ac97_ready;
    uint32_t chunks = (count + CHUNK_FRAMES - 1) / CHUNK_FRAMES;

    // queue up the first pieces (all of them if the sound is short)
    uint32_t queued = 0;
    while (queued < chunks && queued < DESCRIPTORS) {
        fill((int)queued, frames, count, queued);
        queued++;
    }
    reset_channel();
    outl(nabm + NABM_PO_BDBAR, (uint32_t)(uintptr_t)list);
    outb(nabm + NABM_PO_LVI, (uint8_t)(queued - 1));
    outw(nabm + NABM_PO_STATUS, STATUS_CLEAR_ALL);
    outb(nabm + NABM_PO_CONTROL, CONTROL_RUN);

    // each time the card moves on from a descriptor, reuse that slot for the next piece and make
    // it the new last valid one. The card stops by itself (halted) after the last piece
    uint8_t done = 0; // the slot the card was playing last time we looked
    uint32_t last_progress = clck_uptime_ms();
    bool ok = true;
    while (!(inw(nabm + NABM_PO_STATUS) & STATUS_HALTED)) {
        uint8_t current = inb(nabm + NABM_PO_CIV) % DESCRIPTORS;
        while (done != current) {
            if (queued < chunks) {
                fill(done, frames, count, queued++);
                outb(nabm + NABM_PO_LVI, done);
            }
            done = (uint8_t)((done + 1) % DESCRIPTORS);
            last_progress = clck_uptime_ms();
        }
        if (should_stop && should_stop()) break;
        if (clck_uptime_ms() - last_progress > STALL_MS) {
            ok = false;
            break;
        }
        sleep_ms(5);
    }
    reset_channel();
    return ok;
}
