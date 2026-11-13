# Terminal commands

Commands can be typed in upper or lower case. Words in `<angle brackets>` are what you
fill in, and `[square brackets]` mean it can be left out. `HLP` shows this list too.

Press **Tab** to finish a word: the first word becomes a command or app, any later word a
file or folder (after `sd`, only folders). When several things fit, Tab lists them.

Press **Page Up** and **Page Down** to scroll back through what a command printed. The last
400 lines are kept, a tag in the top right says how far back you are, and any other key
takes you back to the prompt. Logging out forgets them.

Anything that isn't a command or an app runs as AerLang, so `2 + 2` or `let x = 5` work
right at the prompt: see [The prompt speaks AerLang](shell.md). A mistyped command gets a
suggestion (`Did you mean DEL?`), and `why` explains any problem.

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
| `vers [file]` | a file's [old versions](versions.md); on its own, every file that has some |
| `undo <file> [n]` | bring a file back as it was (or back from being deleted) |
| `edt <file>` | open a file in the [editor](editor.md) |
| `read [file]` | open a file in the [reader](reader.md) |
| `doc [topic]` | this manual, e.g. `doc sound` |

## Asking AerOS

| Command | Does |
| --- | --- |
| `why` | explain the last thing that went wrong ([more](why.md)) |
| `why <name>` | explain a command, an AerLang function or a C function: `why del` |
| `src <name>` | show the code behind one, with line numbers: `src del`, `src fat.c` |
| `show <command>` | run a command, then see step by step what AerOS did inside: `show del notes.txt` ([more](why.md)) |
| `perm [del <n>]` | what [programs may always do](perms.md) with your files, or forget it |

## Encrypted files

| Command | Does |
| --- | --- |
| `enc <file>` | lock a file with a password |
| `dec <file>` | unlock it again |

See [Accounts and encryption](security.md).

## Accounts

| Command | Does |
| --- | --- |
| `me` | who is logged in |
| `usrs` | list the accounts |
| `mku <name>` | make a new account and its home folder |
| `delu <name>` | delete an account (asks for your password) |
| `pw` | change your password |
| `out` | log out, back to the login screen |

## Programs

| Command | Does |
| --- | --- |
| `<app> [args]` | run `/bin/<app>.aer`, e.g. `hello` or `guess` |
| `aer` | the AerLang prompt |
| `sysinfo` | the system information screen: processor, memory, disks, devices |
| `anim` | a demo of the animations AerLang can draw |
| `run <code>` | run code as a process: `run s:hi` |
| `que <app> [args]` | line a program up to run later (`que` lists them, `que clear` empties it) |
| `go` | run everything queued, in order (Ctrl+C stops the rest) |

AerOS runs one program at a time, so instead of running several together you can queue them
and let `go` run them one after another:

```
que hello
que s:--- now the game ---
que guess
go
```

`que s:<text>` queues a line of text to print, handy between programs.
| `pm` | show the process stack |

## Sound and screen

| Command | Does |
| --- | --- |
| `ply <file>` | play a WAV or MP3 file, or an .mpg [video](video.md); Esc stops it |
| `bp [hz]` | beep the PC speaker |
| `res [WxH]` | show or change the resolution: `res 1280x720` |
| `cls` | clear the screen |
| `ech <text>` | print some text |

## The network

| Command | Does |
| --- | --- |
| `net` | the card, the address, and what was sent and kept out |
| `net ip <address> <gateway> <dns>` | set AerOS's address: `net ip 10.0.2.15/24 10.0.2.2 10.0.2.3` |
| `fw` | what the firewall allows (nothing, to start with) |
| `fw allow <address or name> <port>` | allow one computer on one port, with a shared password |
| `fw del <n>` | block it again |
| `msg <address> <port> <text>` | send a sealed message |
| `lsn <port>` | show the messages that arrive on a port, until Esc |

See [The network](network.md).

## The computer

| Command | Does |
| --- | --- |
| `clk` | the date and time |
| `up` | how long since it started |
| `mem` | memory use |
| `rt [stress]` | [RT mode](rt.md)'s real-time tasks (experimental, picked in the boot options) |
| `drv` | the disks AerOS can see |
| `rbt` | restart |
| `off` | turn off |

Press **Ctrl+C** to stop a program that has got stuck.
