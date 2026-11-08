# RenLang

RenLang is the programming language built into AerOS. Programs are text files ending in `.ren`.
The interpreter runs inside the kernel, so a program can use the screen, keyboard and files directly.

## Running programs

| You type | What happens |
| --- | --- |
| `hello` | runs `hello.ren` from the current directory, or from `/bin/` |
| `hello.ren a b` | the same, with `args` set to `["a", "b"]` |
| `run hello.ren` | runs it through the process manager like any other RenLang instruction |
| `ren` | the RenLang prompt: type code and see the result straight away, `exit` to leave |

Press **Ctrl+C** to stop a program that is stuck in a loop.

## Writing programs on AerOS

`edit myapp.ren` opens the editor (the file is made when you first save it). RenLang code is
coloured in, and a new line keeps the indentation of the one above.

| Key | |
| --- | --- |
| Ctrl+S | save |
| Ctrl+R | save and run the program, then press a key to come back |
| Ctrl+Q | quit (press it twice to throw away unsaved changes) |
| arrows, Home, End, Page Up / Down | move around |

Programs can also be written on another computer and copied onto the AerOS disk with
`make disk-put FILE=myapp.ren` (see `src/README.md`).

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
| bytes | `bytes(100)` | a block of raw bytes (0 to 255), e.g. a file read with `read_bytes` |
| bool | `true`, `false` | |
| nil | `nil` | "nothing", e.g. what a function returns without `return` |

`nil`, `false` and `0` count as false. `and` / `or` give back one of their values, so
`input("Name? ") or "friend"` works. `+` joins strings, and turns anything else into text when
one side is a string: `"Score: " + 10`.

### Built-in functions

**Screen and keyboard**
`print(a, b, ...)` print, separated by spaces, then a new line ·
`write(...)` the same without the new line ·
`input(prompt)` read a line ·
`cls()` clear the screen ·
`color(fg, bg)` text colours (`BLACK` ... `WHITE`) ·
`at(x, y)` move the cursor ·
`key()` wait for a key and return its code (`KEY_UP`, `KEY_ENTER`, `KEY_ESCAPE`, or a character code) ·
`haskey()` true if a key is waiting

**Text**
`len(x)` · `str(x)` · `int("42")` (nil if it isn't a number) · `chr(65)` · `ord("A")` ·
`sub(s, start, count)` · `find(s, part)` (-1 if missing) · `upper(s)` · `lower(s)` ·
`split(s, sep)` (spaces if no separator) · `join(list, sep)`

**Lists and bytes**
`list(n, fill)` · `push(l, x)` · `pop(l)` · `insert(l, i, x)` · `remove(l, i)` · `bytes(n)`

**Files** (paths can be relative or absolute)
`read_file(path)` (nil if missing) · `read_bytes(path)` · `write_file(path, text)` ·
`append_file(path, text)` · `file_exists(path)` · `delete_file(path)` ·
`list_files(dir)` (directories end in `/`)

**Files** (continued)
`cwd()` the current directory, e.g. `"/bin/"`

**System**
`sleep(ms)` · `time()` (milliseconds since boot) · `random(n)` (0 to n - 1) · `beep(hz)` · `exit()`

`args` is the list of words typed after the program's name.

**Graphics**
Call `gfx_open()` to take over the screen (it returns false if there is no graphics screen) and
`gfx_close()` to give it back to the terminal, which also happens on its own when the program ends.
Colours are numbers like `0xFF7F00`, or `rgb(255, 127, 0)`. Drawing off the edge of the screen is fine.

`gfx_width()` · `gfx_height()` · `gfx_clear(colour)` · `pixel(x, y, colour)` ·
`rect(x, y, w, h, colour)` filled · `line(x1, y1, x2, y2, colour)` ·
`text(x, y, text, colour, size)` (size 1 is 8 x 16 pixels per character) ·
`image(bytes, x, y, w, h)` draw a BMP picture (8, 24 or 32 bit), scaled to fit `w` x `h` if given ·
`image_size(bytes)` its `[width, height]`, or nil if it isn't a BMP

```
gfx_open()
let picture = read_bytes("/bin/aeros.bmp")
image(picture, 0, 0)
text(10, 320, "Press a key", 0xFFFFFF, 2)
key()
```

`/bin/viewer.ren` is a complete app built from these: a picture picker and viewer in about 100 lines.
