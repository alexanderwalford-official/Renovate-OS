# Video

AerOS plays **MPEG-1** videos, the `.mpg` files that DVDs' older cousin, the Video CD, used.
Its patents have all run out, so it is free for anyone to use. Try the built-in clip:

```
ply /bin/sample.mpg
```

The picture fills as much of the screen as it can, keeping its shape, and the sound plays
along with it. Videos need a graphics screen: started from the ISO, AerOS has one.

## While it plays

| Key | Does |
| --- | --- |
| Esc or Ctrl+C | stop |
| Space | pause, and carry on |
| Left / Right | 5 seconds back / on |
| any other key | show the time and the keys for a few seconds |

In AerLang, `video("clip.mpg")` plays a video the same way and gives `true` when it played.

## Making .mpg files

**ffmpeg** turns almost any video into one AerOS can play:

```
ffmpeg -i film.mp4 -c:v mpeg1video -q:v 4 -s 640x360 -c:a mp2 -f mpeg film.mpg
```

`-q:v` is the quality (2 is best, 31 smallest) and `-s` the size. Then copy the file onto
the AEROS disk, for example with `make disk-put FILE=film.mpg` in the AerOS source folder.

## How big?

Everything is decoded by the processor, one picture at a time. As a guide:

| Size | In QEMU | On a real PC |
| --- | --- | --- |
| 320 x 240, 480 x 270 | smooth | smooth |
| 640 x 360 | smooth | smooth |
| 1280 x 720 | about 7 pictures a second | depends on the processor |

When a video is too big for the computer, the sound still plays smoothly and the picture
skips frames to keep up with it.

## How it works

The video is never loaded whole: AerOS reads 64 KB of it at a time, so a film can be far
bigger than the computer's memory. The decoder is **pl_mpeg** (MIT licence). Its sound goes
to the [sound](sound.md) mixer, and the sound is the clock: each picture is shown when the
sound reaches its time. The full story is in `docs/VIDEO.md` in the AerOS source.
