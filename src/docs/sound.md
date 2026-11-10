# Sound

AerOS plays sound through an **AC'97** sound card. QEMU and VirtualBox have one, and so do
most PCs from about 1997 to 2006. The boot log says whether one was found:

```
[ OK ] Sound: AC'97 (PCI 00:04.0)
```

## Playing files

```
ply song.mp3
ply /sys/startup.wav
```

Esc or Ctrl+C stops it. In AerLang, `play("song.mp3")` plays a file and waits until it
ends (it gives `false` if it couldn't be played). `ply` plays [videos](video.md) too.

Games can play several sounds at once, in the background, with `sload` and `splay`: see
[AerLang](aerlang.md).

| Format | Plays |
| --- | --- |
| WAV | 8, 16, 24 or 32 bit, mono or stereo, any rate |
| MP3 | any bit rate, ID3 tags are skipped |

The whole file is decoded into memory first (about 10 MB a minute), then played, so it never
stutters. If the card can't play the file's sample rate, AerOS converts it.

## The startup sound

The chime at the splash screen is `/sys/startup.wav`.

- **To change it**, replace that file with any WAV, or an MP3 with the same name. AerOS
  looks inside the file to see what it is, not at the name.
- **To turn it off**, replace it with an empty file: `mkf /sys/startup.wav` after deleting it.
- Deleting it brings the built-in chime back at the next start.

It plays in the background, so pressing X for the boot options still works straight away.

## How it works

The sound card plays straight from memory by itself. AerOS gives it a list of 32 pieces of
the sound, and each time the card finishes one, AerOS points that slot at the next piece:

```
slot:    0     1     2     3    ...   31
piece:   32    33    2     3    ...   31
                     ^ playing now
```

So the CPU only does a little work every few milliseconds, however long the song is.

The full story is in `docs/AUDIO.md` in the AerOS source.
