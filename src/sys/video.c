/*
    video.c - the video player (see video.h)

    THE PIECES

        the file on disk --fs_read_at--> pl_mpeg (lib/mpg.h) --+--> pictures --> back buffer --> screen
             (64KB at a time)                                   |    (gfx_draw_pixels, gfx_show)
                                                                +--> sound --> mixer stream --> card
                                                                     (made 16 bit at 48000 Hz)

    The file is never read whole: pl_mpeg asks for more through three callbacks (load, seek,
    tell), and each load reads the next READ_CHUNK bytes with fs_read_at. So a video can be far
    bigger than the computer's memory.

    KEEPING PICTURE AND SOUND TOGETHER

    The sound is the clock. Sound can't skip or wait without being heard, but a picture can be
    shown a little late, or not at all, without anyone noticing. So:

        time now = the time of the first sound written + how much of it has been heard

    "How much has been heard" is what the mixer has played, minus what is still queued on the
    sound card (mixer_delay, about 90ms). Each picture carries the time it should be shown at;
    the player shows it once the clock gets there. If decoding falls behind and a picture is
    already more than a frame late, it isn't drawn at all, which saves the time to convert and
    draw it, and the next one has a better chance of being on time. (At least one picture is
    shown every 200ms whatever happens, so a video too big for the computer plays jerkily
    rather than looking frozen. The sound is never affected.)

    A video without sound (or after its sound has ended) uses the system timer as its clock.

    THE MAIN LOOP, once round for each thing that needs doing:

        1. keys: stop, pause, skip, show the controls
        2. sound: keep about a quarter of a second queued in the mixer stream
        3. picture: decode the next one if there isn't one waiting, show it when its time comes
        4. nothing due yet? wait a millisecond

    A finished run prints a line on the serial port with how it went (pictures shown and
    dropped, the average time to decode one), handy for seeing how fast a computer copes.
*/

#include "video.h"
#include "mixer.h"
#include "f_sys.h"
#include "sys_clck.h"
#include "../lib/mpg.h"
#include "../lib/aer_stdlib.h"
#include "../lib/aer_string.h"
#include "../lib/aer_stdio.h"
#include "../drivers/video/gfx.h"
#include "../drivers/input/IO.h"
#include "../drivers/serial/serial.h"

#define READ_CHUNK            (64 * 1024)      // how much of the file is read at a time
#define SOUND_BUFFER_FRAMES   (MIXER_RATE)     // the mixer stream holds up to a second...
#define SOUND_AHEAD_FRAMES    (MIXER_RATE / 4) // ...and is kept a quarter of a second ahead
#define SKIP_SECONDS          5.0              // how far Left and Right jump
#define CONTROLS_MS           3000             // how long the controls stay up after a key

// the most stereo frames one block of sound can turn into: 1152 frames at the lowest MP2 rate
// (32000 Hz) become 1152 * 48000 / 32000 = 1728 at the mixer's rate, plus a little spare
#define MOST_CONVERTED_FRAMES 1800

typedef struct {
    // the file
    fs_file_t file;
    uint32_t file_position;    // where the next load reads from (pl_mpeg's "tell")
    uint8_t* chunk;            // READ_CHUNK bytes, what each load reads into

    // the decoder
    plm_t* plm;
    int width;
    int height;
    double framerate;
    double duration;

    // the picture
    uint32_t* pixels;          // the latest picture as 0x00RRGGBB pixels, width x height
    bool have_picture;         // has anything been decoded into pixels yet?
    int draw_x, draw_y;        // where the picture goes on the screen...
    int draw_w, draw_h;        // ...and how big (as big as fits, keeping its shape)
    plm_frame_t* waiting;      // decoded, waiting for its time to come (NULL: decode another)
    bool video_ended;
    uint32_t last_shown_ms;    // when a picture was last put on the screen (clkuptms)

    // the sound
    bool has_sound;
    int voice;                 // the mixer stream
    uint32_t sample_rate;      // the rate in the file: 32000, 44100 or 48000
    bool sound_ended;
    bool sound_start_known;    // has sound_start been set since the last start or skip?
    double sound_start;        // the video time of the first frame written to the stream
    uint32_t step;             // resampling: input frames per output frame, 16.16 fixed point
    uint32_t step_position;    // resampling: where we are between two input frames, 16.16
    int16_t carry_left;        // resampling: the last input frame of the block before
    int16_t carry_right;

    // the clock when there is no sound
    uint32_t clock_start_ms;
    double clock_start_time;

    // what the user has asked for
    bool paused;
    bool stop;
    uint32_t controls_until;   // show the controls until this time (clkuptms)

    // how it went, for the serial port
    uint32_t shown;
    uint32_t dropped;
    uint32_t decodes;
    uint32_t decode_ms;
} player_t;


