"""The network, against tools/aernet.py on this computer (see docs/NETWORK.md).

QEMU's user networking puts this computer at 10.0.2.2 as AerOS sees it, so AerOS's messages to
10.0.2.2:5000 arrive at aernet.py listening on port 5000 here. The other way, QEMU passes a
port here on to AerOS's port 5000. Needs Python's "cryptography" package, as aernet.py does."""

import os
import subprocess
import sys
import time

from aeros import GUEST_PROMPT, Skip, free_port

TOOL = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                    'aernet.py')
PASSWORD = 'sesame123'


def aernet(*args, password=PASSWORD):
    """Run aernet.py with these arguments and the password, giving what it printed."""
    env = dict(os.environ, AERNET_PASSWORD=password)
    return subprocess.run([sys.executable, TOOL] + list(args), env=env,
                          capture_output=True, text=True).stdout.strip()


def test_sealed_messages(t):
    try:
        import cryptography  # noqa: F401  (only checking it is there)
    except ImportError:
        raise Skip('needs Python\'s cryptography package: pip install cryptography')

    # aernet.py here answers every message with "echo: <message>"
    log = open(t.path('aernet.log'), 'w')
    listener = subprocess.Popen([sys.executable, '-u', TOOL, 'listen', '5000', '--echo'],
                                env=dict(os.environ, AERNET_PASSWORD=PASSWORD), stdout=log, stderr=log)
    forward = free_port()
    try:
        aeros = t.boot(network=f'user,model=e1000,hostfwd=udp::{forward}-:5000')
        aeros.wait_for(GUEST_PROMPT, 60)

        # nothing gets out until the firewall allows it
        aeros.cls()
        aeros.run('msg 10.0.2.2 5000 hi')
        aeros.expect("Can't send to 10.0.2.2: the firewall blocks it")

        aeros.ask('fw allow 10.0.2.2 5000')
        aeros.answer(PASSWORD)
        aeros.answer(PASSWORD)
        aeros.wait_for('Allowed 10.0.2.2 port 5000', 60)

        # a message out and its echo back, then one from here to AerOS
        aeros.cls()
        aeros.run('msg 10.0.2.2 5000 hello from AerOS')
        before = aeros.prompts()
        aeros.ask('lsn 5000')
        aeros.wait_for('[10.0.2.2] echo: hello from AerOS', 30)
        aernet('send', '127.0.0.1', str(forward), 'hi from the host')
        aeros.wait_for('[10.0.2.2] hi from the host', 30)
        # the wrong password is kept out, so nothing is shown for it
        aernet('send', '127.0.0.1', str(forward), 'sneaky', password='wrongpass')
        time.sleep(1.5)
        aeros.key('esc')
        aeros.wait_prompt(before)
        aeros.expect_not('sneaky')

        # AerLang's nsend and nrecv
        aeros.cls()
        aeros.run('nsend("10.0.2.2", 5000, "from AerLang")')
        aeros.run('nrecv(5000, 3000)')
        aeros.expect_line('echo: from AerLang')

        # with the rule gone, it is blocked again
        aeros.run('fw del 1')
        aeros.cls()
        aeros.run('msg 10.0.2.2 5000 hi')
        aeros.expect("Can't send to 10.0.2.2: the firewall blocks it")
    finally:
        listener.terminate()
        log.close()
