#!/usr/bin/env python3
"""
check.py - check AerOS's C code keeps the conventions WHY and SRC rely on

    make check            (from src/)
    python3 tools/check.py

WHY explains a function from the comment above it, and SRC shows its code, by reading AerOS's
own source (src/sys/source.c). They find things the simple way a person would, so the code has
to be written the way they expect:

  1. Every function has a comment directly above its definition: "//" lines, or a /* */ block,
     with no blank line in between. (Without one, WHY has nothing to say about it.)
  2. A definition is one line, starting at column 0 and ending with its "{":
         static int fs_list (const char* d_path, fs_list_fn fn, void* ctx) {
     (SRC finds a function by its name on a line at column 0, and a definition split over two
     lines shows up cut in half.)
  3. A function's closing "}" is at column 0, and nothing else inside it is. (SRC shows a
     function down to the next line that starts with "}".)

Each problem is printed as file:line, so editors can jump to it. It exits with 1 if there are any.
Other people's code (the MP3 and MPEG decoders) and the two data files are left out, as they are
from /src/.
"""

import os
import re
import sys

SRC = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'src')
FOLDERS = ['.', 'lib', 'sys', 'sys/accounts', 'sys/terminal', 'lang', 'db', 'drivers/audio',
           'drivers/input', 'drivers/net', 'drivers/pci', 'drivers/serial', 'drivers/storage',
           'drivers/video']
LEFT_OUT = {'sys/splash_logo.c', 'drivers/video/font8x16.c'}

# a line that could start a function: column 0, a name, and a "(" later on
STARTS_LIKE_FUNCTION = re.compile(r'^[A-Za-z_][\w \t\*,]*?\b([A-Za-z_]\w*)\s*\(')
# a string or character in quotes (with \" and \' inside them allowed)
STRINGS = re.compile(r'"(\\.|[^"\\])*"|\'(\\.|[^\'\\])*\'')
# words that start a column 0 line with "(" in it but never a function definition
NOT_FUNCTIONS = ('typedef', '__attribute__', '__asm__', 'asm', 'extern', 'return', 'if', 'for',
                 'while', 'switch', 'else', 'do', 'case', 'goto', 'sizeof')


def c_files():
    """Every .c file of AerOS's own, as (path relative to src/, full path)."""
    for folder in FOLDERS:
        full = os.path.join(SRC, folder)
        for name in sorted(os.listdir(full)):
            relative = os.path.normpath(os.path.join(folder, name)).replace(os.sep, '/')
            if name.endswith('.c') and relative not in LEFT_OUT:
                yield relative, os.path.join(full, name)


def is_comment_end(line):
    """Is this line a "//" comment, or the last line of a /* */ block?"""
    stripped = line.strip()
    return stripped.startswith('//') or stripped.endswith('*/')


def code_lines(lines):
    """Which lines are code rather than inside a /* */ block, as a list of True / False.
    (A block comment's text can start at column 0 and look like code)"""
    out = []
    in_block = False
    for line in lines:
        out.append(not in_block)
        # the state after this line: a "/*" with no "*/" after it opens a block. A "/*" inside
        # quotes is just text, so strings and characters are taken out first
        text = STRINGS.sub('""', line) if not in_block else line
        while True:
            if in_block:
                end = text.find('*/')
                if end < 0:
                    break
                in_block = False
                text = text[end + 2:]
            else:
                start = text.find('/*')
                if start < 0 or '//' in text[:start]:
                    break
                in_block = True
                text = text[start + 2:]
    return out


def check_file(relative, path):
    """The problems in one file, as (line number, message)."""
    lines = open(path, encoding='utf-8', errors='replace').read().replace('\r', '').split('\n')
    is_code = code_lines(lines)
    problems = []
    for i, line in enumerate(lines):
        if not is_code[i]:
            continue
        match = STARTS_LIKE_FUNCTION.match(line)
        if not match or line.split('(')[0].split()[0] in NOT_FUNCTIONS:
            continue
        name = match.group(1)
        if name in NOT_FUNCTIONS:
            continue

        # a definition (its "{" comes before any ";") or only a declaration?
        rest = '\n'.join(lines[i:i + 12])
        brace, semicolon = rest.find('{'), rest.find(';')
        if brace < 0 or (0 <= semicolon < brace):
            continue
        # an initialiser like "static const x names[] = {" has "=" before its "{"
        if '=' in rest[:brace].split('(')[0]:
            continue

        if not line.rstrip().endswith('{'):
            problems.append((i + 1, f'{name}: put the definition on one line, ending with "{{"'))
        if i == 0 or not is_comment_end(lines[i - 1]):
            problems.append((i + 1, f'{name}: no comment directly above it (WHY reads that comment)'))

        # the body: the first "}" at column 0 must be the function's own
        depth = 0
        for j in range(i, len(lines)):
            text = re.sub(r'"(\\.|[^"\\])*"|\'(\\.|[^\'\\])*\'|//.*', '', lines[j]) if is_code[j] else ''
            depth += text.count('{') - text.count('}')
            if j > i and lines[j].startswith('}'):
                if depth != 0:
                    problems.append((j + 1, f'{name}: a "}}" at column 0 inside the function '
                                            '(SRC stops there): indent it'))
                break
            if j > i and depth == 0 and '}' in text:
                problems.append((j + 1, f'{name}: its closing "}}" should be at column 0'))
                break
    return problems


def main():
    count = 0
    for relative, path in c_files():
        for line, message in check_file(relative, path):
            print(f'src/{relative}:{line}: {message}')
            count += 1
    if count:
        print(f'\n{count} problem(s). See the top of tools/check.py for why each one matters.')
        return 1
    print('The code keeps the conventions WHY and SRC rely on.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
