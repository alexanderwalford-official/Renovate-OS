/*
    editor.h
    A full screen text editor, so programs can be written on AerOS itself.

        edit hello.ren

    Arrow keys, Home / End and Page Up / Down move around. Ctrl+S saves, Ctrl+R saves and runs
    the program (for .ren files), Ctrl+Q quits. RenLang files get their keywords, strings,
    numbers and comments coloured in.
*/
#ifndef EDITOR_H
#define EDITOR_H

// edit a file (relative or absolute path), it is created when first saved if it doesn't exist
void editor_open (const char* path);

#endif /* EDITOR_H */
