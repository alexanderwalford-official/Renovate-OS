# How text works

## The screen is a grid of cells

The terminal is always **80 columns by 25 rows** of cells. Each cell holds one character in
the 8 x 16 pixel font, with a text colour and a background colour. Columns count from 0 at
the left, rows from 0 at the top.

On a VGA text screen (QEMU's `-kernel`) the graphics card draws the cells itself. On a
framebuffer (every boot through GRUB) AerOS draws them, made bigger to fill the screen. Either
way it is the same grid: `res 1280x720` makes the cells larger on screen, never more of them.

## Printing, and the cursor

`print(...)` in AerLang and `prtf(...)` in C write at the **cursor**, then move it on. At the
bottom the screen scrolls up, and Page Up looks back at what scrolled away.

```
color(YELLOW, BLACK)        # the colours for what comes next
at(10, 5)                   # move the cursor to column 10, row 5
write("hello")              # write without a new line (print adds one)
```

In C: `colour(YELLOW, BLACK)`, `console_set_cursor(10, 5)`, `prtf("hello")`. There is also
`putc(x, y, fg, bg, ch)` and `puts(x, y, fg, bg, text)`, which draw at a place without using
or moving the cursor.

## Big text

`big()` in AerLang (`big_text()` in C) writes big letters out of cells, like the splash
screen's "aerOS". Each letter is **8 columns wide and 7 rows tall**, so 10 fit across:

```
big(2, 1, "aerOS", ORANGE, BLACK)                      # AerLang
big_text(2, 1, ORANGE, BLACK, "aerOS");                // C
```

Things to know:

- **It doesn't move the cursor.** It draws in place, so put the cursor below it before
  printing more, or the text lands on top of the letters:
  `console_set_cursor(0, 1 + BIG_TEXT_ROWS);`
- **No `\n`.** Every character is drawn as a letter. For a second line, call it again 7 rows
  lower.
- **To centre it,** use its width: `big((80 - len(title) * 8) / 2, 2, title, ORANGE)`.

## Other sizes of big text

Made of cells, big text has one size. On a framebuffer, C can draw it at any size in real
pixels, as a percentage of `big_text`'s:

```
uint8_t rows;
int cols = big_text_sized(2, 1, ORANGE, BLACK, "aerOS", 75, &rows);   // three quarters
console_set_cursor(0, 1 + rows + 1);
```

It says how many columns and rows it covered, rounded up to whole cells. On a VGA text screen
it is drawn full size, as `big_text`. It is drawn over the cells rather than into them, so it
vanishes if those cells are redrawn (when the screen scrolls, or with Page Up): good for a
banner or a title screen. The terminal's banner uses it.

## Text in graphics

After `gopen()` the screen is pixels, not cells, and text can be any whole size:

```
gopen()
text(20, 20, "Score: 10", rgb(255, 255, 255), 3)   # x, y in pixels, 3 times the font's size
```

`text()` draws the 8 x 16 font with each font pixel as a `scale` x `scale` square, so
`scale` 2 is 16 x 32 pixels a letter, 3 is 24 x 48, and so on. In C it is `gfx_text`.

| You want | Use | Size |
| --- | --- | --- |
| normal terminal text | `print`, `write`, `prtf` | one cell a letter |
| a big title on the text screen | `big` / `big_text` | 8 x 7 cells a letter |
| a title any size (framebuffer) | `big_text_sized` (C) | a percentage of that |
| text in a game or picture | `text` / `gfx_text` | any whole scale |

See also: [AerLang](aerlang.md), [How AerOS works](inside.md).
