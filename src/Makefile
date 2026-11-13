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
SOURCES = $(wildcard *.c lib/*.c sys/*.c sys/*/*.c drivers/*/*.c lang/*.c db/*.c)
# the built-in files: everything in disk/ is linked into the kernel as raw data, and sys/apps.c
# copies it to the same place in AerOS when it starts. disk/ is laid out just like AerOS's own
# folders, so disk/bin/hello.aer is /bin/hello.aer and disk/docs/index.md is /docs/index.md.
# Adding a file there is all it takes: obj/builtin_index.c (below) lists them for apps.c
BUILTIN_FILES = $(sort $(shell find disk -type f))

# AerOS's own source code is linked in too, the same way, and shows up as the read-only folder
# /src/ (see sys/source.h): every .c and .h file AerOS was built from, plus the files that
# build and boot it. Left out: other people's code (lib/minimp3, lib/pl_mpeg, which the
# wildcards don't reach) and two big files that are only data (the logo and the font)
HEADERS = $(wildcard *.h lib/*.h lib/shim/*.h sys/*.h sys/*/*.h drivers/*/*.h lang/*.h db/*.h)
SOURCE_CODE = $(sort $(filter-out sys/splash_logo.c drivers/video/font8x16.c, \
                $(SOURCES) $(HEADERS)) entry.asm linker.ld Makefile iso/boot/grub/grub.cfg)
SOURCE_OBJECTS = $(patsubst %,obj/source/%.o,$(SOURCE_CODE)) obj/source_index.o

OBJECTS = obj/entry.o $(patsubst %.c,obj/%.o,$(SOURCES)) $(patsubst %,obj/%.o,$(BUILTIN_FILES)) \
          obj/builtin_index.o $(SOURCE_OBJECTS)

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

# an Intel e1000 network card on QEMU's own little network, where this computer is 10.0.2.2.
# UDP port 5001 here is passed on to AerOS's port 5000, so tools/aernet.py can send to it
QEMU_NET = -nic user,model=e1000,hostfwd=udp::5001-:5000

.PHONY: all clean clean-disk move run run-kernel run-img run-vfs run-uefi disk-ls disk-put setup-dev test check FORCE

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
# _binary_disk_bin_hello_aer_start / _end marking where they are (see sys/apps.c)
obj/disk/%.o: disk/%
	@mkdir -p '$(dir $@)'
	$(LD) -m elf_i386 -r -b binary -z noexecstack -o '$@' '$<'

# a source file for /src/: obj/source/sys/fat.c.o holds sys/fat.c, between the symbols
# _binary_sys_fat_c_start and _end (ld turns every character that isn't a letter or digit into _)
obj/source/%.o: %
	@mkdir -p '$(dir $@)'
	@$(LD) -m elf_i386 -r -b binary -z noexecstack -o '$@' '$<'

# the table of them that sys/source.c looks through, one line per file:
#     { "sys/fat.c", _binary_sys_fat_c_start, _binary_sys_fat_c_end },
# It is written fresh every time, but only replaces the old one when something is different
# (a file was added or removed), so it is only compiled again when it needs to be
source_symbol = _binary_$(subst /,_,$(subst .,_,$(subst -,_,$(1))))

obj/source_index.c: FORCE
	@mkdir -p obj
	@{ \
	  echo '/* written by the Makefile: the source files built into AerOS, see sys/source.h */'; \
	  echo '#include "../sys/source.h"'; \
	  echo; \
	  $(foreach f,$(SOURCE_CODE),echo 'extern const char $(call source_symbol,$(f))_start[], $(call source_symbol,$(f))_end[];';) \
	  echo; \
	  echo 'const source_file_t source_files[] = {'; \
	  $(foreach f,$(SOURCE_CODE),echo '    { "$(f)", $(call source_symbol,$(f))_start, $(call source_symbol,$(f))_end },';) \
	  echo '};'; \
	  echo 'const int source_file_count = sizeof(source_files) / sizeof(source_files[0]);'; \
	} > '$@.new'
	@if cmp -s '$@.new' '$@'; then rm -f '$@.new'; else mv '$@.new' '$@'; fi

