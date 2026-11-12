"""The cryptography: AerOS's own self-test at boot, and the decryptor published in
docs/SECURITY.md, run on a file AerOS encrypted, to show the format is what the docs say."""

import re
import subprocess
import sys

from aeros import GUEST_PROMPT, Skip, read_disk_file


def test_self_test_at_boot(t):
    # the boot log checks SHA-256, HMAC, PBKDF2 and AES-256 against known answers
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.expect_printed('[ OK ] Crypto self-test')


def test_documented_decryptor(t):
    try:
        import cryptography  # noqa: F401  (the published script needs it)
    except ImportError:
        raise Skip("needs Python's cryptography package: pip install cryptography")

    # the script exactly as docs/SECURITY.md prints it
    doc = open(t.repo('docs', 'SECURITY.md'), encoding='utf-8').read()
    section = doc.split('### Checking the format yourself', 1)[1]
    script = t.path('decrypt.py')
    with open(script, 'w', encoding='utf-8') as f:
        f.write(re.search(r'```python\n(.*?)```', section, re.S).group(1))

    disk = t.disk()
    aeros = t.boot(disk=disk)
    aeros.first_login()
    aeros.run('mkf secret.txt Top secret plan')
    aeros.ask('enc secret.txt')
    aeros.answer('a1')
    aeros.answer('a1')
    aeros.wait_for('Encrypted secret.txt', 60)
    aeros.quit()

    sealed = t.path('secret.enc')
    with open(sealed, 'wb') as f:
        f.write(read_disk_file(disk, '/home/tester/secret.txt'))
    right = subprocess.run([sys.executable, script, sealed, 'a1'], capture_output=True, text=True)
    if right.stdout.strip() != 'Top secret plan':
        aeros.fail(f'the documented decryptor gave {right.stdout!r} {right.stderr!r}')
    wrong = subprocess.run([sys.executable, script, sealed, 'nope'], capture_output=True, text=True)
    if 'wrong password' not in wrong.stderr:
        aeros.fail(f'the wrong password was not caught: {wrong.stdout!r} {wrong.stderr!r}')