// =================================================================================================
//  Reading the file for pl_mpeg
// =================================================================================================

// pl_mpeg wants more of the file: read the next chunk and hand it over.
// At the end of the file (or on a disk error) there is simply nothing to hand over, and the
// decoders then return NULL, which is how the player knows a stream has ended.
// (pl_mpeg's plm_buffer_signal_end() is NOT used here: it marks the end by remembering how much
// the buffer holds at that moment, and the decoder reads the end of the file once when it starts,
// to work out the length. That stale mark then makes later reads stop early, and seeking fails)
static void load_more (plm_buffer_t* buffer, void* user) {
    player_t* p = user;
    int32_t got = fs_read_at(&p->file, p->file_position, p->chunk, READ_CHUNK);
    if (got <= 0) {
        return;
    }
    plm_buffer_write(buffer, p->chunk, (size_t)got);
    p->file_position += (uint32_t)got;
}

// pl_mpeg is seeking: the next load starts from `offset`
static void seek_to (plm_buffer_t* buffer, size_t offset, void* user) {
    (void)buffer;
    player_t* p = user;
    p->file_position = (uint32_t)offset;
}

// pl_mpeg asks how far into the file the loads have got
static size_t tell (plm_buffer_t* buffer, void* user) {
    (void)buffer;
    player_t* p = user;
    return p->file_position;
}


// =================================================================================================
//  The clock
// =================================================================================================

// the video time now, in seconds (see "keeping picture and sound together" at the top)
static double clock_now (player_t* p) {
    if (p->has_sound && p->sound_start_known) {
        uint32_t played = mixer_played(p->voice);
        uint32_t delay = mixer_delay();
        uint32_t heard = 0;
        if (played > delay) {
            heard = played - delay;
        }
        return p->sound_start + (double)heard / MIXER_RATE;
    }
    if (p->has_sound) {
        return p->clock_start_time; // no sound written yet (just started or skipped): wait there
    }
    if (p->paused) {
        return p->clock_start_time;
    }
    return p->clock_start_time + (double)(clkuptms() - p->clock_start_ms) / 1000.0;
}

// start the system timer clock from `time` (no sound, or the sound has run out)
static void restart_timer_clock (player_t* p, double time) {
    p->clock_start_time = time;
    p->clock_start_ms = clkuptms();
}


// =================================================================================================
//  Sound
// =================================================================================================

// a sample from -1 to 1 as a 16 bit number (anything outside that range is cut off)
static int16_t to_16_bit (float sample) {
    if (sample >= 1.0f) {
        return 32767;
    }
    if (sample <= -1.0f) {
        return -32768;
    }
    return (int16_t)(sample * 32767.0f);
}

