# The prompt speaks AerLang

The terminal's prompt is also an AerLang prompt. A line that isn't a command or an app is run
as [AerLang](aerlang.md):

```
alex@0//home/alex/: let price = 12
alex@0//home/alex/: price * 3
36
alex@0//home/alex/: "AerOS" + "!"
AerOS!
```

The value of an expression is shown straight away. Variables and functions stay until you
log out, so you can build things up a line at a time.

## What runs as what

The first word decides, in this order:

1. **a command**: `del notes.txt`, `la`
2. **an app**: `hello`, or a `.aer` program in the folder you are in
3. **AerLang**: anything that looks like code: `let x = 1`, `x + 2`, `print(x)`, `"hi"`

A word AerLang doesn't know, on its own or followed by other words (`frob`, `dle notes.txt`),
is taken for a mistyped command, and AerOS suggests the closest one: `Did you mean DEL?`

Because commands come first, a variable with a command's name (`out`, `la`) can't be shown by
typing its name alone. Use `print(out)` instead.

## Code over several lines

A line that opens a block (an `if`, `for`, `while` or `func` without its `end`) carries on.
The prompt changes to `...` until the block is closed:

```
alex@0//home/alex/: func twice(n)
   ... return n * 2
   ... end
alex@0//home/alex/: twice(21)
42
```

Short blocks fit on one line too: `for f in la() do print(f) end`. An empty line stops
waiting for the `end`.

## Commands give back data: cmd()

`cmd(line)` runs a terminal command as if you had typed it, and gives back what it printed as
a list of lines, instead of showing it:

```
let files = cmd("la")
len(files)
cmd("mem")[0]
for row in cmd("vers") do print(row) end
```

The command can't use the keyboard while it runs, so a question it asks gets no answer (DEL's
"Delete it? (y/n)" counts as no). `cmd()` only works at the prompt: a program can't use it,
because programs have to [ask before they touch your files](perms.md), and a command never
asks.

## The AER prompt

`aer` opens a prompt where every line is AerLang, even one that looks like a command, with the
same variables as the terminal. `exit` leaves it.

## Mistakes

AerLang says what went wrong, and spots likely typing mistakes:

```
alex@0//home/alex/: prnt(1)
Error: unknown name 'prnt' (did you mean print?)
```

Press the up arrow to get the line back and fix it. `why` explains the error.
