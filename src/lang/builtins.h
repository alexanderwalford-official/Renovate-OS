/*
    builtins.h
    What the files holding AerLang's built-in functions share with each other (only lang/ uses it).

    The built-ins are split by topic, one file for each:

        aer_builtins.c   the table of every built-in, aer_builtins[]: the index of everything an
                         AerLang program can call, kept in one piece. Also the constants (KEY_UP,
                         BLACK, PI...), which built-ins need which part of AerOS (for app mode),
                         and the tidying up when a program ends
        bi_args.c        checking and unpacking arguments: int_arg, str_arg, file_arg...
        bi_text.c        print, write, input, strings, conversions (str, int, chr...) and maths
        bi_lists.c       lists and bytes
        bi_files.c       files, encrypted files, la and cwd
        bi_screen.c      the text screen and keys: cls, color, at, key, haskey, big, progress,
                         spinner, frame
        bi_graphics.c    graphics (gopen, rect, image...), gbuf and gshow, keydown, and the mouse
        bi_sound.c       beep, play, sounds that mix (sload, splay, sstop, isply) and video
        bi_system.c      sleep, time, random, sysinfo, queue, cmd, appmode and exit
        bi_network.c     nsend and nrecv

    How a built-in works: it gets its arguments as an array of values, which it must not release
    (the caller still owns them), and hands back a new value that the caller will own. A wrong
    argument is reported with aer_error(), which never returns, so the code after a check can
    trust it.

    Adding a built-in:
      1. write  value_t bi_<name> (int argc, value_t* argv, int line)  in the file for its topic,
         not static (the table that calls it is in aer_builtins.c). Put a comment directly above
         it that starts with how it is called, e.g.  // sqrt(x): the square root of x  as that is
         what WHY shows. Check and unpack the arguments with int_arg / str_arg / list_arg ...
         below; a path goes through file_arg or allowed_path, which check the program may use it
      2. declare it below, with the others from its file
      3. add a line to aer_builtins[] in aer_builtins.c: BUILTIN("name", bi_<name>, fewest args,
         most args) (the interpreter checks the argument count before calling it). If it needs a
         part of AerOS that app mode can put away (graphics, sound, video, files, network), add
         its name to that part's list there too, just above aer_profile
      4. add it to docs/AERLANG.md
*/
#ifndef BUILTINS_H
#define BUILTINS_H

#include "aer_internal.h"
#include "../sys/perm.h"

// ---- argument checks (bi_args.c) ----------------------------------------------------------------
// i is the argument's position (0 is the first), fn the AerLang name, for the error message.
// Strings, lists and bytes come back borrowed: don't aer_release() them

int32_t int_arg (value_t* argv, int i, int line, const char* fn);         // a whole number (a decimal is rounded down)
double real_arg (value_t* argv, int i, int line, const char* fn);         // a number, whole or decimal, as a decimal
aer_object_t* str_arg (value_t* argv, int i, int line, const char* fn);   // a string
aer_object_t* list_arg (value_t* argv, int i, int line, const char* fn);  // a list
aer_object_t* bytes_arg (value_t* argv, int i, int line, const char* fn); // bytes (a picture or a sound)
// a file's path split into dir and name, false if it is no good or the program may not use it
bool file_arg (value_t* argv, int i, int line, const char* fn, perm_kind_t kind, char* dir, char* name);
// a file's path, or NULL if it is no good or the program may not use it
const char* allowed_path (value_t* argv, int i, int line, const char* fn, perm_kind_t kind);

// ---- the built-ins, file by file (the comment above each one says what it does) ---------------

// bi_text.c
value_t bi_print (int argc, value_t* argv, int line);
value_t bi_write (int argc, value_t* argv, int line);
value_t bi_input (int argc, value_t* argv, int line);
value_t bi_input_secret (int argc, value_t* argv, int line);
value_t bi_len (int argc, value_t* argv, int line);
value_t bi_str (int argc, value_t* argv, int line);
value_t bi_int (int argc, value_t* argv, int line);
value_t bi_float (int argc, value_t* argv, int line);
value_t bi_sqrt (int argc, value_t* argv, int line);
value_t bi_sin (int argc, value_t* argv, int line);
value_t bi_cos (int argc, value_t* argv, int line);
value_t bi_atan2 (int argc, value_t* argv, int line);
value_t bi_floor (int argc, value_t* argv, int line);
value_t bi_round (int argc, value_t* argv, int line);
value_t bi_abs (int argc, value_t* argv, int line);
value_t bi_chr (int argc, value_t* argv, int line);
value_t bi_ord (int argc, value_t* argv, int line);
value_t bi_sub (int argc, value_t* argv, int line);
value_t bi_find (int argc, value_t* argv, int line);
value_t bi_upper (int argc, value_t* argv, int line);
value_t bi_lower (int argc, value_t* argv, int line);
value_t bi_split (int argc, value_t* argv, int line);
value_t bi_join (int argc, value_t* argv, int line);