obj/source_index.o: obj/source_index.c
	$(CC) $(CFLAGS) -c '$<' -o '$@'

# the table of built-in files that sys/apps.c installs, one line per file in disk/:
#     { "/bin/games/", "snake.aer", _binary_disk_bin_games_snake_aer_start, ..._end, false },
# the folder is the file's own folder with disk/ taken off, and refresh is true for the manual
# (disk/docs/). Like source_index.c it is only replaced when a file was added or removed
builtin_dir = /$(patsubst disk/%,%,$(dir $(1)))

obj/builtin_index.c: FORCE
	@mkdir -p obj
	@{ \
	  echo '/* written by the Makefile: the files in src/disk/, built into AerOS, see sys/apps.h */'; \
	  echo '#include "../sys/apps.h"'; \
	  echo; \
	  $(foreach f,$(BUILTIN_FILES),echo 'extern const char $(call source_symbol,$(f))_start[], $(call source_symbol,$(f))_end[];';) \
	  echo; \
	  echo 'const builtin_file_t builtin_files[] = {'; \
	  $(foreach f,$(BUILTIN_FILES),echo '    { "$(call builtin_dir,$(f))", "$(notdir $(f))", $(call source_symbol,$(f))_start, $(call source_symbol,$(f))_end, $(if $(filter disk/docs/%,$(f)),true,false) },';) \
	  echo '};'; \
	  echo 'const int builtin_file_count = sizeof(builtin_files) / sizeof(builtin_files[0]);'; \
	} > '$@.new'
	@if cmp -s '$@.new' '$@'; then rm -f '$@.new'; else mv '$@.new' '$@'; fi

obj/builtin_index.o: obj/builtin_index.c
	$(CC) $(CFLAGS) -c '$<' -o '$@'

FORCE:

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

# check the C code keeps the conventions WHY and SRC rely on: a comment right above every
# function, definitions on one line, closing braces at column 0 (see tools/check.py)
check:
	python3 ../tools/check.py

# boot AerOS in QEMU and check it does what it should (see docs/TESTING.md). "make test T=why"
# runs only the tests with "why" in their name. They run on every processor core at once: J=1
# runs one at a time
test: main.iso
	QEMU=$(QEMU) python3 ../tools/test/run.py --kernel main.elf --iso main.iso $(if $(J),-j $(J)) $(T)

# boot the ISO through GRUB with the disk attached (terminal output is mirrored to this console via the serial port)
# -boot d starts from the CD, otherwise the BIOS would try the (non bootable) disk first
run: main.iso $(DISK)
	$(QEMU) -cdrom main.iso $(QEMU_DISK) -boot d -smp 1 -serial stdio $(QEMU_AUDIO) $(QEMU_NET)

# boot the kernel directly with the disk attached, skipping GRUB
run-kernel: main.elf $(DISK)
	$(QEMU) -kernel main.elf $(QEMU_DISK) -serial stdio $(QEMU_AUDIO) $(QEMU_NET)

# boot without a disk (temporary VFS mode)
run-vfs: main.iso
	$(QEMU) -cdrom main.iso -smp 1 -serial stdio $(QEMU_AUDIO) $(QEMU_NET)

# boot the ISO like a modern UEFI PC (OVMF firmware). The ISO only contains UEFI boot files when
# grub-efi-amd64-bin is installed (make setup-dev), and the 64 bit firmware needs the 64 bit QEMU
run-uefi: main.iso $(DISK)
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -cdrom main.iso $(QEMU_DISK) -m 256 -serial stdio $(QEMU_AUDIO) $(QEMU_NET)

# boot the ISO as a hard drive image
run-img: main.iso
	$(QEMU) -hda main.iso

# install the relevant packages
setup-dev:
	sudo apt install build-essential gcc-multilib nasm xorriso mtools grub-pc-bin grub-efi-amd64-bin grub-common qemu-system-x86 ovmf
