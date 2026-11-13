# Programs ask first

An AerLang program can read AerOS's own files freely, but before it reads your files, or
changes any file at all, it has to ask you:

```
[ ? ] viewer.aer wants to read files in /home/alex/ (cat.bmp)
      Y = yes, this time    A = always    N = no
```

A program that is drawing graphics asks in a box in the middle of the screen instead, and
its picture is left exactly as it was once you answer.

| Answer | Means |
| --- | --- |
| **Y** | yes, until the program ends |
| **A** | yes, from now on: AerOS remembers it |
| **N** (or Esc) | no, until the program ends |

If you say no, the program carries on as if the file wasn't there: `rdf()` gives `nil`,
`wrf()` gives `false`, and so on.

## What needs asking

| The program wants to | Asks? |
| --- | --- |
| read in `/bin/`, `/docs/`, `/src/` or `/sys/` | no, these are AerOS's own files |
| read in its own folder | no |
| read a file you named when you started it (`read notes.txt`) | no, that was you asking |
| read anything else | yes |
| change or delete any file | yes |
| touch `/acc/`, where the accounts are kept | never allowed, whatever you answer |

A permission covers one folder (not the folders inside it), so a program that reads ten
pictures from the same folder asks once. Being allowed to change files includes reading them.

The terminal's own commands, and code you type at the prompt, never ask: that is you.

## The answers you gave

```
perm              what you have allowed always
perm del 2        forget the second one
perm del all      forget them all
```

They are kept in `/acc/perms`, one line each. No program can ever touch `/acc/`, so no
program can give itself permission.

See also: [AerLang](aerlang.md), [Accounts and encryption](security.md).
