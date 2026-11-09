# Terminal commands

Commands can be typed in upper or lower case. Words in `<angle brackets>` are what you
fill in, and `[square brackets]` mean it can be left out. `HLP` shows this list too.

Press **Tab** to finish a word: the first word becomes a command or app, any later word a
file or folder (after `sd`, only folders). When several things fit, Tab lists them.

Press **Page Up** and **Page Down** to scroll back through what a command printed. The last
400 lines are kept, a tag in the top right says how far back you are, and any other key
takes you back to the prompt. Logging out forgets them.

## Files and folders

| Command | Does |
| --- | --- |
| `la` | list the files and folders here |
| `sd <dir>` | go to a folder: `sd /bin`, `sd ..`, `sd /` |
| `rd <file>` | show a file (asks for the password if it is encrypted) |
| `mkf <file> [text]` | make a file, with some text in it if you like |
| `wr <file> <text>` | add a line to the end of a file |
| `mkd <dir>` | make a folder |
| `del <name>` | delete a file, or a folder and everything in it (asks first) |
| `edit <file>` | open a file in the [editor](editor.md) |
| `read [file]` | open a file in the [reader](reader.md) |
| `docs [topic]` | this manual, e.g. `docs sound` |

## Encrypted files

| Command | Does |
| --- | --- |
| `encrypt <file>` | lock a file with a password |
| `decrypt <file>` | unlock it again |

See [Accounts and encryption](security.md).

## Accounts

| Command | Does |
| --- | --- |
| `whoami` | who is logged in |
| `users` | list the accounts |
| `newuser <name>` | make a new account and its home folder |
| `deluser <name>` | delete an account (asks for your password) |
| `pass` | change your password |
| `out` | log out, back to the login screen |

## Programs

| Command | Does |
| --- | --- |
| `<app> [args]` | run `/bin/<app>.aer`, e.g. `hello` or `guess` |
| `aer` | the AerLang prompt |
| `sysinfo` | the system information screen: processor, memory, disks, devices |
| `anim` | a demo of the animations AerLang can draw |
| `run <code>` | run code as a process: `run s:hi` |
| `queue <app> [args]` | line a program up to run later (`queue` lists them, `queue clear` empties it) |
| `go` | run everything queued, in order (Ctrl+C stops the rest) |

AerOS runs one program at a time, so instead of running several together you can queue them
and let `go` run them one after another:

```
queue hello
queue s:--- now the game ---
queue guess
go
```

`queue s:<text>` queues a line of text to print, handy between programs.
| `pman` | show the process stack |

## Sound and screen

| Command | Does |
| --- | --- |
| `play <file>` | play a WAV or MP3 file, Esc stops it |
| `beep [hz]` | beep the PC speaker |
| `res [WxH]` | show or change the resolution: `res 1280x720` |
| `cls` | clear the screen |
| `echo <text>` | print some text |

## The computer

| Command | Does |
| --- | --- |
| `clk` | the date and time |
| `time` | how long since it started |
| `mem` | memory use |
| `drv` | the disks AerOS can see |
| `reboot` | restart |
| `shutdown` | turn off |

Press **Ctrl+C** to stop a program that has got stuck.
