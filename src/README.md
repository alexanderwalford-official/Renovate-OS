# Source Code

## Current Source Code Statistics
- Builds Passing
- Process handling under development.

## Build Instructions On Linux:
- Please check the "Wiki" tab for more information.
- Install the tools once: `make setup-dev`
- Build the bootable ISO: `make` (creates `main.iso`)
- Boot it in QEMU through GRUB: `make run`
- Boot the kernel directly (faster, skips GRUB): `make run-kernel`
- Everything the terminal prints is mirrored to the serial port, so `make run` also shows it in your shell.

## Layout
- `entry.asm` - multiboot header and entry point, calls `main()` in `kernel.c`.
- `lib/` - the C library replacements (`ren_string.h`, `ren_stdlib.h`, `ren_stdio.h`, `ren_ctype.h`), the helper library (`primary_definitions.h`) and the RenLang compiler (`ren_comp.h`).
- `sys/` - interrupts (GDT / IDT / PIC), system clock, process manager, VFS, boot, recovery, power and the terminal.
- `drivers/` - VGA text mode, PS/2 keyboard, serial port, PC speaker and IDE / ATA drive detection.
