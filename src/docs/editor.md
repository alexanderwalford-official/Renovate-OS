# The editor

`edit <file>` opens any text file. If the file doesn't exist yet it is made when you first
save.

```
 /bin/hello.ren *                                           Line 3, Col 7
 let name = input("Name? ")
 print("Hello " + name)
 ~
 ^S Save   ^R Run   ^Q Quit
```

The top bar shows the file (a `*` means there are unsaved changes) and where the cursor is.

## Keys

| Key | Does |
| --- | --- |
| Ctrl+S | save |
| Ctrl+R | save, then run the program (RenLang files only) |
| Ctrl+Q | quit, press it twice to throw away unsaved changes |
| arrows | move around |
| Home / End | start / end of the line |
| Page Up / Down | a screen at a time |
| Tab | spaces up to the next multiple of 4 |
| Enter | new line, keeping the indentation of the one above |

RenLang files (`.ren`) are coloured in: keywords, strings, numbers and comments each get
their own colour.

Encrypted files can't be edited, because changing them would break their seal. `decrypt`
them first.

To read a file without risking any changes, use the [reader](reader.md) instead.
