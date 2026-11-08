# AerOS
 Official repo for AerOS, written entirely in C and assembly language.
 
 ![alt text](media/thumb.png)
 
## What is AerOS?
It's an opensource operating system that can execute RenGets both natively and remotely on an external server. The final goal is to have an easy to use programming interface that incoperates functionality from the programming language "RenLang". 

## Documentation
AerOS is written to be read. Every source file starts with an explanation of what it does and how, and these guides tie it all together:

- [How AerOS works](docs/ARCHITECTURE.md): the source tree, the boot sequence, memory, interrupts, files, and recipes for adding commands, functions and drivers
- [RenLang](docs/RENLANG.md): the built-in programming language
- [Security](docs/SECURITY.md): user accounts, password hashing and file encryption, with every algorithm explained
- [Sound](docs/AUDIO.md): the AC'97 driver and WAV / MP3 playback

## But why basic C?
It offers a programming interface that is more convient than assembly language. However, assembly language will be used to implement core functionality as that is the only way this can be accomplished. 

## Why not just use the Linux kernel?
Building an operating system from the ground up that can run on bare metal is quite the accomplishment. By using a custom codebase, AerOS can be made far more versitile and even faster than Linux. AerOS is supposed to be the prime example of what a perfectly balanced platform should look like; with the implementation of only running RenGets that meet our specifications. 

## Where's the assembly?
I've decided to use inline assmembly in C with the GCC GNU99 standard for the most part when developing this operating system to keep things a little bit cleaner. Also, this allows me to break into assembly whenever I may need to do so without having to compile external files. 

## Tools Used
- Visual Studio Code IDE
- Ubuntu 22.04 LTS
- GCC GNU99
- NASM X86 ASM
- Qemu i386 & Virtual Box
- GNU GRUB
- Makefile
- OS Dev Wiki: https://wiki.osdev.org/Main_Page 

## To-Do List
- [x] Successful build for printing text.
- [x] Implement boot process that calls the main methods.
- [x] Implement a memory pool and a way to assign / un-assign memory. (kernel heap: malloc / free / realloc, a 4MB pool plus the free RAM found at boot)
- [x] Hardware drivers for user input. (PS/2 keyboard on IRQ1)
- [x] Switch to interrupt instead of maths for delays. (PIT timer at 1000Hz)
- [x] Sound driver for error reporting. (PC speaker beep)
- [x] Implement local filesystem. (FAT16 / FAT32 on IDE disks labelled AEROS, temporary VFS without one)
- [x] Process management.
- [x] Programming interface. (RenLang: an interpreter, a prompt and a text editor, see docs/RENLANG.md)
- [x] Graphics driver. (framebuffer console, pixels, shapes, text and BMP pictures for apps)
- [x] User accounts. (login screen, passwords stored as salted PBKDF2 hashes)
- [x] File encryption. (ENCRYPT / DECRYPT, AES-256 with an HMAC tag)
- [x] Audio. (AC'97 sound card, PLAY for WAV and MP3 files using minimp3, and a startup sound in /sys/startup.wav that you can replace)
- [ ] Ethernet network driver.
- [ ] More in the pipeline coming soon..
