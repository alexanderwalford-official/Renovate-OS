"""AerOS explaining itself: WHY, SRC and the read-only /src/ folder."""

from aeros import GUEST_PROMPT


def test_why_a_command(t):
    # WHY reads the comment above a command's function in AerOS's own source
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.cls()
    aeros.run('why del')
    aeros.wait_for('shows the code', 60)
    aeros.expect('The command runs cli_delete in sys/terminal/t_int.c',
                 'SRC cli_delete shows the code')


def test_why_a_function(t):
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.cls()
    aeros.run('why sqrt')
    aeros.wait_for('shows the code', 60)
    aeros.expect('sqrt() is a built-in AerLang function', 'bi_sqrt in lang/aer_builtins.c')


def test_why_after_a_problem(t):
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.run('rd notez.txt')
    aeros.expect('File not found: notez.txt', '(WHY explains)')
    aeros.cls()
    aeros.run('why')
    aeros.wait_for('What you can do', 60)


def test_src(t):
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.cls()
    aeros.run('src del')
    # the code is longer than the screen, so its start is checked in the serial log
    aeros.expect('Page Up scrolls back to the start')
    aeros.expect_printed('sys/terminal/t_int.c, lines', 'static void cli_delete')


def test_src_folder_is_read_only(t):
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.run('la')
    aeros.expect("src/", "AerOS's own source code (read only)")
    aeros.cls()
    aeros.run('mkf /src/x.txt hi')
    aeros.run('del /src/sys')
    aeros.expect('Could not create /src/x.txt: it can be read but not changed',
                 "Can't delete /src/sys/: it can be read but not changed")
