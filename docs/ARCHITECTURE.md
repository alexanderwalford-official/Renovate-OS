# How AerOS works

A guide to the whole system: what each part does, how a boot unfolds, where everything lives
in memory, and how to change things. Every source file starts with a comment explaining itself
in more detail. This document is the map that ties them together.

The other guides go deeper into one area each:

* [DIFFERENT.md](DIFFERENT.md): what sets AerOS apart (WHY and SRC, AerLang at the prompt,
  apps that ask first, files that remember), and how each one works
* [AERLANG.md](AERLANG.md): the AerLang programming language and its built-in functions
* [SECURITY.md](SECURITY.md): user accounts, password hashing and file encryption
* [AUDIO.md](AUDIO.md): the sound card driver, WAV / MP3 playback and the mixer
* [VIDEO.md](VIDEO.md): playing MPEG-1 videos, and keeping picture and sound together

## The idea

AerOS is meant to be **small enough to understand completely** and **fast because it does so
little**. It leaves out the machinery a general-purpose OS needs for many programs and many
users at once:

| Left out | What AerOS does instead | What that buys |
| --- | --- | --- |
| paging / virtual memory | every pointer is a physical address | no page tables, no TLB misses, nothing to set up |
| multitasking and context switches | one thing runs at a time, to completion | no scheduler overhead, no locks |
| a kernel / user split | everything runs in ring 0 | no system call overhead, drivers are ordinary functions |
| loadable drivers and modules | every driver is compiled in | the whole OS is one ~1.8 MB file (half of it its own source code, for `/src/`) |
| a C library | `src/lib` has just what the kernel uses | no unused code (`--gc-sections` removes the rest) |

The cost is just as clear. A bug anywhere can crash everything, and there is no protection
between programs. AerOS treats this as a fair trade for a system meant to be read, learned
from and modified.

Four things set it apart from the usual design (each is described in full in
[DIFFERENT.md](DIFFERENT.md)):

| Idea | What you see | The code |
| --- | --- | --- |
| it explains itself | `why` after a problem, `why del`, `src del`, the read-only `/src/` folder | `sys/explain.c`, `sys/source.c` |
| the prompt is AerLang | `let x = 2`, `x * 10`, `cmd("la")` at the terminal's prompt | "The shell" in `lang/aer_interp.c` |
| apps ask first | "viewer.aer wants to read files in /home/alex/" | `sys/perm.c` |
| files remember | `vers notes.txt`, `rd notes.txt@2`, `undo notes.txt` | `sys/versions.c` |

## The source tree

