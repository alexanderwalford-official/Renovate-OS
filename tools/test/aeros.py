"""
aeros.py - start AerOS in QEMU and drive it like a person would, for the tests in tests/

QEMU runs with no window. Its "monitor" (a text command line for QEMU itself, reached here over
a TCP socket) is how the tests press keys (sendkey), move the mouse (mouse_move), read the
screen (pmemsave copies the VGA text memory at 0xB8000 to a file) and take screenshots
(screendump). Everything AerOS prints also goes to its serial port, which QEMU writes to a file,
so text can be read that way even when the screen is in graphics mode.

A test asks for a machine with t.boot(...) (see run.py), then uses the methods below:

    aeros = t.boot()                       # boots the kernel in VGA text mode, no disk
    aeros.wait_for('guest@0//: ')          # wait until that text is on the screen
    aeros.run('2 + 2')                     # type a line and press Enter
    aeros.expect_line('4')                 # a line of the screen must be exactly "4"

Anything that doesn't turn out as expected raises Failure, with the screen as it was, so the
report shows what AerOS actually said.
"""

import os
import re
import socket
import subprocess
import time

# characters that need a different name (or Shift) for QEMU's sendkey
KEYMAP = {
    ' ': 'spc', '\n': 'ret', '\b': 'backspace', '/': 'slash', '.': 'dot', '-': 'minus',
    ',': 'comma', '=': 'equal', ';': 'semicolon', "'": 'apostrophe', '[': 'bracket_left',
    ']': 'bracket_right', '\\': 'backslash', '`': 'grave_accent',
    ':': 'shift-semicolon', '_': 'shift-minus', '!': 'shift-1', '@': 'shift-2', '#': 'shift-3',
    '$': 'shift-4', '%': 'shift-5', '^': 'shift-6', '&': 'shift-7', '*': 'shift-8',
    '(': 'shift-9', ')': 'shift-0', '?': 'shift-slash', '~': 'shift-grave_accent',
    '+': 'shift-equal', '"': 'shift-apostrophe', '<': 'shift-comma', '>': 'shift-dot',
    '{': 'shift-bracket_left', '}': 'shift-bracket_right', '|': 'shift-backslash',
}

# the prompt AerOS shows with no disk (nobody logged in, the temporary file system)
GUEST_PROMPT = 'guest@0//: '

# Every prompt AerOS shows when it is ready for the next line: the terminal's
# ("tester@0//home/tester/: "), AER's ("aer> "), and "   ... " inside a block that isn't
# finished. A new one in the serial log means the line before it has finished
PROMPT = re.compile(r'\S+@\d+//\S*: |aer> |   \.\.\. ')

# The questions AerOS asks for a name or a password: "User name: " (a new account), "Username: "
# (logging in), "Password: ", "Again: ",
# "Your password: ", "New password: ", "Password both ends will use: " (see answer())
QUESTION = re.compile(r'(?:User ?name|[Pp]assword|Again)[^:\n]*: ')

# how long QEMU holds each typed key down, in milliseconds. QEMU presses keys one after another
# from a queue, so this sets the typing speed (its own default, 100, is ten keys a second)
KEY_HOLD_MS = 10


class Failure(Exception):
    """A check that didn't pass. The message says what was expected and shows the screen."""


class Skip(Exception):
    """The test can't run here (something it needs is missing). The message says what."""


