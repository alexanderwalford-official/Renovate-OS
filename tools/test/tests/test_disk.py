"""A real FAT disk: accounts and logging in, file versions, file encryption, and permissions."""

import os
import re
import subprocess
import time

from aeros import GUEST_PROMPT, read_disk_file, write_disk_file


def test_accounts(t):
    disk = t.disk()
    aeros = t.boot(disk=disk)
    aeros.first_login('tester', 'secret1')

    # a second account
    aeros.ask('mku bob')
    aeros.answer('pw1')
    aeros.answer('pw1')
    aeros.wait_for('Made the account bob', 60)
    aeros.cls()
    aeros.run('usrs')
    aeros.expect('tester', 'bob')

    # log out, a wrong password, then the right one
    aeros.ask('out')
    aeros.wait_for('Authentication required', 30)
    aeros.answer('bob')
    aeros.answer('nope')
    aeros.wait_for('Wrong user name', 30)
    aeros.login('bob', 'pw1')

    # change the password, then delete the other account (DELU asks for your own password)
    aeros.ask('pw')
    aeros.answer('pw1')
    aeros.answer('pw2')
    aeros.answer('pw2')
    aeros.wait_for('Password changed', 60)
    aeros.ask('delu tester')
    aeros.answer('pw2')
    aeros.wait_for('Deleted tester', 60)
    aeros.quit()
    t.check_disk(disk)


def test_accounts_survive_a_restart(t):
    disk = t.disk()
    aeros = t.boot(disk=disk)
    aeros.first_login('tester', 'secret1')
    aeros.run('mkf kept.txt still here')
    aeros.quit()

    # the same disk again: it asks to log in, and the file is still there
    aeros = t.boot(disk=disk)
    aeros.login('tester', 'secret1')
    aeros.cls()
    aeros.run('rd kept.txt')
    aeros.expect_line('still here')


def test_versions(t):
    disk = t.disk()
    aeros = t.boot(disk=disk)
    aeros.first_login()

    aeros.run('mkf notes.txt one')
    aeros.run('wr notes.txt two')
    # changes closer together than this are kept as one version (VERSIONS_GAP_MS in
    # src/sys/versions.h), so wait it out before the next one
    time.sleep(11)
    aeros.run('wr notes.txt three')
    aeros.cls()
    aeros.run('vers notes.txt')
    aeros.expect('notes.txt@1', 'notes.txt@2')
    aeros.cls()
    aeros.run('rd notes.txt@2')
    aeros.expect_line('one')

    # UNDO brings an old version back
    aeros.cls()
    aeros.run('undo notes.txt 2')
    aeros.expect('notes.txt is back as it was')
    aeros.cls()
    aeros.run('rd notes.txt')
    aeros.expect_line('one')

    # and a deleted file can be brought back too
    aeros.cls()
    aeros.run('del notes.txt')
    aeros.run('vers notes.txt')
    aeros.expect('notes.txt was deleted: UNDO notes.txt brings it back.')
    aeros.run('undo notes.txt')
    aeros.cls()
    aeros.run('rd notes.txt')
    aeros.expect_line('one')

    # outside a home folder there are no versions
    aeros.run('mkf /out.txt x')
    aeros.run('wr /out.txt y')
    aeros.cls()
    aeros.run('vers /out.txt')
    aeros.expect('/out.txt has no old versions.')
    aeros.quit()
    t.check_disk(disk)


def test_encryption(t):
    disk = t.disk()
    aeros = t.boot(disk=disk)
    aeros.first_login()
    aeros.run('mkf secret.txt Top secret plan')
    aeros.ask('enc secret.txt')
    aeros.answer('a1')
    aeros.answer('a1')
    aeros.wait_for('Encrypted secret.txt', 60)

    # the wrong password can't read it, the right one can
    aeros.cls()
    aeros.ask('rd secret.txt')
    aeros.answer('nope')
    aeros.wait_for("Can't read", 60)
    aeros.ask('rd secret.txt')
    aeros.answer('a1')
    aeros.wait_for('Top secret plan', 60)

    # and none of the words are left readable on the disk
    aeros.quit()
    if b'Top secret plan' in open(disk, 'rb').read():
        aeros.fail('the plain text is still on the disk after ENC')


