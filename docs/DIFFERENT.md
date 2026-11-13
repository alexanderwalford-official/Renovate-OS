# What makes AerOS different

AerOS isn't trying to be a small Linux. Four ideas run through it that you won't find together
in the usual hobby OS, or in the big ones:

1. **It explains itself.** `why` explains the last thing that went wrong, `why del` explains
   a command, and `src del` shows its code. `show del notes.txt` runs a command and then
   tells you step by step what AerOS did inside. The source code is inside the OS, in `/src/`.
2. **The prompt is a programming language.** Anything that isn't a command runs as AerLang,
   so `let x = 2` and `x * 10` work at the prompt, and `cmd("la")` turns a command's output
   into a list.
3. **Apps ask before they touch your files.** A program can read AerOS's own files, but it
   asks before it reads yours or changes anything.
4. **Files remember their past.** Every change to a file in your home folder keeps the old
   version: `rd notes.txt@2` reads one, `undo notes.txt` brings it back.

This guide explains how each one works inside, for anyone reading or changing the code. The
user's side is in the manual inside AerOS (`doc why`, `doc shell`, `doc perms`,
`doc versions`).


## 1. It explains itself

### The source, inside the OS

The Makefile links AerOS's own source files into the kernel, the same way it links the
built-in apps:

```
SOURCE_CODE = every .c and .h AerOS is built from, plus entry.asm, linker.ld, Makefile, grub.cfg
              (not lib/minimp3 or lib/pl_mpeg, other people's code, nor the logo and the font,
              which are only data)

sys/fat.c --ld -r -b binary--> obj/source/sys/fat.c.o   (symbols _binary_sys_fat_c_start / _end)
everything --a shell loop--> obj/source_index.c          { "sys/fat.c", start, end }, ...
```

`obj/source_index.c` is written fresh by every `make`, but only replaces the old one when it
changes (a file was added or removed), so it is only compiled again when it needs to be.

`sys/source.c` serves that table as a folder. There are no real folders: `/src/sys/` "exists"
because some file's path starts with `sys/`, and listing it means finding the paths directly
inside it. `sys/f_sys.c` hands every path under `/src/` there before anything else, so every
command and AerLang function reads it like any other folder, and anything that would change it
gets `FS_ERR_READ_ONLY`. The files are exactly the ones this copy of AerOS was built from, so
they always match what is running.

### Reading C without a C parser

`WHY` and `SRC` find things in the source by relying on AerOS's code style, which keeps
`source.c` short:

| To find | It looks for |
| --- | --- |
| a function's definition | a line in a `.c` file starting at column 0 with the name followed by `(`, where `{` comes before any `;` |
| its declaration | the same in a `.h` file, where `;` comes first (it may carry on over more lines) |
| the end of its body | the next line that starts with `}` |
| what it is for | the comment right above it: `//` lines, or a `/* */` block, with no blank line between |

That is why the style guide asks for a comment above every function: it is the explanation
people read inside AerOS.

### WHY with a name

`WHY DEL` finds the command in the table (each row keeps its function's name as a string:
`COMMAND(..., cli_delete)` stores `"cli_delete"` with `#run`), then `explain_function()`:

1. prints where the function is and the comment above it (or above its declaration, if the
   definition has none), joining lines into paragraphs to fit the 80 column screen