```
src/
  entry.asm             multiboot header (asks GRUB for 1024x768x32), sets up the stack, calls main()
  kernel.c              main(): starts the hardware, then runs the boot processes in order
  linker.ld             puts the kernel at 1 MB, header first
  Makefile              builds main.elf and main.iso, runs QEMU (make run / run-vfs / run-uefi)

  drivers/              talking to hardware
    input/    IO.c        PS/2 keyboard: scancodes -> ring buffer -> getk / getln / GetSecret
              GIO.h       inb / outb port I/O and other one-instruction helpers
    video/    VGA_linear.c  the text console: VGA text mode, or 80x25 cells drawn on a framebuffer
              gfx.c       pixels, rectangles, lines, text, BMP pictures and scaled pixel blocks for
                          apps, drawn straight to the screen or into a back buffer (gfx_show)
              bga.c       changing resolution on QEMU / Bochs / VirtualBox graphics (RES command)
              font8x16.c  the console font
    storage/  ata.c       IDE / ATA disks, read and write sectors (PIO)
    pci/      pci.c       finding devices on the PCI bus
    audio/    ac97.c      the AC'97 sound card (whole sounds, or a stream topped up from the timer)
              tone.c      the PC speaker (BP)
    serial/   serial.c    COM1: everything printed on screen is copied here, for debugging

  sys/                  the operating system proper
    boot.c                reads GRUB's multiboot info, prints the boot log
    interrupts.c          GDT, IDT, PIC remapping, exception and IRQ dispatch
    sys_clck.c            the PIT timer (1000 Hz) and the CMOS real time clock
    p_man.c               the process manager (a queue of one-line jobs)
    f_sys.c               the file system front end: DISK mode or the in-memory VFS
    fat.c                 the FAT16 / FAT32 driver
    source.c              the read-only /src/ folder: AerOS's own source, built into it
    versions.c            old versions of the files in home folders (VERS, UNDO, notes.txt@2)
    perm.c                programs ask before they touch your files (PERM)
    explain.c             WHY and SRC: the last problem, its explanation, and reading the source
    fcrypt.c              password protected files
    sound.c               WAV / MP3 decoding and playback
    mixer.c               up to 8 sounds at once, in the background (games, a video's sound track)
    video.c               the .mpg video player (PLY, AerLang's video())
    sysinfo.c             everything known about the computer (CPU, memory, disks, PCI), for SYSINFO
    ui.c                  progress bars and spinners for long jobs (and AerLang's progress / spinner)
    apps.c                the built-in apps and files, copied to the disk at boot
    splash.c              the orange boot screen
    recovery.c            the boot options menu (press X on the splash)
    power.c               reboot and shut down
    accounts/auth.c       user accounts and the login screen
    terminal/t_int.c      the terminal and all its commands
    terminal/editor.c     the text editor (EDT)

  lib/                  the kernel's own small C library
    aer_string.c          memcpy, slen, strtok_r, sdist  aer_stdlib.c  malloc / free, itoa, qsort...
    aer_stdio.c           prtf, sfmt                 aer_ctype.h   isdigit, toupper...
    aer_setjmp.c          setjmp / longjmp (AerLang errors) primary_definitions.c  seq, split, trim
    crypto.c              SHA-256, HMAC, PBKDF2, AES-256, random numbers
    busy.c                busy(): long loops call it so a spinner can turn (see sys/ui.h)
    mp3.c + minimp3/      MP3 decoding
    mpg.c + pl_mpeg/      MPEG-1 video and MP2 sound decoding (pl_mpeg, MIT)
    fpu.c                 switches the x87 FPU on, for the decoders' floats
    shim/                 <string.h> and <stdlib.h> for the decoders, pointing at the files above
    aer_comp.c            runs a process's line of code (the built-in "instructions")

  lang/                 the AerLang interpreter
    aer_lexer.c           source text -> tokens
    aer_parser.c          tokens -> syntax tree
    aer_interp.c          walks the tree; memory, values, variables; the shell's session
    aer_builtins.c        print, rdf, play, gfx_... (every function a program can call)

  apps/                 built-in apps and files, linked into the kernel and copied to /bin/ or /sys/
  docs/                 the in-OS manual (Markdown and HTML), copied to /docs/ and shown by DOC
  iso/boot/grub/grub.cfg  GRUB's menu: loads main.elf, restores the saved screen resolution
```

## A boot, step by step

```mermaid
sequenceDiagram
    participant G as GRUB
    participant E as entry.asm
    participant K as kernel.c main()
    participant P as processes
    G->>E: load main.elf at 1 MB, set the 1024x768 framebuffer, jump to loader
    E->>K: stack set up, eax = magic, ebx = multiboot info
    K->>K: read multiboot info (memory map, framebuffer)
    K->>K: screen, serial, GDT, IDT + PIC, timer (IRQ0), keyboard (IRQ1)
    K->>K: sti: interrupts on, check the timer really ticks
    K->>P: "splash_screen": logo, startup sound, 4 s to press X
    P-->>K: X pressed? -> boot options (normal, fast, demo, reboot, off)
    K->>P: "init_boot": boot log, crypto self-test, sound card, file system, built-in files
    K->>P: "terminal": login (DISK mode), then the command loop, until RBT / OFF
```

Each step after the hardware set-up is a **process**: a line of code (`"splash_screen"`,
`"init_boot"`, `"terminal"`) put on the process stack and handed to `compile()` in
`lib/aer_comp.c`, which knows what each name means. Processes run one at a time to completion,
shortest first. The terminal's `RUN` command creates processes the same way.

