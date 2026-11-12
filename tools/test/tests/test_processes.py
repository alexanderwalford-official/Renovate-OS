"""The process stack: RUN, and the queue (QUE, GO) from the terminal and from AerLang."""

import time

from aeros import GUEST_PROMPT


def test_run(t):
    # RUN starts a process inside the terminal's: both show as RUNNING, and the new one's slot
    # is free again afterwards
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.cls()
    aeros.run('run print_stack')
    aeros.expect('Process Stack: 2 process(es)', 'RUNNING      80       0  terminal',
                 'print_stack')
    aeros.cls()
    aeros.run('pm')
    aeros.expect('Process Stack: 1 process(es)')


def test_queue(t):
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.run('mkf a.aer print("A ran")')
    aeros.run('mkf b.aer print("B ran with", args)')
    aeros.cls()
    # queued first, so it runs first
    aeros.run('que b x y')
    aeros.run('que a')
    aeros.run('que')
    aeros.expect('1. b.aer', '2. a.aer')
    aeros.cls()
    aeros.run('go')
    aeros.expect_order('B ran with ["x", "y"]', 'A ran', 'Ran 2 program(s), the queue is empty.')


def test_queue_stops_on_ctrl_c(t):
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.run('mkf a.aer print("A ran")')
    aeros.run('mkf loop.aer while true do sleep(10) end')
    aeros.cls()
    aeros.run('que a')
    aeros.run('que loop')
    aeros.run('que a')
    # the first one runs, then the loop never ends until Ctrl+C
    before = aeros.prompts()
    aeros.ask('go')
    aeros.wait_for('A ran')
    aeros.key('ctrl-c')
    aeros.wait_prompt(before)
    aeros.run('que')
    # the first ran, the loop was stopped, and the last one is still waiting
    aeros.expect('stopped (Ctrl+C)', '1. a.aer')


def test_queue_clear_and_queue_from_aerlang(t):
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.run('mkf a.aer print("A ran")')
    aeros.cls()
    aeros.run('que a')
    aeros.run('que a')
    aeros.run('que clear')
    aeros.expect('Took 2 program(s) out of the queue.')
    aeros.cls()
    aeros.run('queue("a.aer")')
    aeros.expect_line('1')
    aeros.run('go')
    aeros.expect('A ran', 'Ran 1 program(s), the queue is empty.')
