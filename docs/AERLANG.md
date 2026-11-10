# AerLang

AerLang is the programming language built into AerOS. Programs are text files ending in `.aer`.
The interpreter runs inside the kernel, so a program can use the screen, keyboard and files directly.

## Running programs

| You type | What happens |
| --- | --- |
| `hello` | runs `hello.aer` from the current directory, or from `/bin/` |
| `hello.aer a b` | the same, with `args` set to `["a", "b"]` |
| `run hello.aer` | runs it through the process manager like any other AerLang instruction |
| `let x = 5`, `x * 2` | AerLang right at the terminal's prompt (see "At the prompt" below) |
| `aer` | a prompt where every line is code, even one that looks like a command, `exit` to leave |

Press **Ctrl+C** to stop a program that is stuck in a loop.

### At the prompt

A line typed at the terminal that isn't a command or an app is run as AerLang, in one session
that lasts until you log out, so variables and functions are kept from line to line. The value
of an expression is shown:

```
alex@0//home/alex/: let price = 12
alex@0//home/alex/: price * 3
36
alex@0//home/alex/: func twice(n)
   ... return n * 2
   ... end
alex@0//home/alex/: twice(price)
24
```

A line that opens a block (`if`, `for`, `while` or `func` without its `end`) carries on over
more lines, with `...` as the prompt, until the block is closed (an empty line gives up). The
first word decides what a line is: a command first, then an app, then code. A plain word that
AerLang doesn't know (`frob`, `dle notes.txt`) is taken for a mistyped command and gets a
suggestion, `Did you mean DEL?`. So a variable named like a command (`out`) is shown with
`print(out)`, not by typing its name.

Only at the prompt, `cmd(line)` runs a terminal command and gives back what it printed as a
list of lines, instead of showing it: `len(cmd("la"))`, `cmd("mem")[0]`. The keyboard counts
as unplugged while it runs, so a question the command asks gets no answer (a yes / no question
is a no). Programs can't use `cmd()`: they have to ask before they touch your files, and a
terminal command wouldn't.

### Mistakes

An unknown name gets a suggestion, from the names the program has: `unknown name 'prnt' (did
you mean print?)`. `why` after an error explains it and shows the lines around it, and
`why <function>` (`why gopen`) explains a built-in from the comment above the C code behind
it. `src gopen` shows that code.

AerLang used to be called RenLang, and its programs ended in `.ren`. Those still run if you
type the name with its extension (`old.ren`), and the editor still colours them in.

## Writing programs on AerOS

`edt myapp.aer` opens the editor (the file is made when you first save it). AerLang code is
coloured in, and a new line keeps the indentation of the one above.

| Key | |
| --- | --- |
| Ctrl+S | save |
| Ctrl+R | save and run the program, then press a key to come back |
| Ctrl+Q | quit (press it twice to throw away unsaved changes) |
| arrows, Home, End, Page Up / Down | move around |

Programs can also be written on another computer and copied onto the AerOS disk with
`make disk-put FILE=myapp.aer` (see `src/README.md`).

The apps in `src/apps/` are built into AerOS and copied to `/bin/` when it starts.

## The language

```
# a comment runs to the end of the line

let name = "AerOS"          # make a variable
name = name + "!"           # change it
print("Hello", name)        # Hello AerOS!

if len(name) > 3 then
    print("long")
elif len(name) == 3 then
    print("three")
else
    print("short")
end

for i = 1 to 10 do          # 1, 2 ... 10 (add "step 2" to count in twos, "step -1" to count down)
    if i % 2 == 0 then continue end
    print(i)
end

for colour in ["red", "green"] do
    print(colour)
end

let n = 0
while n < 3 do
    n = n + 1
end

func area(w, h)
    return w * h
end
print(area(3, 4))           # 12
```

- A statement ends at the end of its line. Short blocks can go on one line: `if x then print(x) end`.
- Variables are made with `let`. Assigning to a name that was never made is an error, which catches typos.
- Variables made with `let` inside a function belong to that function. Everything else is global.
- Functions are made at the top level of a file and can be called before the line that makes them.

### Values

| Type | Examples | Notes |
| --- | --- | --- |
| number | `42`, `-7`, `0xFF` | whole numbers, 32 bit. `/` drops the remainder, `%` is the remainder |
| string | `"hi\n"` | escapes: `\n` `\t` `\"` `\\` `\0`. `s[0]` is the first character |
| list | `[1, "two", [3]]` | `l[0]`, `l[0] = 5`, `l + [6]` makes a new list |
| bytes | `bytes(100)` | a block of raw bytes (0 to 255), e.g. a file read with `rdb` |
| bool | `true`, `false` | |
| nil | `nil` | "nothing", e.g. what a function returns without `return` |

`nil`, `false` and `0` count as false. `and` / `or` give back one of their values, so
`input("Name? ") or "friend"` works. `+` joins strings, and turns anything else into text when
one side is a string: `"Score: " + 10`.

