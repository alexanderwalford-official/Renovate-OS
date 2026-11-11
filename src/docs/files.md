# Files and folders

## The folders

| Folder | Holds |
| --- | --- |
| `/acc/` | the accounts file, `users`, and what programs may always do, `perms` |
| `/bin/` | the apps: `hello.aer`, `read.aer`, pictures... |
| `/bin/games/` | the games: `snake.aer`, `guess.aer`, `move.aer` |
| `/docs/` | this manual |
| `/home/<name>/` | each user's own folder, where the prompt starts |
| `/home/<name>/versions/` | the [old versions](versions.md) of the files in it |
| `/src/` | AerOS's own source code, read only ([more](why.md)) |
| `/sys/` | system files, like `startup.wav` and `apps.lst` |
| `/sys/cache/`, `/sys/tmp/` | room for the system to work in |
| `/conf.aer` | settings (not used much yet) |

The apps and this manual are built into AerOS and copied onto the disk when it starts, so
they are there even with no disk at all. A newer AerOS updates its apps for you, but if you
change an app in `/bin/` your version is kept: `/sys/apps.lst` remembers which version
AerOS installed, so it can tell yours apart. To get the built-in copy back, delete your
version and restart. The manual is always refreshed.

## Paths

A path says where a file is. It can start from the top (`/bin/hello.aer`) or from the folder
you are in (`notes.txt`, `../readme.md`):

```
/                  the top folder
/bin/hello.aer     hello.aer in the bin folder
.                  the folder you are in
..                 the folder above it
```

`sd` moves between folders and `la` lists what is in one.

## File names

On a disk, new file names follow the old **8.3** rule: up to 8 letters or numbers, a dot,
and up to 3 more (`NOTES.TXT`, `SONG.MP3`). That is why the HTML sample here is `sample.htm`
and not `sample.html`. Longer names written by another computer can still be read.

On a disk, names are not case sensitive: `Notes.txt` and `NOTES.TXT` are the same file.
Without a disk (VFS and DEMO mode) they are, so keep to one way of writing each name.

## Old versions

Before a file in your home folder is changed or deleted, AerOS keeps a copy of it.
`vers notes.txt` lists them, `rd notes.txt@1` reads the newest, and `undo notes.txt` brings
it back. See [Files remember](versions.md).

## Saving safely

When a file changes, AerOS writes the new version into free space first and only then
switches the file over to it. If the power goes in the middle, you get either the old file
or the new one, never half of each.

## Getting files in and out

With QEMU on another computer, the disk is a file (`disk.img`). From the `src` folder of
AerOS:

```
make disk-put FILE=photo.bmp DIR=/bin
make disk-ls
```
