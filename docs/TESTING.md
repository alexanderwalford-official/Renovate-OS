# Testing AerOS

AerOS's tests boot it in QEMU and use it the way a person would: they type commands, press
keys, move the mouse, and read what comes back on the screen. Nothing inside AerOS knows it is
being tested, so a test passing means AerOS really does that.

## Running them

You need QEMU, Python 3 and mtools (which makes the FAT disk images), on Linux or WSL:

```
sudo apt install qemu-system-x86 mtools
```

Then, from `src/`:

| Type | To |
| --- | --- |
| `make test` | build AerOS, then run every test, as many at once as you have processor cores |
| `make test T=why` | only the tests with `why` in their name |
| `make test J=1` | one at a time (or `J=4` for four), if running them all at once is too much |
| `python3 ../tools/test/run.py -l` | list the tests |

Each test prints `PASS`, `FAIL` or `SKIP` with how long it took, and a summary comes at the
end. `make test` fails (exit code 1) if any test failed, so it can be used before a commit,
along with `make check`, which checks the code keeps the conventions WHY and SRC rely on.

On a computer with plenty of cores the whole suite takes under a minute, which is about as long
as its slowest test (the file versions test waits out an 11 second gap on purpose). One at a time
it takes about six minutes.

**Faster still, with KVM.** QEMU normally imitates the processor in software. If your Linux
user may use `/dev/kvm`, the tests switch KVM on by themselves and AerOS runs on the real
processor. Most Linux computers have it, and so does WSL 2 on Windows 11. If `ls -l /dev/kvm`
shows it but you aren't in its group: `sudo usermod -aG kvm $USER`, then log in again (in WSL,
`wsl --shutdown` from Windows and open it again). `AEROS_TEST_KVM=0` turns it off.

When a test fails, the report says what it expected and shows the screen as it was. Its folder
is kept in `tools/test/out/<test name>/`, with AerOS's serial log (everything AerOS printed)
and any screenshots, so you can see what happened.

The network test and the test of the decryptor in `docs/SECURITY.md` need Python's
`cryptography` package (`pip install cryptography`), like `tools/aernet.py`. Without it, they
are skipped rather than failed.

## What is tested

| File | Tests |
| --- | --- |
| `test_basics.py` | booting, commands, files without a disk, "did you mean" |
| `test_terminal.py` | usage messages, Tab completion, Page Up / Page Down, and the editor (save, run, quit) |
| `test_aerlang.py` | the language (`files/langtest.aer` checks every statement and expression), and AerLang at the prompt |
| `test_explain.py` | WHY, SRC, SHOW and the read-only `/src/` folder |
| `test_processes.py` | RUN, and the queue: QUE, GO, Ctrl+C, `queue()` |
| `test_disk.py` | a real FAT disk: accounts and logging in, file versions, encryption, DEL on big files and folders, the wipe after three wrong passwords, a full disk reported rather than ignored, permissions, and changing files in place (`files/pages.aer`, on FAT16, FAT32 and in memory) |
| `test_apps.py` | the DOC reader, the animation demo, and how built-in apps are installed and kept up to date on a disk |
| `test_crypto.py` | the boot's crypto self-test, and the decryptor printed in `docs/SECURITY.md` run on a file AerOS encrypted |
| `test_media.py` | WAV files in three formats and an MP3 (recorded and checked for the right notes), the startup sound, and video |
| `test_input.py` | the mouse in AerLang, and held keys |
| `test_games.py` | Maze 3D, Snake (and its high scores) and `move.aer`, booted from the ISO in graphics mode |
| `test_database.py` | AerDB from AerLang (`files/aerdb.aer`: every statement, many pages, reopening, COMPACT), the SQL command, and SHOW on a query |
| `test_network.py` | the firewall and sealed messages, against `tools/aernet.py` |

Every test that writes to a disk finishes by checking the disk is still a consistent FAT volume:
`tools/test/fatcheck.py` walks every folder and checks the FAT copies agree, each file's
clusters match its size, and no cluster is lost or used twice. It also runs on its own:
`python3 tools/test/fatcheck.py disk.img`.

## How it works

`tools/test/aeros.py` starts QEMU with no window and talks to it through QEMU's **monitor**, a
command line for QEMU itself, over a local network socket:

- `sendkey` presses keys, and `mouse_move` / `mouse_button` work the mouse.
- `pmemsave` copies the VGA text screen (memory at `0xB8000`, two bytes a character) to a file,
  which is how the screen is read.
