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

    How ac97_play keeps a long sound going with only 32 descriptors. The sound sits in memory
    as one long array, cut into chunks of CHUNK_FRAMES. The descriptors form a ring:

        descriptor:  0     1     2     3    ...   31
        points at:   c32   c33   c2    c3   ...   c31      (c = chunk of the sound)
                      ^           ^
                 refilled    CIV: the card is playing this one

    The card plays descriptor CIV, moves on to CIV + 1 (wrapping 31 -> 0), and stops after LVI
    (the "last valid index"). Whenever CIV has moved on from a descriptor, the driver points that
    descriptor at the next chunk not yet queued and makes it the new LVI. The card never catches
    up with LVI until the last chunk is queued, then it plays to the end and halts by itself.
    Nothing is copied: the descriptors point straight into the sound's own buffer.

    ac97_start (for the startup sound) skips the refilling: it gives every descriptor the most a
    descriptor can hold (32767 stereo frames) and returns, so up to about 21 seconds plays on its
    own while the OS gets on with booting.
*/

#include "ac97.h"
#include "../pci/pci.h"
#include "../input/GIO.h"
#include "../../sys/sys_clck.h"
#include "../../lib/aer_stdio.h"

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
#define CHUNK_FRAMES       4096 // stereo frames per descriptor in ac97_play, about 85ms at 48000 Hz
#define DESCRIPTOR_MAX_FRAMES (AC97_START_MAX / DESCRIPTORS) // a descriptor holds at most 0xFFFE samples
#define STALL_MS           2000 // no progress for this long means the card has stopped responding

// one buffer descriptor, the layout is fixed by the hardware
typedef struct {
    uint32_t address; // where the samples are (a physical address, the same as ours without paging)
    uint16_t samples; // how many 16 bit samples (so two per stereo frame), at most 0xFFFE
    uint16_t flags;   // bit 14 = play silence if we run out, bit 15 = raise an interrupt (unused)
} __attribute__((packed)) descriptor_t;

#define FLAG_SILENCE_IF_EMPTY 0x4000

bool ac97_ready = false;

static uint16_t nam, nabm;         // the two I/O bases
static bool variable_rate = false; // the codec can play rates other than 48000 Hz
static pci_device_t card;          // where the card is on the PCI bus, for ac97_name
// the descriptor list the card reads by DMA (it must be 8 byte aligned)
static descriptor_t list[DESCRIPTORS] __attribute__((aligned(8)));

// find the AC'97 card on the PCI bus and set it up, see ac97.h
bool ac97_init (void) {
    if (ac97_ready) {
        return true; // already set up (resetting now would cut off a sound that's playing)
    }
    // the first PCI device that calls itself a multimedia audio device
    if (!pci_find_class(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO, &card)) {
        return false;
    }
    // BAR 0 is the mixer's I/O ports, BAR 1 the bus master's (the ones that move the samples)
    nam = pci_io_bar(&card, 0);
    nabm = pci_io_bar(&card, 1);
    if (nam == 0 || nabm == 0) {
        return false;
    }
    // PCI devices start with their I/O ports and DMA switched off, so turn both on
    pci_enable_io_and_dma(&card);

    // take the codec out of reset and wait (up to 100ms) for it to say it's ready
    // (0x02 = global control bit 1, "cold reset off". 0x100 = global status bit 8, "codec ready".
    // If it never says so we carry on anyway: the worst that happens is that nothing is heard)
    outl(nabm + NABM_GLOBAL_CTRL, 0x02);
    for (int i = 0; i < 100 && !(inl(nabm + NABM_GLOBAL_STATUS) & 0x100); i++) {
        sleep_ms(1);
    }
    // put the mixer's registers back to their defaults, whatever the BIOS left in them
    outw(nam + NAM_RESET, 0);

    // everything as loud as it goes without distorting, and unmuted
    // (the volume registers hold an attenuation, how much quieter: 0 is none. In the PCM one each
    // side has 5 bits where 8 means 0 dB, left in the high byte and right in the low one)
    outw(nam + NAM_MASTER_VOLUME, 0x0000);
    outw(nam + NAM_HEADPHONE, 0x0000);
    outw(nam + NAM_PCM_VOLUME, 0x0808);

    // turn on variable sample rates if the codec has them (bit 0 of both registers, see the top
    // of the file). Without VRA the codec only ever plays 48000 Hz
    variable_rate = inw(nam + NAM_EXT_ID) & 1;
    if (variable_rate) {
        outw(nam + NAM_EXT_CONTROL, inw(nam + NAM_EXT_CONTROL) | 1);
    }
    ac97_ready = true;
    return true;
}

