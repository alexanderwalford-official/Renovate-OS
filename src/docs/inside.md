# How AerOS works

AerOS is small on purpose: small enough to understand completely, and fast because it does
so little. It leaves out what bigger systems need for many programs at once:

| Left out | Instead | So |
| --- | --- | --- |
| virtual memory | every address is a real one | nothing to set up or look up |
| multitasking | one thing at a time, to the end | no switching, no locks |
| loadable drivers | every driver is built in | the OS is one ~430 KB file |

The cost: a bug anywhere can stop everything. That is a fair trade for a system meant for
learning and building on.

## Starting up

```
GRUB         loads AerOS at 1 MB and sets up the screen
entry.asm    makes a stack, calls main()
kernel.c     screen, interrupts, timer, keyboard, then switches interrupts on
splash       the logo and the startup sound (X for boot options)
init_boot    the boot log: crypto self-test, sound card, disk, built-in files
terminal     the login screen, then the prompt, until you turn it off
```

## Memory

```
0 MB     +--------------------+
         | BIOS, VGA text     |
1 MB     +--------------------+
         | AerOS (code, data, |
         | 256 KB stack,      |
         | 4 MB of heap)      |
         +--------------------+
         | the rest of the    |  added to the heap at start up
         | memory             |
         +--------------------+
```

`MEM` shows how much is in use.

## Interrupts

The CPU stops whatever it is doing when hardware needs attention:

| What | When | AerOS |
| --- | --- | --- |
| the timer | 1000 times a second | counts milliseconds |
| the keyboard | each key press | stores the key |
| a CPU error | divide by zero... | shows the red error screen |

When there is nothing to do, AerOS halts the CPU until the next interrupt, so it uses
almost no power sitting at the prompt.

## The parts

| Part | Source |
| --- | --- |
| the terminal and its commands | `src/sys/terminal/t_int.c` |
| files: disk (FAT) or memory | `src/sys/f_sys.c`, `src/sys/fat.c` |
| accounts and encryption | `src/sys/accounts/`, `src/sys/fcrypt.c` |
| AerLang | `src/lang/` |
| drivers | `src/drivers/` |
| apps and this manual | `src/apps/`, `src/docs/` |

Every source file starts with an explanation of what it does. The full guide, with recipes
for adding commands, AerLang functions and drivers, is `docs/ARCHITECTURE.md` in the AerOS
source.
