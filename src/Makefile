CC = gcc
AS = nasm
LD = ld

# freestanding 32 bit kernel:
#   -fno-stack-protector / -fno-pie / -fcf-protection=none : Ubuntu's gcc turns these on by default, they need runtime support we don't have
#   -mno-sse / -mno-mmx : SSE is not enabled on the CPU, using it would crash
#   -fno-tree-loop-distribute-patterns : stops gcc turning our own memset/memcpy loops into calls to themselves
CFLAGS = -m32 -march=i686 -std=gnu99 -ffreestanding -fno-builtin -fno-stack-protector \
         -fno-pie -fno-pic -fcf-protection=none -fno-asynchronous-unwind-tables \
         -fno-tree-loop-distribute-patterns -mno-mmx -mno-sse -mno-sse2 \
         -Os -Wall -Wextra
LDFLAGS = -m elf_i386 -nostdlib -T linker.ld -z noexecstack

# every header is part of the kernel (it is built as one file), so rebuild when any of them change
HEADERS = $(wildcard lib/*.h sys/*.h sys/*/*.h drivers/*/*.h drivers/*/*/*.h drivers/*/*/*/*.h)

QEMU = qemu-system-i386

.PHONY: all clean move run run-kernel run-img setup-dev

all: main.iso

# compile ISO file (final)
main.iso: main.elf iso/boot/grub/grub.cfg
	cp '$<' iso/boot
	grub-mkrescue -o '$@' iso

# multiboot config
main.elf: entry.o main.o linker.ld
	$(LD) $(LDFLAGS) -o '$@' entry.o main.o

# assembly entry point
entry.o: entry.asm
	$(AS) -f elf32 '$<' -o '$@'

# kernel
main.o: kernel.c $(HEADERS)
	$(CC) $(CFLAGS) -c '$<' -o '$@'

# delete old compiled files
clean:
	rm -f *.elf *.o *.iso iso/boot/*.elf

# copy the file to the host
move:
	#sudo rm /home/vboxuser/share/main.iso
	sudo cp main.iso /home/vboxuser/share/

# boot the ISO through GRUB (terminal output is mirrored to this console via the serial port)
run: main.iso
	$(QEMU) -cdrom main.iso -smp 1 -serial stdio

# boot the kernel directly, skipping GRUB
run-kernel: main.elf
	$(QEMU) -kernel main.elf -serial stdio

# boot the ISO as a hard drive image
run-img: main.iso
	$(QEMU) -hda main.iso

# install the relevant packages
setup-dev:
	sudo apt install build-essential gcc-multilib nasm xorriso mtools grub-pc-bin grub-common qemu-system-x86
