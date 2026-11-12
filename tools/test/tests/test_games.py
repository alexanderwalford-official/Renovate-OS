"""The games in /bin/games/, booted from the ISO so GRUB sets up a graphics screen.

In graphics mode the text screen can't be read, so these check the serial log (where the
terminal's text also goes) and the pixels of screenshots."""

import time

from aeros import GUEST_PROMPT


def colours(shot):
    """How many different colours a screenshot has (a sample of its pixels, for speed)."""
    width, height, pixels = shot
    step = 3 * 97   # every 97th pixel
    return len({pixels[i:i + 3] for i in range(0, len(pixels) - 2, step)})


def test_maze3d(t):
    aeros = t.boot(graphics=True)
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.run('maze3d', 5)
    if colours(aeros.screenshot('title')) < 3:
        aeros.fail('the title screen looks empty')

    # start, then look round with the mouse: the picture must change
    aeros.key('spc')
    time.sleep(3)
    before = aeros.screenshot('start')
    if colours(before) < 6:
        aeros.fail('the maze looks empty')
    for _ in range(10):
        aeros.mouse_move(40, 0)
        time.sleep(0.05)
    time.sleep(1)
    after = aeros.screenshot('turned')
    if before[2] == after[2]:
        aeros.fail('moving the mouse did not turn the view')

    # walking forwards changes it too
    aeros.key('up', 800)
    time.sleep(1.5)
    if aeros.screenshot('walked')[2] == after[2]:
        aeros.fail('holding Up did not move')

    aeros.key('esc')
    aeros.wait_for('Thanks for playing Maze 3D!', 20)
    aeros.expect_not('Error')


def test_move(t):
    # move.aer: a square that moves while an arrow key is held, drawn with a back buffer
    aeros = t.boot(graphics=True, sound=True)
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.ask('move', 3)
    first = aeros.screenshot('start')
    aeros.key('right', 1000)
    time.sleep(1.5)
    if aeros.screenshot('moved')[2] == first[2]:
        aeros.fail('holding Right did not move the square')
    before = aeros.prompts()
    aeros.key('esc')
    aeros.wait_prompt(before, 20)
    aeros.expect_not('Error')


def test_snake(t):
    aeros = t.boot(graphics=True)
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.run('snake', 4)
    aeros.key('spc')
    time.sleep(1)
    first = aeros.screenshot('playing')
    time.sleep(1)
    # the snake moves by itself, so two pictures a second apart differ
    if aeros.screenshot('moved')[2] == first[2]:
        aeros.fail('the snake did not move')
    aeros.key('esc')
    aeros.wait_for('Thanks for playing Snake!', 20)
    aeros.expect_not('Error')