// the card's name and where it is on the PCI bus (bus:slot.function), for the boot log.
// The text is kept in a static buffer, so it stays valid after this returns
const char* ac97_name (void) {
    static char name[32];
    sfmt(name, sizeof(name), "AC'97 (PCI %02x:%02x.%u)", card.bus, card.slot, card.function);
    return name;
}

// ask the codec to play at hz, returning the rate it will really play at
uint32_t ac97_set_rate (uint32_t hz) {
    // codecs handle 8000 to 48000 Hz, anything else has to be converted to 48000 by the caller
    if (hz < 8000 || hz > 48000) {
        hz = 48000;
    }
    if (!variable_rate) {
        return 48000;
    }
    // always written, even for 48000, so the rate a previous sound used doesn't stick
    outw(nam + NAM_FRONT_RATE, (uint16_t)hz);
    return inw(nam + NAM_FRONT_RATE); // the codec may round it to a rate it can do
}

// reset the PCM out channel: stops it and clears its registers.
// The card clears the reset bit itself once it is done, so wait (up to 100ms) for that
static void reset_channel (void) {
    outb(nabm + NABM_PO_CONTROL, CONTROL_RESET);
    for (int i = 0; i < 100 && (inb(nabm + NABM_PO_CONTROL) & CONTROL_RESET); i++) {
        sleep_ms(1);
    }
}

// point descriptor slot at piece number chunk of the sound
static void fill (int slot, const int16_t* frames, uint32_t count, uint32_t chunk) {
    // every chunk is CHUNK_FRAMES long except the last, which is whatever is left over
    uint32_t first = chunk * CHUNK_FRAMES;
    uint32_t length = count - first < CHUNK_FRAMES ? count - first : CHUNK_FRAMES;
    // frames holds left and right, so frame number `first` is 2 * first samples in
    list[slot].address = (uint32_t)(uintptr_t)(frames + first * 2);
    list[slot].samples = (uint16_t)(length * 2);
    list[slot].flags = FLAG_SILENCE_IF_EMPTY;
}

// start playing and return straight away, the card carries on by itself
// every descriptor gets as much as it can hold, so the whole list covers AC97_START_MAX frames
uint32_t ac97_start (const int16_t* frames, uint32_t count) {
    if (!ac97_ready || count == 0) {
        return 0;
    }
    ac97_stream_stop(); // the card plays one thing at a time
    reset_channel();
    // cut the sound into the biggest pieces a descriptor takes, until it is all queued or the
    // list is full (anything past AC97_START_MAX frames is simply not played)
    uint32_t queued = 0, slots = 0;
    while (queued < count && slots < DESCRIPTORS) {
        uint32_t length = count - queued < DESCRIPTOR_MAX_FRAMES ? count - queued : DESCRIPTOR_MAX_FRAMES;
        list[slots].address = (uint32_t)(uintptr_t)(frames + queued * 2);
        list[slots].samples = (uint16_t)(length * 2);
        list[slots].flags = FLAG_SILENCE_IF_EMPTY;
        queued += length;
        slots++;
    }
    // tell the card where the list is and which entry is the last, clear any old status flags,
    // then set it running. It halts by itself after the last descriptor
    outl(nabm + NABM_PO_BDBAR, (uint32_t)(uintptr_t)list);
    outb(nabm + NABM_PO_LVI, (uint8_t)(slots - 1));
    outw(nabm + NABM_PO_STATUS, STATUS_CLEAR_ALL);
    outb(nabm + NABM_PO_CONTROL, CONTROL_RUN);
    return queued;
}

