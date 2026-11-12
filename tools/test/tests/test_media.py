"""Sound and video. QEMU records everything AerOS's sound card plays to a WAV file, which is then
checked for the notes that should be in it."""

import math
import struct
import time
import wave

from aeros import GUEST_PROMPT


def make_tone(path, hz, seconds, rate, channels, width):
    """Write a WAV file of a sine wave: width is bytes per sample (1 is unsigned 8 bit, 2 and 3
    are signed, like the files people really have)."""
    frames = bytearray()
    for i in range(int(rate * seconds)):
        level = math.sin(2 * math.pi * hz * i / rate) * 0.6
        if width == 1:
            sample = bytes([int(128 + level * 127)])
        elif width == 2:
            sample = struct.pack('<h', int(level * 32767))
        else:
            sample = struct.pack('<i', int(level * 8388607))[:3]
        frames += sample * channels
    with wave.open(path, 'wb') as w:
        w.setnchannels(channels)
        w.setsampwidth(width)
        w.setframerate(rate)
        w.writeframes(bytes(frames))


def notes_in(path):
    """The notes in a recording: for each quarter second that isn't silent, its pitch in Hz,
    from how often the wave crosses zero."""
    with wave.open(path) as w:
        rate, channels, count = w.getframerate(), w.getnchannels(), w.getnframes()
        samples = struct.unpack(f'<{count * channels}h', w.readframes(count))[0::channels]
    notes = []
    window = rate // 4
    for start in range(0, len(samples) - window + 1, window):
        part = samples[start:start + window]
        loudness = (sum(s * s for s in part) / len(part)) ** 0.5
        if loudness < 300:
            continue
        crossings = sum(1 for a, b in zip(part, part[1:]) if (a < 0) != (b < 0))
        notes.append(crossings / 2 / (len(part) / rate))
    return notes


def heard(notes, hz):
    """Was a note within 3% of hz in the recording?"""
    return any(abs(n - hz) <= hz * 0.03 for n in notes)


def test_sound_formats(t):
    make_tone(t.path('cd.wav'), 440, 1.0, 44100, 2, 2)      # CD quality: 16 bit stereo
    make_tone(t.path('low.wav'), 1000, 1.0, 11025, 1, 1)    # 8 bit mono, a low rate
    make_tone(t.path('hi.wav'), 660, 1.0, 96000, 2, 3)      # 24 bit at 96 kHz
    with open(t.path('bad.wav'), 'w') as f:
        f.write('not a sound at all')
    disk = t.disk(files={'/cd.wav': t.path('cd.wav'), '/low.wav': t.path('low.wav'),
                         '/hi.wav': t.path('hi.wav'), '/bad.wav': t.path('bad.wav'),
                         '/tone.mp3': t.files + '/tone523.mp3'})
    recording = t.path('out.wav')
    aeros = t.boot(disk=disk, sound=recording)
    aeros.first_login()

    # PLY plays each one to the end before the prompt comes back
    for name in ['/cd.wav', '/low.wav', '/hi.wav', '/tone.mp3']:
        aeros.run('ply ' + name)
    aeros.cls()
    aeros.run('ply /bad.wav')
    aeros.expect("Can't play")
    aeros.quit()

    notes = notes_in(recording)
    for hz in [440, 1000, 660, 523]:
        if not heard(notes, hz):
            aeros.fail(f'no {hz} Hz note in the recording (heard {sorted(set(round(n) for n in notes))})')


def test_startup_sound(t):
    recording = t.path('out.wav')
    aeros = t.boot(sound=recording)
    aeros.wait_for(GUEST_PROMPT, 60)
    time.sleep(3)   # the chime plays in the background, while the boot carries on
    aeros.quit()
    if not notes_in(recording):
        aeros.fail('the startup sound was not heard')


def test_video(t):
    # /bin/sample.mpg is built in. It plays full screen, and Escape stops it
    aeros = t.boot(graphics=True, sound=True)
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.ask('ply /bin/sample.mpg', 3)
    first = aeros.screenshot('playing')
    time.sleep(1)
    if aeros.screenshot('later')[2] == first[2]:
        aeros.fail('the video is not moving')
    before = aeros.prompts()
    aeros.key('esc')
    aeros.wait_prompt(before, 20)
    # the player says how it went on the serial port
    aeros.expect_printed('video: /bin/sample.mpg', 'pictures shown')
