"""The built-in apps: the manual reader, the animation demo, and how built-in apps are installed
onto a disk and kept up to date (see the top of src/sys/apps.c)."""

import hashlib
import re
import time

from aeros import GUEST_PROMPT, Failure, delete_disk_file, read_disk_file, write_disk_file


def quit_app(aeros, key='q'):
    """Press the key that leaves an app, and wait for the terminal to be back."""
    before = aeros.prompts()
    aeros.key(key)
    aeros.wait_prompt(before, 20)


def test_doc_reader(t):
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.ask('doc')
    aeros.wait_for('The AerOS manual', 60)
    quit_app(aeros)

    aeros.ask('doc security')
    aeros.wait_for('Accounts and encryption', 60)
    quit_app(aeros)

    # the same reader shows any text file
    aeros.ask('read /bin/hello.aer')
    aeros.wait_for('hello.aer - a first', 60)
    quit_app(aeros)


def test_animations(t):
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.ask('anim')
    aeros.wait_for('Animations', 30)
    time.sleep(0.5)
    first = aeros.screen()
    time.sleep(1.3)
    if aeros.screen() == first:
        aeros.fail('nothing moved in 1.3 seconds')
    quit_app(aeros, 'spc')


def boot_and_stop(t, disk):
    """Boot with the disk until AerOS asks for an account (the apps are installed before that),
    then stop. Gives what AerOS printed."""
    aeros = t.boot(disk=disk)
    aeros.wait_for('Create the first account', 60)
    printed = aeros.serial()
    aeros.quit()
    return printed


def test_built_in_apps_are_installed(t):
    disk = t.disk()
    built_in_hello = open(t.repo('src', 'apps', 'hello.aer'), 'rb').read()
    built_in_viewer = open(t.repo('src', 'apps', 'viewer.aer'), 'rb').read()

    # a new disk gets every built-in file, and a record of what it was given
    printed = boot_and_stop(t, disk)
    added = re.search(r'(\d+) built-in file\(s\) added or updated', printed)
    if not added or int(added.group(1)) < 10:
        raise Failure('the built-in files were not added:\n' + printed[-800:])
    if read_disk_file(disk, '/bin/hello.aer') != built_in_hello:
        raise Failure('/bin/hello.aer is not the built-in copy')
    record = read_disk_file(disk, '/sys/apps.lst').decode()
    if '/bin/viewer.aer ' not in record:
        raise Failure('/sys/apps.lst has no line for viewer.aer:\n' + record)

    # booting again has nothing to do
    if 'added or updated' in boot_and_stop(t, disk):
        raise Failure('the second boot added files again')

    # an app changed by hand is kept, and so is the change
    write_disk_file(disk, '/bin/hello.aer', 'print("my own hello")\n')
    boot_and_stop(t, disk)
    boot_and_stop(t, disk)
    if read_disk_file(disk, '/bin/hello.aer') != b'print("my own hello")\n':
        raise Failure('a changed hello.aer was replaced')

    # an older copy that nobody changed (its hash is the one in the record) is brought up to date
    older = b'# an older viewer\n'
    record = read_disk_file(disk, '/sys/apps.lst').decode()
    record = re.sub(r'^/bin/viewer\.aer \S+', '/bin/viewer.aer ' + hashlib.sha256(older).hexdigest(),
                    record, flags=re.M)
    write_disk_file(disk, '/sys/apps.lst', record)
    write_disk_file(disk, '/bin/viewer.aer', older)
    boot_and_stop(t, disk)
    if read_disk_file(disk, '/bin/viewer.aer') != built_in_viewer:
        raise Failure('an untouched older viewer.aer was not updated')

    # a deleted app comes back
    delete_disk_file(disk, '/bin/viewer.aer')
    boot_and_stop(t, disk)
    if read_disk_file(disk, '/bin/viewer.aer') != built_in_viewer:
        raise Failure('a deleted viewer.aer did not come back')
    t.check_disk(disk)