// Turn one block of decoded sound (1152 stereo frames of floats, at the file's rate) into 16 bit
// stereo at the mixer's rate, in `out`. Returns how many frames that made.
//
// Changing the rate draws a straight line between neighbouring input frames (like resample() in
// sys/sound.c). Blocks are handled one at a time, so the position between input frames and the
// last frame of each block are carried over to the next block, which makes the joins seamless:
//
//     input index 0 = the last frame of the block before (the "carry"), 1.. = this block
static uint32_t convert_sound (player_t* p, const plm_samples_t* samples, int16_t* out) {
    static int16_t input[PLM_AUDIO_SAMPLES_PER_FRAME * 2];
    uint32_t count = samples->count;
    for (uint32_t i = 0; i < count * 2; i++) {
        input[i] = to_16_bit(samples->interleaved[i]);
    }

    uint32_t made = 0;
    while ((p->step_position >> 16) < count && made < MOST_CONVERTED_FRAMES) {
        uint32_t index = p->step_position >> 16;              // the input frame we are past
        int32_t fraction = (int32_t)((p->step_position & 0xFFFF) >> 1); // how far to the next (15 bits)

        // the two input frames either side (index 0 is the carry from the block before)
        int32_t left_a, right_a;
        if (index == 0) {
            left_a = p->carry_left;
            right_a = p->carry_right;
        } else {
            left_a = input[(index - 1) * 2];
            right_a = input[(index - 1) * 2 + 1];
        }
        int32_t left_b = input[index * 2];
        int32_t right_b = input[index * 2 + 1];

        out[made * 2] = (int16_t)(left_a + (((left_b - left_a) * fraction) >> 15));
        out[made * 2 + 1] = (int16_t)(right_a + (((right_b - right_a) * fraction) >> 15));
        made++;
        p->step_position += p->step;
    }

    // carry over to the next block: its index 0 is this block's last frame
    p->step_position -= count << 16;
    p->carry_left = input[(count - 1) * 2];
    p->carry_right = input[(count - 1) * 2 + 1];
    return made;
}

// keep the mixer stream topped up to SOUND_AHEAD_FRAMES, decoding as much sound as that takes
static void feed_sound (player_t* p) {
    static int16_t converted[MOST_CONVERTED_FRAMES * 2];
    if (!p->has_sound || p->sound_ended || p->paused) {
        return;
    }
    while (mixer_stream_queued(p->voice) < SOUND_AHEAD_FRAMES) {
        plm_samples_t* samples = plm_decode_audio(p->plm);
        if (samples == NULL) {
            p->sound_ended = true;
            return;
        }
        if (!p->sound_start_known) {
            // the first sound since the start or a skip: the clock counts from here
            p->sound_start = samples->time;
            p->sound_start_known = true;
        }
        uint32_t made = convert_sound(p, samples, converted);
        mixer_stream_write(p->voice, converted, made);
    }
}

// the sound has played to its end: carry on with the timer as the clock (a sound track can be
// a little shorter than the pictures)
static void check_sound_finished (player_t* p) {
    if (p->has_sound && p->sound_ended && mixer_stream_queued(p->voice) == 0) {
        double time = clock_now(p);
        p->has_sound = false;
        restart_timer_clock(p, time);
    }
}


// =================================================================================================
//  Pictures
// =================================================================================================

// work out where the picture goes: as big as fits the screen, keeping its shape, in the middle
static void fit_picture (player_t* p) {
    int screen_w = (int)gfx_width();
    int screen_h = (int)gfx_height();
    // the wider of "full width" and "full height", whichever still fits
    if (p->width * screen_h <= p->height * screen_w) {
        p->draw_h = screen_h;
        p->draw_w = p->width * screen_h / p->height;
    } else {
        p->draw_w = screen_w;
        p->draw_h = p->height * screen_w / p->width;
    }
    p->draw_x = (screen_w - p->draw_w) / 2;
    p->draw_y = (screen_h - p->draw_h) / 2;
}

// minutes and seconds, like "1:05"
static void format_time (double seconds, char* out, size_t size) {
    if (seconds < 0) {
        seconds = 0;
    }
    uint32_t whole = (uint32_t)seconds;
    sfmt(out, size, "%u:%02u", whole / 60, whole % 60);
}

