"""The keyboard and the mouse, as AerLang programs see them."""

import time

from aeros import GUEST_PROMPT


def test_mouse(t):
    # One line at the prompt prints the mouse every 200 ms for 6 seconds, while QEMU moves it and
    # presses the left button, each step waiting until a line shows it arrived. (VGA text mode
    # has no pixels, so the pointer moves over a 640 x 480 area, starting in the middle)
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.cls()
    aeros.ask('for i = 1 to 30 do print(mouse(), mousemove(), mousedown(MOUSE_LEFT), sleep(200)) end')
    aeros.wait_for('[320, 240, 0] [0, 0] false', 10)    # the mouse is on, and hasn't moved
    aeros.mouse_move(40, 0)
    aeros.wait_for('[360, 240, 0]', 10)                 # 40 to the right
    aeros.mouse_move(0, 25)
    aeros.wait_for('[360, 265, 0]', 10)                 # 25 down (the screen counts down)
    aeros.mouse_buttons(1)
    aeros.wait_for('[360, 265, 1] [0, 0] true', 10)     # the left button held
    aeros.mouse_buttons(0)
    # mousemove() gave each movement once, in the line after it happened
    aeros.expect_printed('[360, 240, 0] [40, 0]', '[360, 265, 0] [0, 25]')
    aeros.wait_for(GUEST_PROMPT, 15)


def test_mouse_is_off_after_a_program(t):
    # the mouse is only for programs: once the line has finished, moving it changes nothing, and
    # the keyboard still works (mouse bytes never turn into key presses)
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.run('mouse()')
    aeros.mouse_move(100, 100)
    aeros.mouse_buttons(1)
    aeros.mouse_buttons(0)
    time.sleep(0.5)
    aeros.cls()
    aeros.run('print("keys still work")')
    aeros.run('mouse()')
    aeros.expect_line('keys still work', '[320, 240, 0]')


def test_keys_held(t):
    # keydown() sees a key while it is held, without waiting. The loop looks for about 5
    # seconds (1000 times, sleeping 5 ms each time keydown is false) while Q is held for one.
    # QEMU presses keys in order, so Q goes down straight after the Enter that starts the loop
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.run('let seen = false')
    before = aeros.prompts()
    aeros.type('for i = 1 to 1000 do if keydown("q") or sleep(5) then seen = true end end\n')
    aeros.key('q', 1000)
    aeros.wait_prompt(before, 30)
    # the held Q also typed some q's, which wait at the prompt: rub them out
    for _ in range(30):
        aeros.key('backspace')
    aeros.cls()
    aeros.run('seen')
    aeros.expect_line('true')