def test_delete(t):
    # a 20 MB file, a folder with files in folders inside it, and an empty folder
    with open(t.path('big.bin'), 'wb') as f:
        f.write(os.urandom(20 * 1024 * 1024))
    small = t.path('small.txt')
    with open(small, 'w') as f:
        f.write('a small file\n')
    files = {'/big.bin': t.path('big.bin')}
    for i in range(8):
        files[f'/stuff/f{i}.txt'] = small
        files[f'/stuff/sub/f{i}.txt'] = small
    disk = t.disk(files=files)
    subprocess.run(['mmd', '-i', disk, '::/empty'], check=True, capture_output=True)

    aeros = t.boot(disk=disk)
    aeros.first_login()
    aeros.cls()
    aeros.run('del /big.bin')
    aeros.expect('Deleted big.bin.')

    # a folder with something in it asks first: no, then yes
    aeros.ask('del /stuff')
    aeros.wait_for('/stuff/ has 16 file(s) in it. Delete it and everything in it? (y/n)')
    aeros.key('n')
    aeros.wait_for('Nothing was deleted.')
    aeros.cls()
    aeros.ask('del /stuff')
    aeros.wait_for('(y/n)')
    before = aeros.prompts()
    aeros.key('y')
    aeros.wait_prompt(before)
    aeros.expect('Deleted 16 file(s).', 'Deleted /stuff/.')
    aeros.run('del /empty')
    aeros.expect('Deleted /empty/.')

    # all of it is gone from the disk, and the disk is still in good order
    aeros.quit()
    for gone in ['/big.bin', '/stuff/f0.txt']:
        if read_disk_file(disk, gone) is not None:
            aeros.fail(f'{gone} is still on the disk')
    t.check_disk(disk)


def test_three_wrong_passwords_wipe_the_home_folder(t):
    # after the third wrong password in a row, the account's own files are securely deleted
    disk = t.disk()
    aeros = t.boot(disk=disk)
    aeros.first_login('tester', 'secret1')
    for i in range(5):
        aeros.run(f'mkf note{i}.txt private note {i}')
    aeros.ask('out')
    for attempt in range(3):
        aeros.wait_for('Authentication required', 30)
        aeros.answer('tester')
        aeros.answer(f'wrong{attempt}')
        if attempt < 2:
            aeros.wait_for(f'{attempt + 1} of 3', 60)
    aeros.wait_for('have been securely deleted', 60)

    # the right password still works, and says what happened
    aeros.login('tester', 'secret1')
    aeros.expect('wrong password(s) were typed for tester', 'wiped 1 time(s)')
    aeros.quit()
    if b'private note' in open(disk, 'rb').read():
        aeros.fail('the notes can still be read on the disk')
    t.check_disk(disk)


def test_permissions(t):
    # a program asks before reading the user's files: N, then Y, then A (always)
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.run('mkd /data')
    aeros.run('mkf /data/n.txt hello')
    aeros.run('mkf t.aer print(rdf("/data/n.txt"))')
    for key, shows in [('n', 'nil'), ('y', 'hello'), ('a', 'hello')]:
        aeros.cls()
        aeros.ask('t')
        aeros.wait_for('[ ? ] t.aer wants to read files in /data/ (n.txt)')
        before = aeros.prompts()
        aeros.key(key)
        aeros.wait_prompt(before)
        aeros.expect_line(shows)
    # always: no question the next time
    aeros.cls()
    aeros.run('t')
    aeros.expect_not('[ ? ]')
    aeros.expect_line('hello')
    aeros.cls()
    aeros.run('perm')
    aeros.expect('may read', 'files in /data/')
    aeros.run('perm del 1')
    aeros.expect('Forgot 1 answer(s)')


def fill_disk(t, disk):
    """Fill every free byte of a disk image with one big file, between boots."""
    listing = subprocess.run(['mdir', '-i', disk, '::'], capture_output=True, text=True).stdout
    free = int(re.search(r'([\d ]+) bytes free', listing).group(1).replace(' ', ''))
    filler = t.path('filler.bin')
    with open(filler, 'wb') as f:
        f.truncate(free)
    subprocess.run(['mcopy', '-i', disk, filler, '::/filler.bin'], check=True, capture_output=True)


def test_a_full_disk_is_reported(t):
    # A write that fails says so, rather than nothing: the boot log for a built-in file it
    # couldn't update, and the terminal for a version of a file it couldn't keep
    disk = t.disk()
    aeros = t.boot(disk=disk)
    aeros.first_login()
    aeros.run('mkf notes.txt hello')
    aeros.quit()

    # an older manual page for the boot to update, then no room at all
    write_disk_file(disk, '/docs/why.md', 'an older page\n')
    fill_disk(t, disk)

    aeros = t.boot(disk=disk)
    aeros.wait_for('Authentication required', 60)
    aeros.expect_printed("built-in file(s) couldn't be written to the disk")
    aeros.login()
    aeros.cls()
    aeros.run('wr notes.txt changed')
    aeros.expect("Couldn't keep a version of notes.txt for UNDO: no space left")
    aeros.run('why')
    aeros.expect('What you can do')