// the controls along the bottom of the screen: a progress bar and the keys
static void draw_controls (player_t* p) {
    int screen_w = (int)gfx_width();
    int screen_h = (int)gfx_height();
    int bar_y = screen_h - 48;

    gfx_rect(0, bar_y - 8, screen_w, 56, 0x181818);

    // the progress bar, filled up to how far through the video we are
    double position = clock_now(p);
    int bar_w = screen_w - 40;
    int filled = 0;
    if (p->duration > 0) {
        filled = (int)(bar_w * (position / p->duration));
    }
    if (filled > bar_w) {
        filled = bar_w;
    }
    gfx_rect(20, bar_y, bar_w, 6, 0x404040);
    gfx_rect(20, bar_y, filled, 6, 0xFF7F00);

    char now[16], total[16], line[96];
    format_time(position, now, sizeof(now));
    format_time(p->duration, total, sizeof(total));
    const char* state = "";
    if (p->paused) {
        state = "   (paused)";
    }
    sfmt(line, sizeof(line), "%s / %s%s", now, total, state);
    gfx_text(20, bar_y + 16, line, 0xF0F0F0, 1);
    const char* keys = "Space pause   Left / Right 5s   Esc stop";
    gfx_text(screen_w - 20 - (int)slen(keys) * 8, bar_y + 16, keys, 0xFF7F00, 1);
}

// draw the latest picture (and the controls if they are up) and put it all on the screen
static void present (player_t* p) {
    gfx_clear(0x000000); // the black bars either side, and wherever the controls were
    if (p->have_picture) {
        gfx_draw_pixels(p->pixels, p->width, p->height, p->width, p->draw_x, p->draw_y, p->draw_w, p->draw_h);
    }
    if (clkuptms() < p->controls_until) {
        draw_controls(p);
    }
    gfx_show();
}

// turn a decoded picture into pixels and show it
static void show_frame (player_t* p, plm_frame_t* frame) {
    // pl_mpeg writes blue, green, red, alpha bytes, which read as one little endian 32 bit
    // number are 0xAARRGGBB: the 0x00RRGGBB gfx_draw_pixels wants (it ignores the top byte)
    plm_frame_to_bgra(frame, (uint8_t*)p->pixels, p->width * 4);
    p->have_picture = true;
    present(p);
    p->shown++;
}

// decode the next picture, if none is waiting, and keep count of how long that takes
static void decode_next_picture (player_t* p) {
    if (p->waiting != NULL || p->video_ended) {
        return;
    }
    uint32_t before = clkuptms();
    p->waiting = plm_decode_video(p->plm);
    p->decode_ms += clkuptms() - before;
    p->decodes++;
    if (p->waiting == NULL) {
        p->video_ended = true;
    }
}

// Show the waiting picture once its time has come.
// A picture already more than a frame late is skipped (dropped), which saves the time it would
// take to draw and helps the next one be on time. But never for long: if nothing has been shown
// for MOST_DROPPED_MS, the late picture is shown anyway. Otherwise a video slightly too big for
// the computer would drop every picture and look frozen, instead of playing a little jerkily
#define MOST_DROPPED_MS 200

static void show_when_due (player_t* p) {
    if (p->waiting == NULL) {
        return;
    }
    double now = clock_now(p);
    double due = p->waiting->time;
    if (due > now) {
        return; // not yet
    }
    bool late = now - due > 1.0 / p->framerate;
    bool shown_recently = clkuptms() - p->last_shown_ms < MOST_DROPPED_MS;
    if (late && p->have_picture && shown_recently) {
        p->dropped++;
    } else {
        show_frame(p, p->waiting);
        p->last_shown_ms = clkuptms();
    }
    p->waiting = NULL;
}


// =================================================================================================
//  Keys: stop, pause, skip
// =================================================================================================

// plm_seek hands the picture it lands on to the video callback: keep it to show straight away
static void capture_frame (plm_t* plm, plm_frame_t* frame, void* user) {
    (void)plm;
    player_t* p = user;
    plm_frame_to_bgra(frame, (uint8_t*)p->pixels, p->width * 4);
    p->have_picture = true;
}

