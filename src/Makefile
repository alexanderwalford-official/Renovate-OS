.POSIX:
.PHONY: clean run rub-img

main.iso: main.elf
	cp '$<' iso/boot
	grub-mkrescue -o '$@' iso

# main.elf is the the multiboot file.
main.elf: entry.o main.o
	ld -m elf_i386 -nostdlib -T linker.ld -o '$@' $^

entry.o: entry.asm
	nasm -f elf32 '$<' -o '$@'

main.o: main.c
	gcc -c -m32 -std=c99 -ffreestanding -fno-builtin -Os -o '$@' -Wall -Wextra '$<'

clean:
	rm -f *.elf *.o iso/boot/*.elf *.iso

run: main.elf
	qemu-system-i386 -kernel '$<'

run-img: main.iso
	qemu-system-i386 -hda '$<'