def free_port():
    """A TCP port nothing is using right now, so several machines can run side by side."""
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class AerOS:
    """One running copy of AerOS in QEMU."""

    def __init__(self, workdir, kernel=None, iso=None, disk=None, memory=256, network=None, sound=None):
        """Start QEMU. Give either kernel (main.elf: VGA text mode, the screen can be read) or iso
        (main.iso: GRUB sets up graphics, read text with serial()). disk is a disk image to
        attach (see make_disk), network a QEMU -nic setting. sound gives AerOS an AC'97 sound
        card: a file name records everything it plays to that WAV file, True plays nowhere."""
        self.workdir = workdir
        self.serial_path = os.path.join(workdir, 'serial.log')
        self.graphics = iso is not None
        self.answered = 0   # questions answered so far (see answer())
        port = free_port()
        args = [os.environ.get('QEMU', 'qemu-system-i386'), '-display', 'none', '-m', str(memory),
                '-monitor', f'tcp:127.0.0.1:{port},server,nowait',
                '-serial', f'file:{self.serial_path}', '-no-reboot']
        # "fast" on the kernel's command line skips the splash screen's 4 second wait and the
        # boot log's pause (boot_has_word in src/sys/boot.c). GRUB has its own command line, so
        # the ISO boots as it always does
        args += ['-cdrom', iso] if iso else ['-kernel', kernel, '-append', 'fast']
        # KVM runs AerOS on the real processor instead of QEMU imitating one, which is much
        # faster. It is there on most Linux computers (AEROS_TEST_KVM=0 turns it off)
        if os.access('/dev/kvm', os.R_OK | os.W_OK) and os.environ.get('AEROS_TEST_KVM') != '0':
            args += ['-accel', 'kvm']
        if disk:
            args += ['-drive', f'file={disk},format=raw,if=ide,index=0']
        if network:
            args += ['-nic', network]
        if sound:
            # QEMU's "wav" audio backend writes the card's output to a file instead of speakers
            backend = f'wav,id=snd0,path={sound}' if isinstance(sound, str) else 'none,id=snd0'
            args += ['-audiodev', backend, '-device', 'AC97,audiodev=snd0']
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

        # the monitor socket takes a moment to appear
        for _ in range(100):
            try:
                self.sock = socket.create_connection(('127.0.0.1', port))
                break
            except OSError:
                if self.proc.poll() is not None:
                    raise Failure('QEMU did not start: ' + self.proc.stderr.read().decode(errors='replace'))
                time.sleep(0.1)
        else:
            raise Failure('could not reach the QEMU monitor')
        self.sock.settimeout(0.5)
        self._read()             # the monitor's greeting
        self.sock.settimeout(0.05)

    # ---- the QEMU monitor --------------------------------------------------------------------

    def _read(self):
        """Everything the monitor has said since the last read."""
        data = b''
        try:
            while True:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                data += chunk
        except socket.timeout:
            pass
        return data.decode(errors='replace')

    def monitor(self, command):
        """Run a QEMU monitor command and give back what it printed."""
        self.sock.sendall((command + '\n').encode())
        time.sleep(0.05)
        return self._read()

    # ---- the keyboard and mouse --------------------------------------------------------------

    def key(self, name, hold_ms=None):
        """Press one key by QEMU's name for it: 'ret', 'esc', 'up', 'ctrl-c', 'shift-a'...
        hold_ms keeps it held down that long (for keydown() in games)."""
        self.monitor(f'sendkey {name} {hold_ms or KEY_HOLD_MS}')

    def type(self, text):
        """Type text ('\\n' is Enter). Every key goes to QEMU at once and QEMU presses them in
        order, KEY_HOLD_MS each, so this returns before the typing has finished: wait for what
        it should do (run() and wait_for() do)."""
        names = []
        for ch in text:
            if ch in KEYMAP:
                names.append(KEYMAP[ch])
            elif ch.isupper():
                names.append('shift-' + ch.lower())
            else:
                names.append(ch)
        self.sock.sendall(''.join(f'sendkey {n} {KEY_HOLD_MS}\n' for n in names).encode())
        # the monitor answers every command; read the answers so they can't pile up and stall it
        time.sleep(0.02)
        self._read()

    def prompts(self):
        """How many times AerOS has asked for a line so far (see PROMPT)."""
        return len(PROMPT.findall(self.serial()))

    def run(self, line, settle=None, timeout=60):
        """Type a line and press Enter, then wait until AerOS asks for the next line, however long
        that takes (up to timeout seconds). For a line that asks a question (a password, Y / N)
        or keeps running, give settle instead: it then just waits that many seconds."""
        before = self.prompts()
        self.type(line + '\n')
        if settle is not None:
            time.sleep(settle)
            return
        self.wait_prompt(before, timeout, what=repr(line))

    def wait_prompt(self, since, timeout=60, what='it'):
        """Wait for a new prompt, after `since` of them (from prompts()): the way to wait for a
        program to end after a key, e.g. Escape in a game."""
        start = time.time()
        while self.prompts() <= since:
            if time.time() - start > timeout:
                self.fail(f'{what} did not finish in {timeout}s')
            if self.proc.poll() is not None:
                self.fail(f'QEMU stopped while waiting for {what} to finish')
            time.sleep(0.05)

    def ask(self, line, pause=0):
        """Type a line that asks something (a password, Y / N) or keeps running (an app), without
        waiting for the next prompt. Answer its questions with answer()."""
        self.run(line, settle=pause)

    def questions(self):
        """How many questions AerOS has asked so far (see QUESTION)."""
        return len(QUESTION.findall(self.serial()))

    def answer(self, text, timeout=120):
        """Wait until AerOS asks a question this hasn't answered yet ("Password: "...), then type
        the answer and press Enter. Waiting for the question matters: AerOS throws away keys
        typed before a password question, so typing early would lose them."""
        start = time.time()
        while self.questions() <= self.answered:
            if time.time() - start > timeout:
                self.fail(f'waited {timeout}s for a question to answer with {text!r}')
            if self.proc.poll() is not None:
                self.fail('QEMU stopped while waiting for a question')
            time.sleep(0.05)
        self.answered += 1
        self.type(text + '\n')

    def cls(self):
        """Clear the screen, so the next checks only see what comes after."""
        self.run('cls')

    def mouse_move(self, dx, dy):
        """Move the mouse (right and down are positive)."""
        self.monitor(f'mouse_move {dx} {dy}')

    def mouse_buttons(self, mask):
        """Set which mouse buttons are held: 1 left, 2 right, 4 middle, 0 none."""
        self.monitor(f'mouse_button {mask}')

    # ---- reading what AerOS shows ------------------------------------------------------------

    def screen(self):
        """The 80 x 25 text screen, as 25 lines (VGA text mode only, so boot with kernel=)."""
        path = os.path.join(self.workdir, 'vga.bin')
        if os.path.exists(path):
            os.remove(path)
        # each cell is two bytes in VGA memory: the character, then its colours
        self.monitor(f'pmemsave 0xb8000 4000 "{path}"')
        for _ in range(50):
            if os.path.exists(path) and os.path.getsize(path) == 4000:
                break
            time.sleep(0.05)
        else:
            raise Failure('could not read the screen from QEMU')
        data = open(path, 'rb').read()[0::2]
        rows = []
        for row in range(25):
            cells = data[row * 80:(row + 1) * 80]
            # each row stays 80 characters, spaces and all: the prompt ends in a space
            rows.append(''.join(chr(c) if 32 <= c < 127 else ' ' for c in cells))
        return '\n'.join(rows)

    def serial(self):
        """Everything AerOS has printed so far (its serial port), with Windows line ends removed."""
        try:
            return open(self.serial_path, errors='replace').read().replace('\r', '')
        except FileNotFoundError:
            return ''

    def text(self):
        """What a check looks at: the screen in text mode, the serial log in graphics mode."""
        return self.serial() if self.graphics else self.screen()

    def screenshot(self, name='shot'):
        """The screen's pixels: (width, height, bytes of red, green, blue for each pixel)."""
        path = os.path.join(self.workdir, name + '.ppm')
        if os.path.exists(path):
            os.remove(path)
        self.monitor(f'screendump "{path}"')
        for _ in range(50):
            if os.path.exists(path) and os.path.getsize(path) > 0:
                time.sleep(0.2)   # let QEMU finish writing it
                break
            time.sleep(0.1)
        else:
            raise Failure('could not take a screenshot')
        data = open(path, 'rb').read()
        # a binary PPM: "P6", width, height, the largest value (255), then the pixels
        match = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', data)
        if not match:
            raise Failure('the screenshot is not a PPM picture')
        width, height = int(match.group(1)), int(match.group(2))
        return width, height, data[match.end():]

    # ---- checks ------------------------------------------------------------------------------

    def fail(self, message):
        """Stop the test with this message and what AerOS was showing."""
        shown = '\n'.join(row.rstrip() for row in self.text().split('\n')).strip('\n')
        raise Failure(f'{message}\n----- what AerOS showed -----\n{shown}')

    def wait_for(self, text, timeout=60):
        """Wait until text appears (see text()), giving how many seconds it took."""
        start = time.time()
        while time.time() - start < timeout:
            if text in self.text():
                return time.time() - start
            if self.proc.poll() is not None:
                self.fail(f'QEMU stopped while waiting for {text!r}')
            time.sleep(0.3)
        self.fail(f'waited {timeout}s for {text!r}')

    def wait_gone(self, text, timeout=30):
        """Wait until text is no longer showing."""
        start = time.time()
        while text in self.text():
            if time.time() - start > timeout:
                self.fail(f'{text!r} was still showing after {timeout}s')
            time.sleep(0.1)

    def expect(self, *texts):
        """Each text must be somewhere on the screen (or in the serial log in graphics mode)."""
        shown = self.text()
        for text in texts:
            if text not in shown:
                self.fail(f'expected to see {text!r}')

    def expect_printed(self, *texts):
        """Each text must have been printed at some point since boot (the serial log keeps it
        all, so this also finds text that has scrolled off the top of the screen)."""
        printed = self.serial()
        for text in texts:
            if text not in printed:
                self.fail(f'expected {text!r} to have been printed')

    def expect_not(self, *texts):
        """None of these texts may be showing."""
        shown = self.text()
        for text in texts:
            if text in shown:
                self.fail(f'did not expect to see {text!r}')

    def expect_line(self, *lines):
        """Each of these must be a whole line on the screen, e.g. the answer to a sum."""
        shown = [row.strip() for row in self.text().split('\n')]
        for line in lines:
            if line not in shown:
                self.fail(f'expected a line saying exactly {line!r}')

    def expect_order(self, *texts):
        """These texts must all be showing, in this order."""
        shown = self.text()
        at = 0
        for text in texts:
            found = shown.find(text, at)
            if found < 0:
                self.fail(f'expected {text!r} after the text before it')
            at = found + len(text)

    # ---- common steps ------------------------------------------------------------------------

    def first_login(self, name='tester', password='secret1'):
        """On a new disk AerOS asks for the first account: make one and wait for its prompt."""
        self.wait_for('Create the first account', 60)
        self.answer(name)
        self.answer(password)
        self.answer(password)
        self.wait_for(f'{name}@0//home/{name}/: ', 180)

    def login(self, name='tester', password='secret1'):
        """Log in at the login screen, and wait for the prompt."""
        self.wait_for('Authentication required', 60)
        self.answer(name)
        self.answer(password)
        self.wait_for(f'{name}@0//home/{name}/: ', 120)

    def quit(self):
        """Stop QEMU (fine to call more than once)."""
        if self.proc.poll() is None:
            try:
                self.monitor('quit')
                self.proc.wait(timeout=10)
            except Exception:
                self.proc.kill()
        try:
            self.sock.close()
        except Exception:
            pass