2. goes through its body for calls: every name with a `(` after it, outside comments and
   strings, and not `x.run(...)` or `x->run(...)` (a pointer can't be looked up by name)
3. keeps the ones that are AerOS functions, leaving out the everyday C library ones in
   `lib/aer_*.c` (`prtf`, `scpy`...), and lists each with the first sentence of its own comment

AerLang functions work the same way: the built-in table stores `"bi_gfx_open"` for `gopen`
through its `BUILTIN` macro, and `aer_builtin_function()` looks it up.

### The last problem

A command that reports a problem calls `explain_say(PROBLEM_..., subject, format, ...)`
instead of `prtf`. The message looks the same (plus "(WHY explains)" for the first three
after logging in), and `sys/explain.c` keeps a copy. `WHY` on its own then prints the
explanation for that kind of problem from a table:

```c
[PROBLEM_READ_ONLY] = {
    "That is in /src/, AerOS's own source code, which can be read but not changed.",   // what
    "The files in /src/ are the ones this copy of AerOS was built from, ...",           // why
    "Read anything in there with @cli_read_file. To try out a change, ...",             // fix
    "fs_create_bytes", NULL,                                                             // code
},
```

`@cli_read_file` prints as the name of the command that runs `cli_read_file` today (`RD`),
through `cli_command_called()`, so renaming a command in the table renames it here too. The
code entry is the function that makes the decision; `WHY` says where it is and that `SRC`
shows it (one for DISK mode, and optionally another for the in-memory file system).

Two kinds of problem get more:

* **a file that wasn't found**: `explain_say` keeps the folder and name that were looked
  for, and `WHY` lists the names in that folder at most two changes away (`sdist`, in
  `lib/aer_string.c`: an edit distance that counts two swapped letters as one change).
* **an AerLang error**: `aer_error()` hands the message, the line and the program's code to
  `explain_program()`, which keeps a copy of the lines around it (the program's memory is gone
  by the time you type `WHY`), and `WHY` shows them with an arrow at the line that failed.

### SHOW: watching it work

`WHY` explains after the fact. `SHOW` narrates: `show del notes.txt` runs the line as if it had
been typed (`HandleInput`), then lists what AerOS did inside, step by step, each step naming the
C function that did it, so `WHY` and `SRC` can take it from there.

The steps come from `TRACE()` lines in the code (`sys/trace.h`). Each one says, in plain words,
what its function decided or did:

```c
TRACE("kept %s as it was, as %s@1 (in %s), so UNDO can bring it back", name, name, place.versions);
```

`TRACE()` is a macro that adds the function's own name (C's `__func__`) and checks a flag
first, so while nothing is watching it costs one comparison and no more. While `SHOW` watches,
`trace_note()` formats each note into the next slot of a plain array (48 of them: SHOW wants
how a command began, so any more are only counted), and the same note twice in a row becomes
one step with a count. `trace_print()` lists them, the names in a column and each note wrapped
under its own.

The notes are written where something is decided: which of the three places a file is in
(`f_sys.c`), clusters taken and freed and the order things are written in (`fat.c`), versions
kept or why not (`versions.c`), which permission rule allowed or refused something
(`perm.c`), what the firewall let through (`net.c`), a slow or failed disk write (`ata.c`), what
app mode put away (`appmode.c`), and processes made and run (`p_man.c`). Not every little
thing: what someone reading the code would want to know. Adding to it is one line.


## 2. The prompt is a programming language

### What a line is

`HandleInput()` in `sys/terminal/t_int.c` decides from the first word, in this order:

1. a command in the table
2. an app: `/bin/<word>.aer`, or a program in the current folder
3. AerLang, if `aer_shell_wants()` says the line is code
4. otherwise an unknown command, with "Did you mean DEL?" from the closest command or app name
   (`sdist`, one change allowed for names of three letters or fewer, two for longer ones)

Everything is code except what looks like a mistyped command: a plain word (letters, digits,
`_`, `.` and `/`), followed by nothing or by something that could start another word. Even
then it is code if the word is an AerLang keyword (`let`, `for`...) or a variable made at the
prompt. So `x = 1`, `f(2)`, `2 + 2` and `"hi"` are code, while `frob` and `dle notes.txt`
aren't.

### One session from login to logout

AerLang normally gives each program a clean session that is thrown away when it ends. The
prompt has a session of its own, started at login (`aer_shell_start`) and ended at logout
(`aer_shell_stop`), with `repl` set (the value of an expression is shown) and `shell` set
(`cmd()` works).

Only one session can be the running one: the interpreter's `state`, `names` and `blocks`
globals. So `session_begin()` copies the shell's session aside before a program starts, and
`session_end()` copies it back:

```
logged in:      state/names/blocks = the shell's session
run "hello":    shell's session --copied to--> shell_copy
                state/names/blocks = a clean session for hello.aer
hello ends:     its memory is freed, shell_copy --copied back--> state/names/blocks
```

A program can't start another program, so one copy is all that is needed.

Code at the prompt can run more code at the prompt (`cmd("1 + 1")` runs a "command" that is
AerLang), so `aer_shell_run()` saves where an error jumps to, the code and the running
function's variables, and puts them back afterwards. Logging out from inside a line of code
(`cmd("out")`) can't free the session under the line's feet, so it waits until the line is
done.

Every line's code is kept until logout, because a function made on one line is used on later
ones. A very long session slowly fills memory; logging out and in again empties it.

### Lines that carry on

`aer_open_blocks()` (in `lang/aer_lexer.c`) counts the `if`, `while`, `for` and `func` words
in the code, less the `end`s, skipping strings and comments. While that is above 0 the
terminal reads more lines with a `...` prompt, then runs them all together.

### cmd(): commands that give back data

`cmd(line)` calls `cli_capture()`, which runs the line with `console_capture_begin()` on: every
character the console would draw goes into a 16 KB buffer instead, and the buffer is split into
a list of lines. While capturing, `getk()` gives 0 (the keyboard is "unplugged"), since nobody
can see a question being asked, so a yes / no question is answered no and a typed answer is
empty.