Since nothing runs side by side, programs can instead be **queued** to run one after another:
`QUE <program>` (or AerLang's `queue()`) puts a process on the stack as WAITING, and `GO`
runs the waiting ones in the order they were queued, each to completion. Ctrl+C in one of them
stops the rest of the queue as well. See "The queue" in `sys/p_man.h`.

## Memory

There is no paging, so these are the real physical addresses:

```
0x00000000  +---------------------------+
            | BIOS data, real mode IVT  |  left alone
0x000B8000  | VGA text buffer           |  the console in text mode (80 x 25 x 2 bytes)
0x00100000  +---------------------------+  1 MB
            | .text   code              |
            | .rodata constants         |  the kernel, about 430 KB
            | .data   globals           |
            | .bss    256 KB stack      |
            |         4 MB heap pool    |
            +---------------------------+  end of the kernel
            | free RAM                  |  the biggest free stretch in GRUB's memory map is
            |   (added to the heap)     |  handed to heap_add_region() at boot
            +---------------------------+  end of RAM
   ...
0xFD000000  | framebuffer (typically)   |  wherever GRUB says, used as a plain pointer
            +---------------------------+
```

* **The stack** (256 KB) is shared by everything, including interrupt handlers. Code that might
  recurse deeply (the AerLang parser and interpreter) checks how much is left.
* **The heap** (`malloc` / `free` in `lib/aer_stdlib.c`) is "next fit" with a 16-byte header
  per block. `malloc` carries on searching from where it last allocated, and `free` merges a
  block with its free neighbours in constant time. It starts with the 4 MB pool inside the kernel and grows by the free
  RAM found at boot, about 250 MB on a 256 MB machine.
* **DMA** (the sound card) works with ordinary heap pointers, because without paging every
  address the CPU sees is the address the hardware sees.

## Interrupts

| Vector | Source | Handler |
| --- | --- | --- |
| 0-31 | CPU exceptions (divide by zero, page fault...) | the red panic screen, with the exception's name |
| 32 (IRQ0) | PIT timer, 1000 times a second | counts milliseconds, adds entropy for the random generator |
| 33 (IRQ1) | PS/2 keyboard | scancode -> character -> ring buffer, adds entropy, Ctrl+C sets a break flag |

Waiting is done with `hlt` until the next interrupt (`sleep_ms`), so an idle AerOS uses almost
no CPU. If the firmware has switched the PIT off, `clck_verify()` notices at boot and every wait
becomes a busy loop instead (about a millisecond per 1000 port writes, so timing is rough).

## Files

`src/sys/f_sys.c` is the only file system interface the rest of the OS uses. At boot it picks
one of two modes:

| Mode | When | Files go to |
| --- | --- | --- |
| DISK | an IDE disk with a FAT16 / FAT32 volume labelled `AEROS` is found | the disk (`fat.c`) |
| VFS | no such disk, or DEMO mode chosen in the boot options | memory, lost at power off |

Paths are always absolute directories ending in `/` plus a name (`"/sys/"` + `"conf.aer"`).
Users can type relative paths with `.` and `..`, and `fs_split_path` / `fs_resolve_dir` turn
them into that form. On disk, new names must fit 8.3 (`NOTES.TXT`). Existing long names can be
read.

Each user's home folder is `/home/<name>/`, made with the account. The folders made at every
boot are `/acc/` (accounts, and `perms`, what programs may always do), `/bin/` (apps), `/docs/`
(the manual), `/sys/` (system files: `startup.wav`, `cache/`, `tmp/`) and `/conf.aer`.

Three things are layered on top, all inside `f_sys.c`'s public functions, so every command
and every AerLang function gets them without doing anything:

* **`/src/`** is neither on the disk nor in memory: each function checks `source_in()` first
  and hands the path to `sys/source.c`, which serves AerOS's own source files from the kernel.
  Anything that would change one gets `FS_ERR_READ_ONLY`.
* **Versions**: before a file in a home folder is changed or deleted, `versions_keep()`
  (`sys/versions.c`) copies it into `/home/<name>/versions/`. A name like `notes.txt@2` that
  isn't a real file is read from there instead. Writing with `wipe_old` (encryption) deletes
  a file's versions, and `fs_empty_dir` pauses versions while it works.
* **Permissions** aren't in `f_sys.c`: they are checked one level up, by AerLang's file
  functions, so the terminal (which is you) never asks. See `sys/perm.h`.

## The manual inside AerOS

`src/docs/` holds the manual that ships inside the OS: short Markdown pages written for an
80-column screen, plus an HTML sample. They are linked into the kernel like the apps and
copied to `/docs/` at boot. Each page is **replaced whenever it differs** from the built-in
copy, so the manual always matches the version that is running.

The apps are more careful, because they are yours to change. `/sys/apps.lst` records the
SHA-256 of each app as AerOS installed it. At boot an app on the disk that still matches its
record is replaced by a newer built-in version, and one that doesn't (you changed it) is kept.
Apps installed before the record existed are kept too, since there is no way to tell. See
the top of `sys/apps.c`.

`DOC [topic]` opens it in the reader, `/bin/read.aer`. The reader is an AerLang app that shows
Markdown, HTML and plain text with colours, word wrapping, tables and links (Tab picks a link,
Enter opens it, Backspace goes back). To change a page, edit it in `src/docs/` and rebuild. To
add one, also add it to the table in `src/sys/apps.c` and link to it from `index.md`.

## The terminal

`cli_init()` in `sys/terminal/t_int.c` is the main loop of the running system:

```
loop forever:
    auth_login()          (DISK mode only), then start the shell's AerLang session
    draw the banner
    until OUT (log out):
        prompt "alex@0//: "  ->  read a line  ->  the first word is:
                                                    a command in commands[]    run it
                                                    an app, /bin/<word>.aer    run that
                                                    anything that looks like code: AerLang
                                                    anything else: "Did you mean DEL?"
```

Everything about a command's name comes from that one table: `HLP` (help) lists it, Tab
completion offers it, its "Usage:" message is built from it, and `WHY` / `SRC` find its code
through it (each row keeps its function's name as a string, see the `COMMAND` macro). So a new
command only needs one table line, and renaming a command means changing one word. Text in
other files that names a command (`sys/explain.c`) writes `@cli_list` and gets the current
name from `cli_command_called()`.

Commands report problems with `explain_say()` / `explain_say_fs()` (`sys/explain.h`) rather
than a plain `prtf`: the message looks the same, but AerOS remembers it, so `WHY` can explain
it afterwards. The line is read with
`GetLine()` (in `drivers/input/IO.c`), which hands Tab to `cli_complete()`. That completes
the first word from the command table and the `.aer` programs in `/bin/` and the current
folder, and later words from the file system.

## AerLang

```
source text --aer_lexer.c--> tokens --aer_parser.c--> syntax tree --aer_interp.c--> runs it
                                                                        |
                                                   aer_builtins.c: print, rdf, play...
```

The interpreter tracks every allocation, so when a program ends, normally or through an error,
everything it used is freed at once. Errors `longjmp` straight back to the start of the run.
See [AERLANG.md](AERLANG.md) for the language and the comments at the top of each `lang/` file
for the details.

The terminal's prompt has an AerLang session of its own, "the shell", which lasts from logging
in to logging out. Only one session can be the running one, so when a program starts, the
shell's session is copied aside and copied back when the program ends ("The shell" in
`aer_interp.c`). Programs go through `aer_run_file()`, which also tells `sys/perm.c` which
program is running, so its file functions ask before they touch your files.

## Building and testing

```bash
cd src
make            # builds main.elf and main.iso (needs gcc-multilib, nasm, grub, mtools: make setup-dev)
make run        # boots the ISO in QEMU with a 64 MB AEROS disk (disk.img) and an AC'97 sound card
make run-vfs    # without a disk
make run-uefi   # as a UEFI PC (OVMF)
make disk-put FILE=photo.bmp DIR=/bin   # copy a file onto the disk
```

The kernel is built with `-Os -ffunction-sections -fdata-sections` and linked with
`--gc-sections`, so any function nothing calls is left out. There are no warnings, and new code
should keep it that way. Useful QEMU options while testing:

* `-kernel main.elf` boots without GRUB (faster, and the screen stays in VGA text mode, so its
  text can be read from memory at `0xB8000` with the QEMU monitor's `pmemsave`)
* `-serial stdio` shows everything printed on screen in your terminal
* `-audiodev wav,id=snd0,path=out.wav -device AC97,audiodev=snd0` records the sound card to a file

## Recipes

### Add a terminal command

In `src/sys/terminal/t_int.c`:

```c
// VER: print the version
static void cli_version (char* arguments) {
    (void)arguments; // VER takes no arguments
    prtf("AerOS " AEROS_VERSION "\n");
}
```

and one line in `commands[]`: the name you type, what goes after it (for help and the "Usage:"
message, "" if nothing), the description help shows, and the function:

```c
    COMMAND("ver",  "",           "PRINT THE AEROS VERSION",                                    cli_version),
```

A command that needs something after it can call `show_usage(NULL)` when it is missing, which
prints e.g. `Usage: VER` built from the table. When something goes wrong, say so with
`explain_say(PROBLEM_..., subject, "Couldn't ...")`, or `explain_say_fs(result, subject,
"Couldn't ...")` for a file system result code, so `WHY` can explain it. The comment above the
function is what `WHY VER` shows, so write it for the person typing the command.

### Explain a new kind of problem

Add a `PROBLEM_` to the list in `src/sys/explain.h`, and its explanation to the table in
`src/sys/explain.c`: what happened, why AerOS works that way, what to do about it, and the C
function that makes the decision (for `SRC`). Write commands as `@cli_function` so they are
named whatever the command is called today.

### Add an AerLang function

In `src/lang/aer_builtins.c`, write the function in the right section:

```c
// twice(n): n times two
static value_t bi_twice (int argc, value_t* argv, int line) {
    (void)argc;
    return aer_int(int_arg(argv, 0, line, "twice") * 2);  // int_arg reports a wrong type for you
}
```

then add `BUILTIN("twice", bi_twice, 1, 1),` to `aer_builtins[]` (name, function, fewest and
most arguments), and list it in [AERLANG.md](AERLANG.md). The comment above it, starting with
how it is called, is what `WHY TWICE` shows.

A function that takes a file's path gets it with `file_arg(..., PERM_READ or PERM_CHANGE, ...)`
(or `allowed_path`), which asks the person at the keyboard if the program needs permission,
and fails the same way as a missing file if the answer is no.

### Add a built-in app or file

Put the file in `src/apps/` (or `src/docs/` for a manual page), then add two lines to
`src/sys/apps.c`: `EMBEDDED(apps_name_ext)` and a `FILE_ENTRY` saying which folder it goes in
and whether to refresh it when it changes. The Makefile links it into the kernel automatically.
On a disk, new names must fit 8.3 (`sample.htm`, not `sample.html`).

### Add a driver

1. Put it in `src/drivers/<kind>/` (any `.c` file there is built automatically).
2. For a PCI card, find it with `pci_find_class()`, read its ports with `pci_io_bar()`, and call
   `pci_enable_io_and_dma()`.
3. Call its init function from `kernel.c` (if it needs to run before interrupts are on) or from
   `init_boot()` in `sys/boot.c`, and print a `[ OK ]` / `[WARN]` line in the boot log.
4. If it needs an interrupt, install a handler with `irq_install_handler(n, fn)` and
   `pic_unmask(n)` (see the timer in `sys/sys_clck.c`).

### Style

* Every file starts with a comment saying what it is for and how it works.
* Every function has a comment right above it (no blank line in between): `WHY` reads them,
  so they are the explanation people see inside AerOS. A function's definition starts at
  column 0 with its return type, `int fs_list (...) {`, which is how `SRC` finds it.
* Comments explain **why**, and anything that isn't obvious from the code: the hardware's rules,
  the edge cases, the reasons for a choice.
* Functions return a value or an `*_OK` / `*_ERR_*` code with a matching `*_strerror()` message.
  There are no hidden globals for errors.
* Secrets are wiped with `wipe()` as soon as they're no longer needed.
* Builds must stay free of warnings.