def read_disk_file(disk, path):
    """A file's bytes from a disk image (with QEMU stopped), or None if it isn't there."""
    result = subprocess.run(['mtype', '-i', disk, '::' + path], capture_output=True)
    return result.stdout if result.returncode == 0 else None


def write_disk_file(disk, path, data):
    """Put a file on a disk image (with QEMU stopped), replacing one that is there."""
    source = disk + '.put'
    with open(source, 'wb') as f:
        f.write(data if isinstance(data, bytes) else data.encode())
    subprocess.run(['mcopy', '-o', '-i', disk, source, '::' + path], check=True, capture_output=True)
    os.remove(source)


def delete_disk_file(disk, path):
    """Delete a file from a disk image (with QEMU stopped)."""
    subprocess.run(['mdel', '-i', disk, '::' + path], check=True, capture_output=True)


def make_disk(path, size_mb=32, fat32=False, files=None):
    """Make an empty FAT disk labelled AEROS (the label AerOS looks for), with mtools.
    files maps a path on the disk ('/bin/x.aer') to a file to copy there."""
    with open(path, 'wb') as f:
        f.truncate(size_mb * 1024 * 1024)
    sectors = size_mb * 2048
    fat = ['-F'] if fat32 else []
    subprocess.run(['mformat', '-i', path, '-T', str(sectors), '-h', '16', '-s', '63', '-v', 'AEROS']
                   + fat + ['::'], check=True, capture_output=True)
    for target, source in (files or {}).items():
        folder = os.path.dirname(target)
        if folder not in ('', '/'):
            subprocess.run(['mmd', '-D', 's', '-i', path, '::' + folder], capture_output=True)
        subprocess.run(['mcopy', '-o', '-i', path, source, '::' + target], check=True, capture_output=True)
    return path
