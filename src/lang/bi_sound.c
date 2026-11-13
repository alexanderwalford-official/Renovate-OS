/*
    bi_sound.c
    AerLang's built-in functions for sound and video: a beep on the PC speaker (beep), playing a
    WAV or MP3 file and waiting for it (play), sounds for games that play while the program
    carries on, several at once (sload, splay, sstop, isply, mixed by sys/mixer.c), and playing
    an .mpg video (video).

    How a built-in works, and how to add one, is at the top of builtins.h.
*/

#include "builtins.h"
#include "../lib/aer_string.h"
#include "../lib/aer_stdlib.h"
#include "../drivers/video/gfx.h"
#include "../drivers/input/IO.h"
#include "../drivers/audio/tone.h"
#include "../sys/sound.h"
#include "../sys/mixer.h"
#include "../sys/video.h"

// beep(hz): a beep on the PC speaker, at hz if given (the standard beep without it)
value_t bi_beep (int argc, value_t* argv, int line) {
    // 0 asks tone.h for its standard beep
    beep(argc > 0 ? int_arg(argv, 0, line, "beep") : 0);
    return aer_nil();
}

// play() stops on Ctrl+C, and then the program stops too (like sleep)
static bool play_interrupted;

// asked over and over by the sound player while play() runs: true means stop now.
// kb_take_break() only reports a Ctrl+C once, so the answer is kept in play_interrupted, both
// for later calls and for bi_play to see afterwards
static bool play_should_stop (void) {
    if (kb_take_break()) {
        play_interrupted = true;
    }
    return play_interrupted;
}

// play(path): play a WAV or MP3 file and wait for it to finish, false if it couldn't be played
value_t bi_play (int argc, value_t* argv, int line) {
    (void)argc;
    // a fresh start, a Ctrl+C from an earlier play() doesn't count
    play_interrupted = false;
    const char* path = allowed_path(argv, 0, line, "play", PERM_READ);
    if (path == NULL) {
        return aer_bool(false);
    }
    int result = sound_play_file(path, play_should_stop);
    // stopped by Ctrl+C: end the whole program, not just the sound
    if (play_interrupted) {
        aer_stop(line);
    }
    return aer_bool(result == SOUND_OK);
}

// ---- for games: sounds that mix, and play while the program carries on -------------------------

// sload(path): load a WAV or MP3 file, ready for splay(). Gives back the sound (as bytes), or
// nil if it can't be loaded
value_t bi_sload (int argc, value_t* argv, int line) {
    (void)argc;
    int16_t* frames;
    uint32_t count;
    const char* path = allowed_path(argv, 0, line, "sload", PERM_READ);
    if (path == NULL || sound_load(path, &frames, &count) != SOUND_OK) {
        return aer_nil();
    }
    // count is in frames, and a frame is a left and a right sample of 2 bytes each
    uint32_t size = count * 4; // stereo, 2 bytes a sample
    // copy the sound into AerLang bytes, so the program owns it (and it goes when the program
    // does), then free the copy sound_load made in kernel memory
    value_t sound = aer_bytes(size);
    memcpy(sound.as.obj->data.bytes, frames, size);
    free(frames);
    return sound;
}

// splay(sound, loop, volume): start playing a sound from sload(), and carry on straight away.
// loop (false if left out) plays it round and round; volume runs from 0 to 100 (100 if left
// out). Gives back a number for sstop() and isply(), or -1 if no sound could be started (no
// sound card, or 8 sounds already playing)
value_t bi_splay (int argc, value_t* argv, int line) {
    aer_object_t* sound = bytes_arg(argv, 0, line, "splay");
    bool loop = argc > 1 && aer_truthy(argv[1]);
    int32_t percent = 100;
    if (argc > 2) {
        percent = int_arg(argv, 2, line, "splay");
    }
    // turn 0 to 100 into the mixer's own scale, 0 to MIXER_FULL_VOLUME (multiplied first, so
    // the division doesn't round small percentages down to nothing)
    int volume = (int)(percent * MIXER_FULL_VOLUME / 100);

    // The mixer reads the sound's bytes while it plays, so they must not be freed meanwhile,
    // even if the program lets go of the sound. Holding an extra reference keeps them; they
    // are freed with everything else when the program ends
    aer_retain(argv[0]);
    // the length is given in frames, 4 bytes each (see sload)
    int voice = mixer_play((const int16_t*)sound->data.bytes, sound->length / 4, loop, volume);
    return aer_int(voice);
}

// sstop(voice): stop a sound splay() started. sstop() on its own stops them all
value_t bi_sstop (int argc, value_t* argv, int line) {
    if (argc == 0) {
        mixer_stop_all();
        return aer_nil();
    }
    mixer_stop(int_arg(argv, 0, line, "sstop"));
    return aer_nil();
}

// isply(voice): is that sound still playing?
value_t bi_isply (int argc, value_t* argv, int line) {
    (void)argc;
    return aer_bool(mixer_playing(int_arg(argv, 0, line, "isply")));
}

// ---- video -------------------------------------------------------------------------------------

// video(path): play an .mpg video full screen with its sound, until it ends or Esc is pressed
// (Space pauses, Left / Right skip 5 seconds). Gives back true if it played. A video takes the
// whole screen and hands it back to the terminal at the end, so a program drawing graphics calls
// gopen() again afterwards
value_t bi_video (int argc, value_t* argv, int line) {
    (void)argc;
    const char* path = allowed_path(argv, 0, line, "video", PERM_READ);
    if (path == NULL) {
        return aer_bool(false);
    }
    // the video player takes the screen over itself, so close the program's graphics first
    // (and say so in aer->graphics, so drawing afterwards needs a new gopen())
    if (aer->graphics) {
        gfx_end();
        aer->graphics = false;
    }
    return aer_bool(video_play_file(path) == VIDEO_OK);
}
