# Files remember

Before a file in your home folder is changed or deleted, AerOS keeps a copy of it as it was.
The last ten copies of each file are kept, so a mistake is never more than a command away
from being put right.

## Looking back

```
vers notes.txt        the old versions, newest first
rd notes.txt@1        the file as it was before its last change
rd notes.txt@2        and before the change before that
```

`notes.txt@2` works anywhere a file is read: with `rd`, in the reader (`read notes.txt@2`),
and in AerLang (`rdf("notes.txt@2")`). `vers` on its own lists every file in your home
folder that has old versions.

## Going back

```
undo notes.txt        bring back notes.txt@1
undo notes.txt 3      bring back notes.txt@3
```

What `undo` replaces is kept as a version too, so if you undo by mistake, `undo` again puts
it back. A deleted file comes back the same way: `del notes.txt`, then `undo notes.txt`.

## What is kept

| Kept | Not kept |
| --- | --- |
| files anywhere in `/home/<name>/` | files outside home folders |
| the last 10 versions of each file | files bigger than 1MB |
| a file deleted with `del` | the files of a folder deleted all at once |
| | more than one copy every 10 seconds |

The 10 second rule stops a program that writes a file over and over (a log, say) from
filling the disk: you get the file as it was before it started.

The copies are kept in your home folder's `versions` folder, so they belong to you, and the
wipe after three wrong passwords takes them along with everything else.

## Versions and encryption

Encrypting a file with `enc` deletes its old versions, overwriting them with zeros first:
they are exactly the copies the encryption is there to protect.

See also: [Files and folders](files.md), [Accounts and encryption](security.md).
