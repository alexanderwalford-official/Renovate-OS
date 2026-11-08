# Getting started

## Booting

When AerOS starts you see the orange splash screen and hear the startup sound. Press **X**
while it is showing to open the boot options:

| Key | Option |
| --- | --- |
| 1 | Normal boot |
| 2 | Fast boot: skips the pause on the boot log |
| 3 | Demo mode: never saves anything to a disk |
| 4 | Reboot |
| 5 | Shut down |

Then the boot log lists every piece of hardware AerOS set up, with `[ OK ]`, `[WARN]` or
`[FAIL]` in front of each line.

## The three modes

The coloured line under the terminal's title says which mode you are in:

| Mode | When | Your files |
| --- | --- | --- |
| DISK | an AEROS disk was found | saved to the disk |
| VFS | no AEROS disk | kept in memory, lost when it turns off |
| DEMO | picked in the boot options | kept in memory, disks untouched |

An AEROS disk is any IDE disk formatted as FAT16 or FAT32 with the volume label `AEROS`.

## Logging in

In DISK mode AerOS asks who you are. The very first time, it asks you to make an account:
choose a name (up to 8 lower case letters, numbers, `_` and `-`) and a password. After that,
the login screen appears at every start, and you start in your home folder, `/home/<name>/`.
In VFS and DEMO mode there is no login and you are `guest`.

**Careful:** after 3 wrong passwords in a row for an account, everything in its home folder
is deleted. The first wrong password brings a warning. See [Accounts and encryption](security.md).

## The prompt

```
alex@0//:
```

shows who you are (`alex`) and the folder you are in (`/`). Type a command and press Enter.
The up and down arrows bring back earlier commands.

## First things to try

| Type | To |
| --- | --- |
| `help` | list every command |
| `docs` | open this manual |
| `la` | list the files here |
| `hello` | run the hello program |
| `guess` | play a guessing game |
| `viewer` | look at the pictures in `/bin/` |
| `play /sys/startup.wav` | hear the startup sound again |
| `edit notes.txt` | write a file |
| `aer` | try AerLang, type `exit` to leave |

Next: [Terminal commands](commands.md)
