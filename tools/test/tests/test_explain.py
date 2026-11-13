"""AerOS explaining itself: WHY, SRC and the read-only /src/ folder."""

from aeros import GUEST_PROMPT


def test_why_a_command(t):
    # WHY reads the comment above a command's function in AerOS's own source
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.cls()
    aeros.run('why del')
    aeros.wait_for('shows the code', 60)
    aeros.expect('The command runs cli_delete in sys/terminal/cmd_files.c',
                 'SRC cli_delete shows the code')


def test_why_a_function(t):
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.cls()
    aeros.run('why sqrt')
    aeros.wait_for('shows the code', 60)
    aeros.expect('sqrt() is a built-in AerLang function', 'bi_sqrt in lang/bi_text.c')


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
    aeros.expect_printed('sys/terminal/cmd_files.c, lines', 'void cli_delete')


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


def test_show_a_change(t):
    # SHOW runs a command, then lists what AerOS did inside, step by step: here the version kept
    # first, the new data written, and the old data freed only after the entry points at the new
    aeros = t.boot(disk=t.disk())
    aeros.first_login()
    aeros.run('mkf notes.txt hello')
    aeros.cls()
    aeros.run('show wr notes.txt again')
    aeros.expect('What AerOS did:', 'WHY <name> explains a step, SRC <name> shows its code.')
    aeros.expect_order('versions_keep', 'fat_write_chain', 'fat_replace_file', 'fat_free_chain')
    aeros.expect('so UNDO can bring it back')


def test_show_a_refusal(t):
    # it shows why something was refused, too, naming the function that decided
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.cls()
    aeros.run('show msg 10.0.2.2 5000 hi')
    aeros.expect("Can't send to 10.0.2.2: the firewall blocks it",
                 'net_send', 'not sending to 10.0.2.2 port 5000')
    aeros.cls()
    aeros.run('show')
    aeros.expect('Usage: SHOW <COMMAND>')