### Built-in functions

The short names follow the same pattern as the terminal's commands, so they can be guessed:

| Part | Means | | Part | Means |
| --- | --- | --- | --- | --- |
| `rd` | read | | `f` | file |
| `wr` | write (replace) | | `b` | bytes |
| `ap` | append (add to the end) | | `enc` / `dec` | encrypt / decrypt |
| `del` | delete | | `g` | graphics |
| `is` | is there one? / is it? | | `la` | list all (like the `LA` command) |

So `rdf` reads a file, `apf` appends to one, `isf` asks if a file is there, `rdenc` reads an
encrypted file and `gopen` opens the graphics screen.

**Screen and keyboard**
`print(a, b, ...)` print, separated by spaces, then a new line ·
`write(...)` the same without the new line ·
`input(prompt)` read a line · `inpw(prompt)` the same, showing `*` (for passwords) ·
`cls()` clear the screen ·
`color(fg, bg)` text colours (`BLACK` ... `WHITE`, and `ORANGE`, the logo orange) ·
`at(x, y)` move the cursor ·
`key()` wait for a key and return its code: a character code, or one of `KEY_UP`, `KEY_DOWN`,
`KEY_LEFT`, `KEY_RIGHT`, `KEY_HOME`, `KEY_END`, `KEY_PGUP`, `KEY_PGDN`, `KEY_DELETE`,
`KEY_ENTER`, `KEY_BACKSPACE`, `KEY_TAB`, `KEY_ESCAPE` ·
`haskey()` true if a key is waiting