// stop whatever the card is playing (a sound, or a stream)
void ac97_stop (void) {
    if (ac97_ready) {
        ac97_stream_stop();
        reset_channel();
    }
}

// ---- streaming ------------------------------------------------------------------------------
//
// ac97_play and ac97_start play a sound that is already decoded, all of it, in memory. A game
// or a video can't do that: the sound is made as it goes (several sounds mixed together, or
// decoded a little at a time). So in streaming mode the driver owns 32 small blocks, one per
// descriptor, and asks a "fill" function for the next block of sound whenever the card is
// getting close to the end of what it has been given:
//
//     descriptor:  0    1    2    3    4    5    6   ...   31
//                       ^    ^------- queued -------^
//                    playing (CIV)              last valid (LVI)
//
// ac97_stream_service() runs from the timer interrupt every couple of milliseconds. It keeps
// STREAM_AHEAD blocks queued after the one playing, filling each with the fill function and
// moving LVI along. Because it runs from the timer, the sound carries on smoothly however long
// the program takes over each frame, without the program having to do anything.
//
// STREAM_AHEAD is the balance between delay and safety: a sound started now is heard after the
// blocks already queued (about 85ms), and the card only runs dry if the timer interrupt is held
// off for that long. If it does run dry, the card halts and the next service starts it again.

#define STREAM_BLOCK_FRAMES 512 // stereo frames per block: about 10.7ms at 48000 Hz
#define STREAM_AHEAD        8   // blocks kept queued after the one playing: about 85ms

// one block of samples per descriptor, 2KB each (left and right, 16 bits). Static, so the card
// can read them by DMA for as long as streaming lasts
static int16_t stream_blocks[DESCRIPTORS][STREAM_BLOCK_FRAMES * 2] __attribute__((aligned(16)));
static ac97_fill_fn stream_fill = NULL;
// volatile: the timer interrupt reads it, and the program side changes it
static volatile bool streaming = false;
static uint8_t stream_next = 0; // the next descriptor to fill

// fill one descriptor's block with the next piece of sound and make it the last valid one.
// The descriptor is set up before LVI moves, so the card never sees it half written
static void stream_queue (uint8_t slot) {
    stream_fill(stream_blocks[slot], STREAM_BLOCK_FRAMES);
    list[slot].address = (uint32_t)(uintptr_t)stream_blocks[slot];
    list[slot].samples = STREAM_BLOCK_FRAMES * 2;
    list[slot].flags = FLAG_SILENCE_IF_EMPTY;
    outb(nabm + NABM_PO_LVI, slot);
}

// how many descriptors there are from `from` up to (not including) `to`, going round the ring.
// Adding DESCRIPTORS first keeps it from going negative when `to` has wrapped past 31 to 0
static uint8_t slots_between (uint8_t from, uint8_t to) {
    return (uint8_t)((to + DESCRIPTORS - from) % DESCRIPTORS);
}

// start streaming: from now on fill is asked for each block of sound, see ac97.h
bool ac97_stream_start (ac97_fill_fn fill) {
    if (!ac97_ready || fill == NULL) {
        return false;
    }
    streaming = false; // keep the timer's service out until everything is set up
    reset_channel();
    // always the same rate, so every program's sound can be mixed together as it is
    ac97_set_rate(AC97_STREAM_RATE);
    stream_fill = fill;

    // queue the first blocks, then start the card on them. stream_next is the descriptor after
    // the last one queued, where the service carries on from
    outl(nabm + NABM_PO_BDBAR, (uint32_t)(uintptr_t)list);
    for (uint8_t slot = 0; slot < STREAM_AHEAD; slot++) {
        stream_queue(slot);
    }
    stream_next = STREAM_AHEAD;
    outw(nabm + NABM_PO_STATUS, STATUS_CLEAR_ALL);
    outb(nabm + NABM_PO_CONTROL, CONTROL_RUN);
    streaming = true;
    return true;
}

