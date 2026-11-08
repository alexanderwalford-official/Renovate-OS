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
- Boot without a disk (temporary VFS mode): `make run-vfs`
- Everything the terminal prints is mirrored to the serial port, so `make run` also shows it in your shell.

## The Disk
- `make run` attaches `disk.img`, a 64MB FAT32 disk labelled `RENOVATE` (created on first use, kept between builds).
- Renovate OS only saves to a FAT16 / FAT32 volume labelled `RENOVATE`, so it never writes to someone's own drives.
  On the first boot it creates the system folders (`/acc`, `/bin`, `/sys` ...) and `conf.ren`.
- File names created on the OS must be 8.3 (e.g. `notes.txt`). Long names copied from Windows / Linux can be read.
- Copy a file onto the disk: `make disk-put FILE=hello.ren` (add `DIR=/bin` for a folder), list it: `make disk-ls`.
- Start again with an empty disk: `make clean-disk`.
- To try the OS without touching any disk, press X on the splash screen and pick `[3] Demo mode`.

## Layout
- `entry.asm` - multiboot header and entry point, calls `main()` in `kernel.c`.
- `lib/` - the C library replacements (`ren_string.h`, `ren_stdlib.h`, `ren_stdio.h`, `ren_ctype.h`), the helper library (`primary_definitions.h`) and the RenLang compiler (`ren_comp.h`).
- `sys/` - interrupts (GDT / IDT / PIC), system clock, process manager, VFS, boot, recovery, power and the terminal.
- `drivers/` - VGA text mode, PS/2 keyboard, serial port, PC speaker and IDE / ATA drives (detection, sector read / write, partitions).
- `sys/fat.h` - the FAT16 / FAT32 driver, `sys/f_sys.h` picks the disk or the temporary VFS.