**Text**
`len(x)` · `str(x)` · `int("42")` (nil if it isn't a number) · `chr(65)` · `ord("A")` ·
`sub(s, start, count)` · `find(s, part)` (-1 if missing) · `upper(s)` · `lower(s)` ·
`split(s, sep)` (spaces if no separator) · `join(list, sep)`

**Lists and bytes**
`list(n, fill)` · `push(l, x)` · `pop(l)` · `insert(l, i, x)` · `remove(l, i)` · `bytes(n)`

**Files** (paths can be relative or absolute)
`rdf(path)` (nil if missing) · `rdb(path)` · `wrf(path, text)` ·
`apf(path, text)` · `isf(path)` · `delf(path)` ·
`la(dir)` (directories end in `/`) · `cwd()` the current directory, e.g. `"/bin/"`

`rdf("notes.txt@2")` (and `rdb`, `isf`) reach an old version of a file in a home folder (see
VERS in the manual). A program asks the person at the keyboard before it reads anything
outside `/bin/`, `/docs/`, `/src/`, `/sys/`, its own folder and the files named on its
command line, and before it changes any file at all. If the answer is no, the function fails
as if the file wasn't there (nil or false), and `/acc/` is never allowed. The rules are in
`src/sys/perm.h`.

**At the prompt only**
`cmd(line)` runs a terminal command and gives back what it printed, as a list of lines

**App mode** (src/sys/appmode.h)
Before a program runs, the whole file is read, so AerOS knows every part of itself the program
can use: built-ins are only reached by name, every name is in the source, and there is no eval.
From the names it works out the program's profile (graphics, sound, video, files, network) and
puts away what the program won't use. At the moment that is the network card's buffers, about
82 KB, as everything else is only in memory while it is used anyway. It happens for every
program, however new, and a part that was put away starts again the next time it is used.
`appmode()` gives back `[parts used, KB put away, KB free]`, e.g.
`[["graphics", "files"], 82, 257990]`, and puts away again anything started since. At the
prompt it puts away everything it can. `MEM` shows what app mode did for the last program.

**Network** (sealed messages, to and from computers the firewall allows: see NETWORK.md)
`nsend(host, port, text)` true if it was sent (host is an address, or a name a rule was made
for) · `nrecv(port, ms)` wait up to ms milliseconds: the message, or nil

**Encrypted files** (AES-256 with a password, the file keeps its name)
`enc(path, password)` · `dec(path, password)` (both true if it worked) ·
`isenc(path)` · `rdenc(path, password)` the contents, without changing the file
(nil if the password is wrong)

**System**
`sleep(ms)` · `time()` (milliseconds since boot) · `random(n)` (0 to n - 1) · `beep(hz)` ·
`play(path)` play a WAV or MP3 file and wait for it to finish (false if it can't be played) ·
`video(path)` play an .mpg video full screen with its sound, until it ends or Esc is pressed
(Space pauses, Left / Right skip 5 seconds); true if it played. It takes the whole screen and
gives it back to the terminal, so a program drawing graphics calls `gopen()` again after it
(see [VIDEO.md](VIDEO.md)) · `exit()` ·
`queue(line)` add a program to the queue the `GO` command runs, e.g. `queue("hello.aer a b")`
or `queue("s:some text")`; returns how many are queued (-1 if the process stack is full) ·
`sysinfo()` everything AerOS knows about the computer, as a list of `[group, name, value]`
rows, e.g. `["Processor", "Speed", "about 2400 MHz"]` (`/bin/sysinfo.aer` shows them)

`args` is the list of words typed after the program's name.

**Animation** (the same bars and spinners AerOS itself uses, see `src/sys/ui.h`)
`progress(x, y, width, done, total, colour)` a bar like `[#####.....]  50%` (the bar is
`width` cells, plus 8 for the brackets and the percentage) ·
`spinner(x, y, style, colour)` the spinner's frame for this moment, style 0 to 4 (line, arrows,
pulse, dots, bounce); returns how many cells it took ·
`frame(fps)` wait until it's time for the next frame, so a loop runs at a steady speed however
long the drawing took. The colours can be left out.

`big(x, y, text, colour, background)` text in big letters, like the splash screen's
"aerOS": each letter is 8 columns wide and 7 rows tall (10 fit across the screen), drawn with
half block characters, so it works on any screen. The colour (WHITE) and background (BLACK)
can be left out; returns how many columns wide it is, e.g. to centre it:
`big((80 - len(title) * 8) / 2, 2, title, ORANGE)`

```
while not haskey() do
    progress(2, 10, 40, time() / 50 % 101, 100)
    spinner(2, 12, 4, LIGHT_CYAN)
    frame(20)
end
```

`/bin/anim.aer` shows them all.

**Graphics**
Call `gopen()` to take over the screen (it returns false if there is no graphics screen) and
`gclose()` to give it back to the terminal, which also happens on its own when the program ends.
Colours are numbers like `0xFF7F00`, or `rgb(255, 127, 0)`. Drawing off the edge of the screen is fine.

`gw()` · `gh()` · `gclear(colour)` · `pixel(x, y, colour)` ·
`rect(x, y, w, h, colour)` filled · `line(x1, y1, x2, y2, colour)` ·
`text(x, y, text, colour, size)` (size 1 is 8 x 16 pixels per character) ·
`image(bytes, x, y, w, h)` draw a BMP picture (8, 24 or 32 bit), scaled to fit `w` x `h` if given
(up to 16384 x 16384, so it can be far bigger than the screen; only the part on screen is drawn) ·
`imgsz(bytes)` its `[width, height]`, or nil if it isn't a BMP

```
gopen()
let picture = rdb("/bin/aeros.bmp")
image(picture, 0, 0)
text(10, 320, "Press a key", 0xFFFFFF, 2)
key()
```

**For games** (smooth movement, keys held down, sounds that overlap)
`gbuf(on)` with `true`, draw into a back buffer (a copy of the screen in memory) instead of
the screen, so nothing shows until `gshow()` · `gshow()` put the finished frame on the screen
in one go, so it never flickers · `keydown(key)` is the key held down right now? A character
(`"a"`, `" "`) or `KEY_LEFT`, `KEY_SPACE`, `KEY_SHIFT`, `KEY_CTRL`, `KEY_ALT`... It doesn't wait
(unlike `key()`) and any number of keys can be down at once ·
`sload(path)` load a WAV or MP3 into memory, ready to play (nil if it can't) ·
`splay(sound, loop, volume)` start it and carry on straight away; `loop` plays it round and round,
`volume` is 0 to 100. Up to 8 sounds play at once, mixed together. Returns a number for:
`sstop(n)` stop it (`sstop()` stops every sound) · `isply(n)` is it still playing?
Sounds stop when the program ends.

Two ways to read keys, for two jobs: `keydown()` for things that last while a key is held
(moving), and `haskey()` / `key()` for single presses (jumping, firing). `keydown()` only
sees the moment it is asked, so a quick tap between two frames can slip past it, while every
press waits for `key()` until it is read.

```
gopen()
gbuf(true)
let jump = sload("/sys/startup.wav")
let x = 100
while not keydown(KEY_ESCAPE) do
    if keydown(KEY_LEFT) then
        x = x - 4
    end
    if keydown(KEY_RIGHT) then
        x = x + 4
    end
    if haskey() and key() == ord(" ") then
        splay(jump)
    end
    gclear(0)
    rect(x, 200, 40, 40, 0xFF7F00)
    gshow()
    frame(60)                # 60 frames a second
end
```

`/bin/move.aer` is a small example of all of these. How they work: the back buffer is in
`src/drivers/video/gfx.c`, held keys in `src/drivers/input/IO.c`, and the mixer in
`src/sys/mixer.c`, which feeds the sound card from the timer interrupt so sound keeps playing
smoothly whatever the program is doing.

`/bin/viewer.aer` is a complete app built from these: a picture picker and viewer that zooms
(`+` / `-`, `0` to fit) by drawing the picture bigger than the screen and moving it around.
`/bin/read.aer` is a bigger one: the Markdown / HTML reader behind the `DOC` command, with its
own parsers, word wrapping and link navigation, all in AerLang.
