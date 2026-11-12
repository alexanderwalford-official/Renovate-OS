"""A real FAT disk: accounts and logging in, file versions, file encryption, and permissions."""

import time

from aeros import GUEST_PROMPT


def test_accounts(t):
    aeros = t.boot(disk=t.disk())
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
    aeros = t.boot(disk=t.disk())
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


def test_permissions(t):
    # a program asks before reading the user's files: N, then Y, then A (always)
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.run('mkd /data')
    aeros.run('mkf /data/n.txt hello')
    aeros.run('mkf t.aer print(rdf("/data/n.txt"))')
    aeros.cls()
    aeros.ask('t', 1.5)
    aeros.expect('[ ? ] t.aer wants to read files in /data/ (n.txt)')
    aeros.key('n')
    time.sleep(1)
    aeros.expect_line('nil')
    aeros.cls()
    aeros.ask('t', 1.5)
    aeros.key('y')
    time.sleep(1)
    aeros.expect_line('hello')
    aeros.ask('t', 1.5)
    aeros.key('a')
    time.sleep(1)
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
