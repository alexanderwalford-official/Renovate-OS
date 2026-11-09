# The reader

`read <file>` shows Markdown (`.md`), HTML (`.htm` or `.html`) and plain text files, a
screen at a time. With no file it opens this manual, and so does the `doc` command:
`doc sound` opens `/docs/sound.md`.

## Keys

| Key | Does |
| --- | --- |
| Up / Down | scroll a line |
| Page Up / Page Down, Space | scroll a screen |
| Home / End | the top / the bottom |
| Tab or Right | pick the next link |
| Left | pick the link before |
| Enter | open the picked link |
| Backspace | go back to the page before |
| Q or Esc | leave |

Links to other files open them, links like `#keys` jump to a heading on the page, and web
links (`http://...`) are shown but can't be opened. An encrypted file asks for its password
first.

## Markdown

The reader understands the common parts of Markdown:

| You write | You get |
| --- | --- |
| `# Title`, `## Section` | headings |
| `**bold**`, `*italic*` | **bold** and *italic* |
| a word in backticks | `code` |
| `[text](page.md)` | a link |
| `- item` or `1. item` | lists, indent by 2 for more levels |
| `> words` | a quote |
| three backticks | a code block, until the next three |
| `---` | a line across the page |
| lines starting with a bar | a table, like this one |

Here is how some of it looks:

> A quote is shown with a bar down its left side.

1. a numbered list
2. with a second item
   - and a list inside it

---

## HTML

The reader reads headings (`<h1>` to `<h6>`), paragraphs, `<br>`, lists (`<ul>`, `<ol>`,
`<li>`), `<b>`, `<i>`, `<code>`, `<pre>`, `<blockquote>`, `<hr>`, links (`<a href>`) and
tables, plus the usual `&amp;` style characters. Scripts, styles and anything it doesn't
know are left out. Open the [HTML sample](sample.htm) to see it.

## How it works

The reader is an AerLang program, `/bin/read.aer`, about 1200 lines with comments. It works in
three steps:

```
file --parse--> blocks --layout--> screen lines --view--> what you see
```

1. **parse**: the file becomes a list of blocks (headings, paragraphs, list items, code...).
   Markdown and HTML each have their own parser, and both make the same kind of blocks.
2. **layout**: the blocks are word wrapped into lines of coloured pieces of text.
3. **view**: the lines are drawn a screen at a time, and the keys move around them.

Because it is a normal AerLang program, you can change it: `edt /bin/read.aer`. The
colours are at the top of the file.