// bi_lists.c
value_t bi_list (int argc, value_t* argv, int line);
value_t bi_push (int argc, value_t* argv, int line);
value_t bi_pop (int argc, value_t* argv, int line);
value_t bi_insert (int argc, value_t* argv, int line);
value_t bi_remove (int argc, value_t* argv, int line);
value_t bi_bytes (int argc, value_t* argv, int line);

// bi_files.c
value_t bi_read_file (int argc, value_t* argv, int line);
value_t bi_read_bytes (int argc, value_t* argv, int line);
value_t bi_write_file (int argc, value_t* argv, int line);
value_t bi_append_file (int argc, value_t* argv, int line);
value_t bi_file_exists (int argc, value_t* argv, int line);
value_t bi_delete_file (int argc, value_t* argv, int line);
value_t bi_cwd (int argc, value_t* argv, int line);
value_t bi_list_files (int argc, value_t* argv, int line);
value_t bi_encrypt_file (int argc, value_t* argv, int line);
value_t bi_decrypt_file (int argc, value_t* argv, int line);
value_t bi_is_encrypted (int argc, value_t* argv, int line);
value_t bi_read_encrypted (int argc, value_t* argv, int line);

// bi_screen.c
value_t bi_cls (int argc, value_t* argv, int line);
value_t bi_color (int argc, value_t* argv, int line);
value_t bi_at (int argc, value_t* argv, int line);
value_t bi_key (int argc, value_t* argv, int line);
value_t bi_haskey (int argc, value_t* argv, int line);
value_t bi_big_text (int argc, value_t* argv, int line);
value_t bi_progress (int argc, value_t* argv, int line);
value_t bi_spinner (int argc, value_t* argv, int line);
value_t bi_frame (int argc, value_t* argv, int line);

// bi_graphics.c
value_t bi_gfx_open (int argc, value_t* argv, int line);
value_t bi_gfx_close (int argc, value_t* argv, int line);
value_t bi_gfx_width (int argc, value_t* argv, int line);
value_t bi_gfx_height (int argc, value_t* argv, int line);
value_t bi_rgb (int argc, value_t* argv, int line);
value_t bi_gfx_clear (int argc, value_t* argv, int line);
value_t bi_pixel (int argc, value_t* argv, int line);
value_t bi_rect (int argc, value_t* argv, int line);
value_t bi_line (int argc, value_t* argv, int line);
value_t bi_text (int argc, value_t* argv, int line);
value_t bi_image (int argc, value_t* argv, int line);
value_t bi_image_size (int argc, value_t* argv, int line);
value_t bi_gbuf (int argc, value_t* argv, int line);
value_t bi_gshow (int argc, value_t* argv, int line);
value_t bi_keydown (int argc, value_t* argv, int line);
value_t bi_mouse (int argc, value_t* argv, int line);
value_t bi_mousemove (int argc, value_t* argv, int line);
value_t bi_mousedown (int argc, value_t* argv, int line);

// bi_sound.c
value_t bi_beep (int argc, value_t* argv, int line);
value_t bi_play (int argc, value_t* argv, int line);
value_t bi_sload (int argc, value_t* argv, int line);
value_t bi_splay (int argc, value_t* argv, int line);
value_t bi_sstop (int argc, value_t* argv, int line);
value_t bi_isply (int argc, value_t* argv, int line);
value_t bi_video (int argc, value_t* argv, int line);

// bi_system.c
value_t bi_sleep (int argc, value_t* argv, int line);
value_t bi_time (int argc, value_t* argv, int line);
value_t bi_random (int argc, value_t* argv, int line);
value_t bi_system_info (int argc, value_t* argv, int line);
value_t bi_queue (int argc, value_t* argv, int line);
value_t bi_cmd (int argc, value_t* argv, int line);
value_t bi_appmode (int argc, value_t* argv, int line);
value_t bi_exit (int argc, value_t* argv, int line);

// bi_network.c
value_t bi_net_send (int argc, value_t* argv, int line);
value_t bi_net_receive (int argc, value_t* argv, int line);

#endif /* BUILTINS_H */
