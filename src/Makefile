CC = gcc
AS = nasm
LD = ld

# freestanding 32 bit kernel:
#   -fno-stack-protector / -fno-pie / -fcf-protection=none : Ubuntu's gcc turns these on by default, they need runtime support we don't have
#   -mno-sse / -mno-mmx : SSE is not enabled on the CPU, using it would crash
#   -fno-tree-loop-distribute-patterns : stops gcc turning our own memset/memcpy loops into calls to themselves
#   -ffunction-sections / --gc-sections : functions nothing calls are left out of the kernel
CFLAGS = -m32 -march=i686 -std=gnu99 -ffreestanding -fno-builtin -fno-stack-protector \
         -fno-pie -fno-pic -fcf-protection=none -fno-asynchronous-unwind-tables \
         -fno-tree-loop-distribute-patterns -mno-mmx -mno-sse -mno-sse2 \
         -ffunction-sections -fdata-sections -Os -Wall -Wextra -MMD -MP
LDFLAGS = -m elf_i386 -nostdlib -T linker.ld -z noexecstack --gc-sections

# every .c file is compiled on its own into obj/, then linked together
SOURCES = $(wildcard *.c lib/*.c sys/*.c sys/*/*.c drivers/*/*.c lang/*.c)
# the built-in files are linked into the kernel as raw data, and sys/apps.c copies them onto
# the disk when the OS starts: apps/ (the apps and their files) into /bin/, and docs/ (the
# documentation the DOCS command shows) into /docs/
BUILTIN_FILES = $(wildcard apps/* docs/*)
OBJECTS = obj/entry.o $(patsubst %.c,obj/%.o,$(SOURCES)) $(patsubst %,obj/%.o,$(BUILTIN_FILES))

QEMU = qemu-system-i386

# the disk AerOS saves files to: a FAT32 image labelled AEROS (64MB)
# it is kept between builds, "make clean-disk" deletes it
DISK = disk.img
DISK_SECTORS = 131072
QEMU_DISK = -drive file=$(DISK),format=raw,if=ide,index=0

# an AC'97 sound card, heard through PulseAudio (WSLg and most Linux desktops have it)
# "make run AUDIO=none" keeps the card but plays nothing, if PulseAudio isn't there
AUDIO = pa
QEMU_AUDIO = -audiodev $(AUDIO),id=snd0 -device AC97,audiodev=snd0

.PHONY: all clean clean-disk move run run-kernel run-img run-vfs run-uefi disk-ls disk-put setup-dev

all: main.iso

# compile ISO file (final)
main.iso: main.elf iso/boot/grub/grub.cfg
	cp '$<' iso/boot
	grub-mkrescue -o '$@' iso

# multiboot config
main.elf: $(OBJECTS) linker.ld
	$(LD) $(LDFLAGS) -o '$@' $(OBJECTS)

# assembly entry point
obj/entry.o: entry.asm
	@mkdir -p obj
	$(AS) -f elf32 '$<' -o '$@'

# kernel
obj/%.o: %.c
	@mkdir -p '$(dir $@)'
	$(CC) $(CFLAGS) -c '$<' -o '$@'

# a built-in file: "ld -b binary" wraps the file's bytes in an object file, with the symbols
# _binary_<folder>_<name>_start / _end marking where they are (see sys/apps.c)
obj/apps/%.o: apps/%
	@mkdir -p obj/apps
	$(LD) -m elf_i386 -r -b binary -z noexecstack -o '$@' '$<'

obj/docs/%.o: docs/%
	@mkdir -p obj/docs
	$(LD) -m elf_i386 -r -b binary -z noexecstack -o '$@' '$<'

# the MP3 and MPEG decoders include <string.h> and <stdlib.h>: point those at AerOS's own
# versions (lib/shim), for these two files only
obj/lib/mp3.o: CFLAGS += -Ilib/shim
obj/lib/mpg.o: CFLAGS += -Ilib/shim

# header dependencies written by -MMD
-include $(OBJECTS:.o=.d)

# delete old compiled files
clean:
	rm -rf obj *.elf *.o *.iso iso/boot/*.elf

# copy the file to the host
move:
	#sudo rm /home/vboxuser/share/main.iso
	sudo cp main.iso /home/vboxuser/share/

# create the disk (mtools formats it, no root needed)
$(DISK):
	truncate -s $$(( $(DISK_SECTORS) * 512 )) '$@'
	mformat -i '$@' -T $(DISK_SECTORS) -h 16 -s 63 -F -v AEROS ::

clean-disk:
	rm -f $(DISK)

# list every file on the disk
disk-ls: $(DISK)
	mdir -i $(DISK) -/ ::

# copy a file onto the disk, e.g. "make disk-put FILE=hello.aer" (DIR=/bin to put it in a folder)
disk-put: $(DISK)
	mcopy -i $(DISK) -o '$(FILE)' ::$(DIR)/

# boot the ISO through GRUB with the disk attached (terminal output is mirrored to this console via the serial port)
# -boot d starts from the CD, otherwise the BIOS would try the (non bootable) disk first
run: main.iso $(DISK)
	$(QEMU) -cdrom main.iso $(QEMU_DISK) -boot d -smp 1 -serial stdio $(QEMU_AUDIO)

# boot the kernel directly with the disk attached, skipping GRUB
run-kernel: main.elf $(DISK)
	$(QEMU) -kernel main.elf $(QEMU_DISK) -serial stdio $(QEMU_AUDIO)

# boot without a disk (temporary VFS mode)
run-vfs: main.iso
	$(QEMU) -cdrom main.iso -smp 1 -serial stdio $(QEMU_AUDIO)

# boot the ISO like a modern UEFI PC (OVMF firmware). The ISO only contains UEFI boot files when
# grub-efi-amd64-bin is installed (make setup-dev), and the 64 bit firmware needs the 64 bit QEMU
run-uefi: main.iso $(DISK)
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -cdrom main.iso $(QEMU_DISK) -m 256 -serial stdio $(QEMU_AUDIO)

# boot the ISO as a hard drive image
run-img: main.iso
	$(QEMU) -hda main.iso

# install the relevant packages
setup-dev:
	sudo apt install build-essential gcc-multilib nasm xorriso mtools grub-pc-bin grub-efi-amd64-bin grub-common qemu-system-x86 ovmf
