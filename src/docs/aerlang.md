# AerLang

AerLang is the programming language built into AerOS. A program is a text file ending in
`.aer`. Every app in `/bin/`, including this reader, is an AerLang program you can open with
`read` and change with `edt`.

## Running programs

| Type | To |
| --- | --- |
| `hello` | run `hello.aer` from here, or from `/bin/` |
| `hello.aer a b` | the same, with `args` set to `["a", "b"]` |
| `aer` | the prompt: type code and see the result, `exit` to leave |
| `edt app.aer` then Ctrl+R | write a program and run it |

**Ctrl+C** stops a program that is stuck.

Programs from before AerLang got its name end in `.ren`. They still run if you type the
extension too: `old.ren`.

## The language

```
# a comment runs to the end of the line
let name = "AerOS"              # make a variable
name = name + "!"               # change it
print("Hello", name)            # Hello AerOS!

if len(name) > 3 then
    print("long")
elif len(name) == 3 then
    print("three")
else
    print("short")
end

for i = 1 to 10 do              # add "step 2" to count in twos
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
print(area(3, 4))               # 12
```

- A statement ends at the end of its line. Short blocks fit on one line:
  `if x then print(x) end`
- Variables are made with `let`. Using a name that was never made is an error, which
  catches typing mistakes.
- Variables made with `let` inside a function belong to it. Everything else is global.
- Functions are made at the top level and can be used before the line that makes them.

## Values

| Type | Examples | Notes |
| --- | --- | --- |
| number | `42`, `-7`, `0xFF` | whole numbers, `/` drops the remainder |
| string | `"hi\n"` | `s[0]` is the first character |
| list | `[1, "two", [3]]` | `l[0] = 5` changes an item |
| bytes | `bytes(100)` | raw bytes, e.g. from `rdb` |
| bool | `true`, `false` | |
| nil | `nil` | nothing |

`nil`, `false` and `0` count as false. `and` / `or` give back one of their sides, so
`input("Name? ") or "friend"` works. `+` joins strings: `"Score: " + 10`.

## Built-in functions