- `screendump` saves the screen's pixels, for graphics.
- Everything AerOS prints also goes to its serial port, which QEMU writes to `serial.log`.

Most tests boot `main.elf` straight away (`-kernel`), which leaves the screen in VGA text mode
so it can be read. QEMU also gives the kernel the command line `fast` (`-append fast`), which
AerOS reads from the multiboot information (`boot_has_word` in `src/sys/boot.c`): it skips the
splash screen's 4 second wait and the boot log's pause, so a test reaches the prompt in about a
second. The graphics tests boot `main.iso` instead, where GRUB sets up a graphics screen: they
read the serial log and check the pixels of screenshots.

The tests never sleep for a guessed time after typing a line. Every prompt AerOS shows also goes
to the serial log, so `run()` waits until a new one appears: a quick command moves on straight
away, and a slow one is waited for. Keys go to QEMU all at once and QEMU presses them in order,
10 ms each.

Every test runs in a folder of its own with its own QEMU on a free port, so tests can run side
by side without getting in each other's way. Disks are made fresh for each test with
`make_disk()`, so they never touch your own disk image or files.

## Writing a test

A test is a function whose name starts with `test_`, in a file in `tools/test/tests/` whose
name starts with `test_`. It is given `t`, and boots AerOS with `t.boot()`:

```python
from aeros import GUEST_PROMPT

def test_sums(t):
    aeros = t.boot()                      # the kernel, in text mode, with no disk
    aeros.wait_for(GUEST_PROMPT)          # wait for the prompt
    aeros.cls()                           # clear the screen, so only the answer is checked
    aeros.run('6 * 7')                    # type it and press Enter
    aeros.expect_line('42')               # a line that is exactly "42"
```

The machine (`aeros.py` explains each one):

| Use | For |
| --- | --- |
| `t.boot(graphics=True)` | the ISO, in graphics mode |
| `t.boot(disk=t.disk())` | with a new, empty AEROS disk (`files={'/bin/x.aer': path}` copies files onto it) |
| `aeros.first_login()` | make the first account on a new disk, and wait for its prompt |
| `aeros.run(line)` | type a line, press Enter, and wait for the next prompt |
| `t.boot(sound='out.wav')` | with a sound card, recording everything AerOS plays (`sound=True` plays nowhere) |
| `aeros.ask(line)`, `aeros.answer(text)` | a line that asks a question or keeps running (an app), and each answer: `answer()` waits for AerOS to ask ("Password: "...) before it types |
| `aeros.wait_prompt(since)` | wait for the next prompt after a key ends a program (`since = aeros.prompts()` first) |
| `aeros.wait_gone(text)` | wait until text is no longer showing |
| `aeros.type(text)` | just type |
| `aeros.login(name, password)` | log in at the login screen |
| `aeros.key('esc')`, `aeros.key('up', 800)` | press a key, or hold it for 800 ms |
| `aeros.mouse_move(dx, dy)`, `aeros.mouse_buttons(1)` | the mouse |
| `aeros.wait_for(text)` | wait until text is showing (up to a minute) |
| `aeros.expect(text, ...)`, `aeros.expect_not(...)` | text must (or must not) be showing |
| `aeros.expect_line(line, ...)` | a whole line must say exactly this |
| `aeros.expect_printed(text, ...)` | printed at some point, even if it has scrolled off the screen |
| `aeros.expect_order(a, b, ...)` | these must show, in this order |
| `aeros.screen()`, `aeros.serial()` | the text screen, or everything printed so far |
| `aeros.screenshot()` | `(width, height, pixels)`, three bytes a pixel |
| `aeros.fail(message)` | stop the test, showing the screen |
| `t.check_disk(disk)` | fail unless the disk is a consistent FAT volume (stop the machine first) |
| `read_disk_file(disk, path)`, `write_disk_file(...)`, `delete_disk_file(...)` | a file on a disk image, between boots |
| `t.path(name)`, `t.repo('src', ...)`, `t.files` | a file in the test's folder, in the repository, or in `tools/test/files/` |

`run()` only waits for a prompt, so a line that asks something (a password, Y / N) or keeps
running (a game, `lsn`) never gives one: use `ask()`. Then wait for what should happen with
`wait_for()` rather than sleeping: it carries on as soon as the text appears, and fails clearly
if it never does. Fixed sleeps are what make tests fail when the computer is busy, because
everything takes longer than the sleep allowed for.

Raise `Skip('why')` (from `aeros`) when a test can't run on this computer, as the network test
does without the `cryptography` package.