// top up the stream, called from the timer interrupt (see the streaming notes above)
void ac97_stream_service (void) {
    if (!streaming) {
        return;
    }
    // CIV only ever counts 0 to 31, the % is just in case a card reports more
    uint8_t playing = inb(nabm + NABM_PO_CIV) % DESCRIPTORS;

    // ran dry: the card played everything it had and halted. Queue some more after the block
    // it stopped on and start it again
    if (inw(nabm + NABM_PO_STATUS) & STATUS_HALTED) {
        outw(nabm + NABM_PO_STATUS, STATUS_CLEAR_ALL);
        stream_next = (uint8_t)((playing + 1) % DESCRIPTORS);
        for (int i = 0; i < STREAM_AHEAD; i++) {
            stream_queue(stream_next);
            stream_next = (uint8_t)((stream_next + 1) % DESCRIPTORS);
        }
        outb(nabm + NABM_PO_CONTROL, CONTROL_RUN);
        return;
    }

    // keep STREAM_AHEAD blocks queued after the one playing. slots_between counts the playing
    // block too, hence <=. Usually this queues one block, or none if the card hasn't moved on
    while (slots_between(playing, stream_next) <= STREAM_AHEAD) {
        stream_queue(stream_next);
        stream_next = (uint8_t)((stream_next + 1) % DESCRIPTORS);
    }
}

// stop streaming. `streaming` goes false before the reset, so a timer interrupt arriving in
// between finds nothing to do instead of starting the card up again
void ac97_stream_stop (void) {
    if (!streaming) {
        return;
    }
    streaming = false;
    reset_channel();
}

// whether the card is streaming right now (something else may have taken it since)
bool ac97_streaming (void) {
    return streaming;
}

// how many frames a sound handed to the fill function now waits before it is heard
uint32_t ac97_stream_delay (void) {
    // the blocks queued ahead of the one playing, plus about half of the one playing
    return STREAM_AHEAD * STREAM_BLOCK_FRAMES + STREAM_BLOCK_FRAMES / 2;
}

// play the whole sound, refilling the ring as the card moves along it (see the top of the file)
bool ac97_play (const int16_t* frames, uint32_t count, bool (*should_stop)(void)) {
    if (!ac97_ready || count == 0) {
        return ac97_ready;
    }
    // how many chunks the sound needs, rounding up so a short last piece still counts
    uint32_t chunks = (count + CHUNK_FRAMES - 1) / CHUNK_FRAMES;
    ac97_stream_stop(); // the card plays one thing at a time

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
    uint32_t last_progress = clkuptms();
    bool ok = true;
    while (!(inw(nabm + NABM_PO_STATUS) & STATUS_HALTED)) {
        uint8_t current = inb(nabm + NABM_PO_CIV) % DESCRIPTORS;
        // catch up with every slot the card has finished since last time (more than one if we
        // were slow to look). Once all chunks are queued the slots are just stepped past
        while (done != current) {
            if (queued < chunks) {
                fill(done, frames, count, queued++);
                outb(nabm + NABM_PO_LVI, done);
            }
            done = (uint8_t)((done + 1) % DESCRIPTORS);
            last_progress = clkuptms();
        }
        // the caller's way out, e.g. a key pressed to stop the sound
        if (should_stop && should_stop()) {
            break;
        }
        // CIV hasn't moved for 2 seconds, longer than any one chunk lasts: the card is stuck,
        // so give up rather than wait for ever
        if (clkuptms() - last_progress > STALL_MS) {
            ok = false;
            break;
        }
        // a chunk lasts about 85ms, so looking every 5ms is far more than enough
        sleep_ms(5);
    }
    // stops the card if we left early, and leaves the channel tidy for the next sound
    reset_channel();
    return ok;
}