// jump `seconds` forwards or backwards (to the nearest whole picture before that time)
static void skip (player_t* p, double seconds) {
    double target = clock_now(p) + seconds;
    if (target < 0) {
        target = 0;
    }
    if (p->duration > 0 && target > p->duration - 1) {
        target = p->duration - 1;
    }

    plm_set_video_decode_callback(p->plm, capture_frame, p);
    bool landed = plm_seek(p->plm, target, 0);
    plm_set_video_decode_callback(p->plm, NULL, NULL);
    if (!landed) {
        return;
    }

    // carry on from the new place: forget the picture that was waiting, and start the sound
    // (if there is any) and the clock again from there
    p->waiting = NULL;
    p->video_ended = false;
    if (p->voice >= 0) {
        mixer_stream_clear(p->voice); // drop the sound queued from the old place
        p->has_sound = true;
        p->sound_ended = false;
        p->sound_start_known = false;
        p->step_position = 0;
    }
    restart_timer_clock(p, plm_get_time(p->plm));
    present(p);
}

static void toggle_pause (player_t* p) {
    p->paused = !p->paused;
    if (p->has_sound) {
        mixer_pause(p->voice, p->paused);
    }
    if (p->paused) {
        p->clock_start_time = clock_now(p); // (the timer clock stands still while paused)
        p->controls_until = 0xFFFFFFFF;    // the controls stay up while paused
    } else {
        restart_timer_clock(p, p->clock_start_time);
        p->controls_until = clkuptms() + CONTROLS_MS;
    }
    present(p);
}

static void handle_keys (player_t* p) {
    if (kb_take_break()) {
        p->stop = true; // Ctrl+C
        return;
    }
    while (kb_haschar()) {
        int key = getk();
        if (key == KEY_ESCAPE) {
            p->stop = true;
            return;
        }
        if (key == ' ') {
            toggle_pause(p);
            continue;
        }
        if (!p->paused) {
            p->controls_until = clkuptms() + CONTROLS_MS; // any key shows the controls
        }
        if (key == KEY_LEFT) {
            skip(p, -SKIP_SECONDS);
        } else if (key == KEY_RIGHT) {
            skip(p, SKIP_SECONDS);
        } else {
            present(p);
        }
    }
}


// =================================================================================================
//  Starting, playing and stopping
// =================================================================================================

bool video_is_video (const char* path) {
    fs_file_t file;
    uint8_t start[4];
    if (!fs_open(path, &file)) {
        return false;
    }
    int32_t got = fs_read_at(&file, 0, start, sizeof(start));
    if (got != (int32_t)sizeof(start)) {
        return false; // shorter than 4 bytes: not a video
    }
    return mpg_detect(start, sizeof(start));
}

// give back everything the player took (safe on a half set up player)
static void close_player (player_t* p) {
    if (p->voice >= 0) {
        mixer_stop(p->voice);
    }
    if (p->plm != NULL) {
        plm_destroy(p->plm); // this also frees the buffer it was made with
    }
    free(p->pixels);
    free(p->chunk);
}

