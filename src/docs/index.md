# The AerOS manual

Welcome to AerOS, a small operating system written to be read and changed. This manual lives
in `/docs/` on your computer and is shown by the reader app, `/bin/read.aer`.

**Moving around:** the arrow keys and Page Up / Page Down scroll. **Tab** picks the next link
(shown in blue), **Enter** opens it, and **Backspace** comes back. **Q** leaves the reader.

## Start here

- [Getting started](start.md): booting, the modes, logging in, your first commands
- [Terminal commands](commands.md): everything you can type at the prompt
- [Files and folders](files.md): where things are kept, paths and file names

## What makes AerOS different

- [Ask AerOS why](why.md): WHY explains what went wrong, SRC shows the code behind anything
- [The prompt speaks AerLang](shell.md): variables, sums and code right at the prompt
- [Files remember](versions.md): old versions of your files, and UNDO
- [Programs ask first](perms.md): no app touches your files without asking you

## Making things

- [AerLang](aerlang.md): the built-in programming language
- [The editor](editor.md): writing files and programs with EDT
- [The reader](reader.md): this app, and the Markdown and HTML it understands

## Features

- [Accounts and encryption](security.md): logins, passwords and locked files
- [Sound](sound.md): playing WAV and MP3 files, and the startup sound
- [Video](video.md): playing .mpg videos, and making them
- [The network](network.md): sealed messages, and a firewall that starts with everything blocked
- [How AerOS works](inside.md): what happens inside, for the curious

## Reading other files

The reader opens any Markdown (`.md`), HTML (`.htm`) or text file:

```
read notes.md
read /bin/hello.aer
read /docs/sample.htm
```

The files here are replaced with fresh copies when AerOS starts, so they always match the
version you are running. Your own files in `/docs/` with other names are left alone.