`cmd()` only works at the prompt. Programs have to ask before they touch your files (idea 3),
and a terminal command never asks, so `cmd()` would be a way round that.


## 3. Apps ask before they touch your files

### The rules

`perm_allows(folder, name, kind)` in `sys/perm.c` goes through these in order and stops at the
first that decides:

| | When | Then |
| --- | --- | --- |
| 1 | no program is running (a command, or code at the prompt) | allowed: that is you |
| 2 | the file is in `/acc/` (the accounts) | refused, always |
| 3 | reading in `/bin/`, `/docs/`, `/src/` or `/sys/`, in the program's own folder, or a file or folder named on its command line | allowed |
| 4 | already answered while this program runs | that answer |
| 5 | answered "always" before (in `/acc/perms`) | allowed |
| 6 | anything else | ask |

There are two kinds, `PERM_READ` and `PERM_CHANGE` (make, change or delete), and being allowed
to change includes reading. A permission is for one folder, not the folders inside it.

### Where it is checked

`aer_run_file()` tells `perm_program_started()` which program is starting (its folder, name and
arguments) and `perm_program_ended()` when it is done. AerLang's file functions get their path
through `file_arg(..., kind, ...)` or `allowed_path(...)` in `lang/bi_args.c`, which call
`perm_allows()`. If the answer is no they fail exactly as they would for a missing file (`nil`
or `false`), so a program doesn't need any special code for it.

The checks are deliberately not in `sys/f_sys.c`: the terminal's commands use that too, and
they never need to ask.

### Asking

On the text screen the question is two lines in yellow, and the answer is typed after them. A
program drawing graphics gets a box in the middle of the screen instead, drawn with
`gfx_overlay_begin()` / `gfx_overlay_end()` (in `drivers/video/gfx.c`), which keep a copy of the
pixels under the box and draw straight to the screen even when the program has a back buffer,
so its picture is exactly as it was once the box goes. Esc, Ctrl+C, or no keyboard at all
(inside `cmd()`) count as no.

"Always" answers are lines in `/acc/perms`:

```
alex /bin/viewer.aer read /home/alex/
alex /home/alex/game.aer change /home/alex/
```

who answered, the program, the kind, and the folder. `/acc/` is the one folder no program can
touch, so no program can give itself permission. `PERM` lists the logged in user's lines and
`PERM DEL` removes them.

What isn't covered: the terminal's own commands (they are you), and C code inside the kernel.
A program is known by its path, so a different program saved under the same name in the same
place inherits its "always" answers.


## 4. Files remember their past

### What is kept, and when

`sys/f_sys.c` calls `versions_keep(folder, name)` (in `sys/versions.c`) before it changes or
deletes a file: in `fs_write_f`, `fs_write_bytes` and `fs_delete_f`. That keeps a copy of the
file as it is, if:

* it is inside a home folder, `/home/<name>/`, and not in that home folder's `versions/`
* it is no bigger than 1 MB
* no version of it was kept in the last 10 seconds (so a program writing a log line by line
  keeps the file as it was before it started, not a thousand copies)

`fs_empty_dir()` (deleting a whole folder, and the wipe after three wrong passwords) pauses
versions while it works: copying everything just to delete it would be pointless. Writing with
`wipe_old` (encryption) calls `versions_forget()` instead, which overwrites the file's versions
with zeros and deletes them, as they are exactly what the encryption is protecting.

### Where, and in what form

The versions live in the home folder's own `versions/` folder. Names on a FAT disk must fit 8.3,
so each file gets ten slots named after an FNV-1a hash of its lower case path:

```
/home/alex/notes.txt  --FNV-1a-->  3F2A91C0  -->  /home/alex/versions/3F2A91C0.V0 ... .V9
```

Each version starts with one line, then the file's bytes as they were:

```
#AERV 17 10-11-26--14:30:05 /home/alex/notes.txt
...the old contents...
```

17 is how many versions were kept before this one: sorting the slots by it puts them in
order, newest first, as `@1`, `@2`... A new version goes into an empty slot while there are
fewer than ten, then into the oldest one's place. The path in the line is checked too, so two
files whose paths happen to have the same hash can't mix up their versions.

### Reading and going back

`fs_read_bytes()` and `fs_file_exists()` try a name like `notes.txt@2` as a version when there
is no real file called that, so `RD`, the reader and AerLang's `rdf()` all understand it
without knowing anything about versions.

`versions_restore()` (`UNDO`) first keeps the file as it is now, ignoring the 10 second rule
(an undo must always be undoable), then writes the old contents back, or makes the file again
if it was deleted.
