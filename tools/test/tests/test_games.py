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


def has_colour(shot, colour):
    """Whether a screenshot has a pixel of this colour (0xRRGGBB) anywhere."""
    width, height, pixels = shot
    want = colour.to_bytes(3, 'big')
    found = pixels.find(want)
    while found != -1:
        if found % 3 == 0:
            return True
        found = pixels.find(want, found + 1)
    return False


def wait_for_colour(aeros, colour, name, timeout=60):
    """Take screenshots until one has this colour in it."""
    start = time.time()
    while not has_colour(aeros.screenshot(name), colour):
        if time.time() - start > timeout:
            aeros.fail(f'{name}: waited {timeout}s for the colour {colour:06X}')
        time.sleep(0.5)


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
    # Snake asks first if it may keep its high scores in /bin/games/
    aeros.ask('snake')
    aeros.wait_for('[ ? ]', 30)
    aeros.key('y')
    time.sleep(4)
    aeros.key('spc')
    time.sleep(0.5)
    first = aeros.screenshot('playing')
    time.sleep(0.5)
    # the snake moves by itself, so two pictures apart differ. (Quickly: it reaches the
    # wall in about 3 seconds, and Escape then needs pressing again on the game-over screen)
    if aeros.screenshot('moved')[2] == first[2]:
        aeros.fail('the snake did not move')
    aeros.key('esc')
    aeros.wait_for('Thanks for playing Snake!', 20)
    aeros.expect_not('Error')


GOLD = 0xFFD166   # Snake's colour for "NEW HIGH SCORE" and the high scores' heading


def test_snake_high_scores(t):
    # A copy of Snake whose score starts at 7, so the snake running into the wall by itself
    # makes a high score, and asks for a name
    game = open(t.repo('src', 'disk', 'bin', 'games', 'snake.aer')).read()
    if game.count('    score = 0\n') != 1:
        raise AssertionError("snake.aer's score = 0 line has changed: update this test")
    with open(t.path('snake7.aer'), 'w') as f:
        f.write(game.replace('    score = 0\n', '    score = 7\n'))
    # (in /bin/: /bin/games/ isn't there until AerOS has added its built-in files)
    disk = t.disk(files={'/bin/snake7.aer': t.path('snake7.aer')})

    aeros = t.boot(graphics=True, disk=disk)
    aeros.first_login()
    before = aeros.prompts()
    aeros.ask('snake7')
    aeros.wait_for('[ ? ]', 30)
    aeros.key('a')
    time.sleep(4)
    # no scores yet, so the title screen has no high scores on it
    if has_colour(aeros.screenshot('title'), GOLD):
        aeros.fail('the title screen shows high scores before there are any')

    # play: the snake goes right until it hits the wall, and then it's a high score
    aeros.key('spc')
    wait_for_colour(aeros, GOLD, 'new_high_score')
    aeros.type('Ann\n')
    time.sleep(2)
    aeros.screenshot('game_over')
    aeros.key('esc')
    aeros.wait_prompt(before, 20, what='snake7')
    aeros.expect_not('Error')

    # the score is in the database
    aeros.cls()
    aeros.run('sql /bin/games/scores.db SELECT game, name, score FROM scores')
    aeros.expect_line('game | name | score', 'snake | Ann | 7', '(1 row)')

    # and on the title screen next time, without asking again (the serial log keeps
    # everything, so count the questions)
    asked = aeros.serial().count('[ ? ]')
    aeros.ask('snake7')
    time.sleep(4)
    if not has_colour(aeros.screenshot('title_with_scores'), GOLD):
        aeros.fail('the title screen does not show the high scores')
    if aeros.serial().count('[ ? ]') != asked:
        aeros.fail('Snake asked again, after being told "always"')