**Screen and keys:** `print(...)`, `write(...)` (no new line), `input(prompt)`,
`inpw(prompt)` (shows `*`), `cls()`, `color(fg, bg)`, `at(x, y)`, `key()` (waits,
gives the key's code), `haskey()`.

Colours: `BLACK`, `DARK_BLUE`, `GREEN`, `CYAN`, `RED`, `PURPLE`, `ORANGE` (the logo
orange, also called `BROWN`), `BRIGHT`, `DARK_GRAY`, `LIGHT_BLUE`, `LIGHT_GREEN`,
`LIGHT_CYAN`, `LIGHT_RED`, `LIGHT_PURPLE`, `YELLOW`, `WHITE`. Keys: `KEY_UP`,
`KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT`, `KEY_HOME`, `KEY_END`, `KEY_PGUP`, `KEY_PGDN`,
`KEY_DELETE`, `KEY_ENTER`, `KEY_BACKSPACE`, `KEY_TAB`, `KEY_ESCAPE`.

**Text:** `len(x)`, `str(x)`, `int("42")` (nil if not a number), `chr(65)`, `ord("A")`,
`sub(s, start, count)`, `find(s, part)` (-1 if missing), `upper(s)`, `lower(s)`,
`split(s, sep)`, `join(list, sep)`.

**Lists:** `list(n, fill)`, `push(l, x)`, `pop(l)`, `insert(l, i, x)`, `remove(l, i)`,
`bytes(n)`.

**Files:** `rdf(path)` (nil if missing), `rdb(path)`, `wrf(path, text)`,
`apf(path, text)`, `isf(path)`, `delf(path)`, `la(dir)`,
`cwd()`.

The names are built like the terminal's commands: `rd` read, `wr` write, `ap` append,
`del` delete, `is` "is there one?", then `f` file or `b` bytes. So `rdf` reads a file and
`isf` asks if a file is there. `enc` / `dec` encrypt and decrypt, and `g` means graphics.

**Encrypted files:** `enc(path, password)`, `dec(path, password)`,
`isenc(path)`, `rdenc(path, password)` (nil if the password is wrong).

**System:** `sleep(ms)`, `time()` (ms since start), `random(n)` (0 to n - 1), `beep(hz)`,
`play(path)`, `exit()`. `args` is the list of words typed after the program's name.
`sysinfo()` gives everything AerOS knows about the computer as a list of
`[group, name, value]`, e.g. `["Processor", "Speed", "about 2400 MHz"]`.
`queue("hello.aer")` adds a program (with its arguments) to the queue that `go` runs, and
gives back how many are queued.

**Animation:** `progress(x, y, width, done, total, colour)` draws a bar like
`[#####.....]  50%`, `spinner(x, y, style, colour)` draws a spinner as it should look right
now (styles 0 to 4: line, arrows, pulse, dots, bounce), and `frame(fps)` waits for the next
frame, so a loop runs at a steady speed. The colour can be left out.

**Big text:** `big(x, y, text, colour, background)` writes text in big letters like the
splash screen's "aerOS": each letter is 8 columns wide and 7 rows tall, so 10 fit across the
screen. It gives back how many columns wide it is, handy for centring:

```
let title = "Hello"
big((80 - len(title) * 8) / 2, 2, title, ORANGE)
```

```
let done = 0
while done <= 100 do
    progress(2, 10, 40, done, 100)
    spinner(2, 12, 4)
    done = done + 1
    frame(20)                   # 20 frames a second
end
```

**Graphics:** `gopen()` takes over the screen and `gclose()` gives it back.
Colours are numbers like `0xFF7F00` or `rgb(255, 127, 0)`.
`gw()`, `gh()`, `gclear(c)`, `pixel(x, y, c)`, `rect(x, y, w, h, c)`,
`line(x1, y1, x2, y2, c)`, `text(x, y, s, c, size)`, `image(bytes, x, y, w, h)` (a BMP
picture), `imgsz(bytes)`. `w` x `h` can be bigger than the screen and `x`, `y`
negative: only the part on the screen is drawn, which is how the viewer zooms in.

```
gopen()
image(rdb("/bin/aeros.bmp"), 0, 0)
text(10, 320, "Press a key", 0xFFFFFF, 2)
key()
```

**For games:** `gbuf(true)` draws into a back buffer out of sight and `gshow()` puts
each finished frame on the screen, so moving things don't flicker. `keydown(k)` says if a
key is held right now (`"a"`, `KEY_LEFT`, `KEY_SPACE`, `KEY_SHIFT`...), without waiting.
`sload(path)` loads a WAV or MP3, and `splay(sound, loop, volume)` starts it and carries on
at once: up to 8 sounds play together. `sstop(n)` stops one, `isply(n)` asks if it still
plays. Use `keydown` for things that last (moving) and `haskey()` / `key()` for single
presses (jumping): every press waits there until it is read, so a quick tap is never missed.
`/bin/move.aer` uses them all.

```
gopen()
gbuf(true)
let x = 100
while not keydown(KEY_ESCAPE) do
    if keydown(KEY_RIGHT) then
        x = x + 4
    end
    gclear(0)
    rect(x, 100, 40, 40, 0xFF7F00)
    gshow()
    frame(60)
end
```

## Programs to learn from

| File | Shows |
| --- | --- |
| `/bin/hello.aer` | input, functions, lists, loops |
| `/bin/guess.aer` | random numbers and a game loop |
| `/bin/viewer.aer` | graphics, pictures and a menu |
| `/bin/move.aer` | smooth movement, held keys and mixed sounds |
| `/bin/read.aer` | this reader: text handling, a whole app |
| `/bin/sysinfo.aer` | system information: lists, layout and scrolling |
| `/bin/anim.aer` | spinners, progress bars and moving text |
