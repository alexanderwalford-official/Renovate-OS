# Build Instructions

## Not OS Specific:
- nasm -f elf64 boot.asm -o boot.o
- g++ -c kernel.c -o kernel.o -ffreestanding -fno-exceptions -fno-rtti
- gcc boot.o kernel.o -T linker.ld -o kern -nostdlib -nodefaultlibs -lgcc

## On Linux:
- cd src/_dev/
- grub-mkrescue iso --output=kern.iso