// open the file and the decoder, and read the video's size, speed and length
static int open_player (player_t* p, const char* path) {
    if (!fs_open(path, &p->file)) {
        return VIDEO_ERR_NOT_FOUND;
    }
    p->chunk = malloc(READ_CHUNK);
    if (p->chunk == NULL) {
        return VIDEO_ERR_NO_MEMORY;
    }
    mpg_init();
    plm_buffer_t* buffer = plm_buffer_create_with_callbacks(load_more, seek_to, tell, p->file.size, p);
    if (buffer == NULL) {
        return VIDEO_ERR_NO_MEMORY;
    }
    p->plm = plm_create_with_buffer(buffer, 1); // 1: destroying the decoder destroys the buffer
    if (p->plm == NULL) {
        return VIDEO_ERR_NO_MEMORY;
    }
    // read the headers: the picture size, the speed, the sound
    if (!plm_has_headers(p->plm) || plm_get_num_video_streams(p->plm) == 0) {
        return VIDEO_ERR_FORMAT;
    }
    p->width = plm_get_width(p->plm);
    p->height = plm_get_height(p->plm);
    p->framerate = plm_get_framerate(p->plm);
    p->duration = plm_get_duration(p->plm);
    if (p->width <= 0 || p->height <= 0 || p->framerate <= 0) {
        return VIDEO_ERR_FORMAT;
    }
    p->pixels = malloc((size_t)p->width * (size_t)p->height * 4);
    if (p->pixels == NULL) {
        return VIDEO_ERR_NO_MEMORY;
    }
    return VIDEO_OK;
}

// get the sound ready: a mixer stream, and the numbers for changing the rate. Without a sound
// card (or sound in the file) the video plays silently, timed by the system timer
static void open_sound (player_t* p) {
    p->voice = -1;
    p->sample_rate = (uint32_t)plm_get_samplerate(p->plm);
    if (plm_get_num_audio_streams(p->plm) > 0 && p->sample_rate > 0) {
        p->voice = mixer_stream_open(SOUND_BUFFER_FRAMES, MIXER_FULL_VOLUME);
    }
    p->has_sound = p->voice >= 0;
    if (!p->has_sound) {
        plm_set_audio_enabled(p->plm, 0); // don't spend time decoding sound nobody hears
        p->sound_ended = true;
        return;
    }
    p->step = (p->sample_rate << 16) / MIXER_RATE;
}

// a line on the serial port saying how playback went
static void report (player_t* p, const char* path) {
    char line[160];
    uint32_t average = 0;
    if (p->decodes > 0) {
        average = p->decode_ms / p->decodes;
    }
    sfmt(line, sizeof(line), "video: %s %dx%d, %u pictures shown, %u dropped, %u ms to decode one\r\n",
         path, p->width, p->height, p->shown, p->dropped, average);
    serial_write(line);
}

int video_play_file (const char* path) {
    player_t p;
    memset(&p, 0, sizeof(p));
    p.voice = -1;

    int result = open_player(&p, path);
    if (result != VIDEO_OK) {
        close_player(&p);
        return result;
    }
    if (!gfx_begin()) {
        close_player(&p);
        return VIDEO_ERR_NO_SCREEN;
    }
    gfx_buffered(true);
    fit_picture(&p);
    open_sound(&p);
    restart_timer_clock(&p, 0.0);
    p.controls_until = clkuptms() + CONTROLS_MS;

    while (!p.stop) {
        handle_keys(&p);
        if (p.stop) {
            break;
        }
        if (p.paused) {
            sleep_ms(10);
            continue;
        }

        feed_sound(&p);
        decode_next_picture(&p);
        show_when_due(&p);
        check_sound_finished(&p);

        // the controls have just timed out: draw the picture again without them
        if (p.controls_until != 0 && clkuptms() >= p.controls_until) {
            p.controls_until = 0;
            present(&p);
        }

        bool pictures_done = p.video_ended && p.waiting == NULL;
        bool sound_done = !p.has_sound;
        if (pictures_done && sound_done) {
            break;
        }
        // nothing due yet: rest until the next interrupt
        if (p.waiting != NULL || p.video_ended) {
            sleep_ms(1);
        }
    }

    report(&p, path);
    close_player(&p);
    gfx_end();
    return VIDEO_OK;
}

const char* video_strerror (int error) {
    switch (error) {
        case VIDEO_OK:            return "ok";
        case VIDEO_ERR_NOT_FOUND: return "no such file";
        case VIDEO_ERR_FORMAT:    return "not an MPEG-1 video AerOS can play (.mpg)";
        case VIDEO_ERR_NO_SCREEN: return "videos need a graphics screen, and this one is text only";
        default:                  return "not enough memory";
    }
}
