# remove old files
.PHONY: clean run rub-img

# compile ISO file (final)
main.iso: main.elf
	cp '$<' iso/boot
	grub-mkrescue -o '$@' iso

# multiboot config
main.elf: entry.o main.o
	ld -m elf_i386 -nostdlib -T linker.ld -o '$@' $^

# assembly entry point
entry.o: entry.asm
	nasm -f elf32 '$<' -o '$@'

# kernel
main.o: kernel.c
	gcc -c -m32 -std=gnu99 -ffreestanding -fno-builtin -Os -o '$@' -Wall -Wextra '$<'

# delete old compiled files
clean:
	rm -f main.o
	rm -f main.iso
	rm -f entry.o
	rm -f *.elf *.o iso/boot/*.elf *.iso

# GRUB config with kernel integration
run: main.elf
	qemu-system-i386 -kernel '$<'

# compile ISO file
rub-img: main.iso
	qemu-system-i386 -hda '$<'
