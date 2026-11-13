# Ask AerOS why

AerOS can tell you why something went wrong, how any command works, and show you the very
code that does it, all without leaving AerOS.

## When something goes wrong

Type `why` after a problem, and AerOS explains it:

```
alex@0//home/alex/: rd notez.txt
File not found: notez.txt  (WHY explains)
alex@0//home/alex/: why
```

You get what happened, why AerOS works that way, what you can do about it, and the name of
the C function that made the decision. For a file that isn't there, `why` also lists the
names in that folder that are spelled almost the same. For an AerLang program that stopped,
it shows the lines around the error with an arrow at the one that failed.

The first few problems after logging in end with `(WHY explains)`, as a reminder. After that
AerOS assumes you know.

## How does a command work?

`why` with a name explains that name instead:

| Type | To learn about |
| --- | --- |
| `why del` | a command: what it does, and the C function behind it |
| `why gopen` | an AerLang function |
| `why fs_list` | any C function in AerOS |

The explanation is read straight out of AerOS's own source code: it is the comment written
above the function, and a list of the AerOS functions it uses, each with the first sentence
of its own comment. So it is always right for the AerOS you are running. Follow the trail by
asking `why` about any of the functions it lists.

## Show me the code

`src` shows the code itself, with line numbers:

```
src del          the function that runs DEL
src gopen        the C code behind an AerLang function
src fs_list      any C function
src fat.c        a whole file, in the reader
```

Page Up scrolls back if the function is longer than the screen.

## Watch it work

`show` runs a command, then tells you step by step what AerOS did inside to carry it out:

```
alex@0//home/alex/: show wr notes.txt more
What AerOS did:
  1. versions_keep     kept notes.txt as it was, as notes.txt@1, so UNDO can
                       bring it back
  2. fs_write_f        adding to /home/alex/notes.txt
  3. fat_write_chain   wrote 19 bytes into 1 new cluster(s), starting at
                       cluster 1175
  4. fat_replace_file  pointed notes.txt's entry at the new data (one sector
                       write), so the old data can go
  5. fat_free_chain    freed 1 cluster(s), writing 1 sector(s) of each FAT copy
```

Each step names the C function that did it, so `why` and `src` can tell you more about any of
them. It works for anything you can type: `show del notes.txt`, `show hello`, `show msg ...`,
or a line of AerLang. It is a good way to see why something was refused, too: `show msg 10.0.2.2
5000 hi` shows the firewall deciding.

## /src/: all of AerOS

The source code AerOS was built from is built into it, as a folder you can read but not
change:

```
sd /src
la
read /src/sys/fat.c
```

Everything is there except other people's code (the MP3 and video decoders) and two big
files that are only data (the splash screen's logo and the font). Anything that would change
a file in `/src/` is refused; to try out a change, copy the file somewhere of your own first.

See also: [Terminal commands](commands.md), [How AerOS works](inside.md).
