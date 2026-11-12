#!/usr/bin/env python3
"""
run.py - AerOS's tests: boot AerOS in QEMU and check that it does what it should

    make test                       (from src/: builds first, then runs every test)
    python3 tools/test/run.py       every test
    python3 tools/test/run.py why   only the tests with "why" in their name
    python3 tools/test/run.py -j 1  one at a time (they run on every processor core at once unless you say)
    python3 tools/test/run.py -l    list the tests

It needs QEMU (qemu-system-i386), Python 3 and mtools (to make the disk images), on Linux or WSL.
See docs/TESTING.md for how the tests work and how to write one.

A test is a function called test_something in a file in tests/. It is given a Test, t, and
boots AerOS with t.boot() (see aeros.py for what the machine can do). Every machine runs in a
folder of its own, so tests can't get in each other's way, and is stopped afterwards whatever
happened. A test passes if it returns, and fails if a check raises Failure (or anything else
goes wrong). The folder of a test that fails is kept in tools/test/out/, with AerOS's serial log.
"""

import argparse
import concurrent.futures
import importlib.util
import os
import shutil
import sys
import tempfile
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

from aeros import AerOS, Failure, Skip, make_disk  # noqa: E402  (needs the path above)


class Test:
    """What a test is given: somewhere to work, and a way to boot AerOS."""

    def __init__(self, name, kernel, iso):
        self.name = name
        self.kernel = kernel
        self.iso = iso
        self.workdir = tempfile.mkdtemp(prefix='aeros-test-')
        self.files = os.path.join(HERE, 'files')   # files the tests copy onto disks
        self.machines = []

    def boot(self, graphics=False, disk=None, **options):
        """Start AerOS: the kernel in text mode, or the ISO (graphics=True) for graphics tests.
        disk is an image from t.disk(). Other options go to AerOS() (memory, network)."""
        machine = AerOS(self.workdir, kernel=None if graphics else self.kernel,
                        iso=self.iso if graphics else None, disk=disk, **options)
        self.machines.append(machine)
        return machine

    def disk(self, name='disk.img', **options):
        """A new, empty AEROS disk in this test's folder (see make_disk for the options)."""
        return make_disk(os.path.join(self.workdir, name), **options)

    def path(self, name):
        """A file in this test's own folder."""
        return os.path.join(self.workdir, name)

    def finish(self, keep):
        """Stop every machine, then tidy the folder away (or keep it to look at)."""
        for machine in self.machines:
            machine.quit()
        if keep:
            out = os.path.join(HERE, 'out', self.name)
            shutil.rmtree(out, ignore_errors=True)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            shutil.move(self.workdir, out)
            return out
        shutil.rmtree(self.workdir, ignore_errors=True)
        return None


def find_tests():
    """Every test_ function in tests/*.py, as (name, function), in file and line order."""
    found = []
    folder = os.path.join(HERE, 'tests')
    for file in sorted(os.listdir(folder)):
        if not (file.startswith('test_') and file.endswith('.py')):
            continue
        spec = importlib.util.spec_from_file_location(file[:-3], os.path.join(folder, file))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        functions = [(getattr(module, n).__code__.co_firstlineno, n) for n in dir(module)
                     if n.startswith('test_') and callable(getattr(module, n))]
        for _, name in sorted(functions):
            found.append((f'{file[5:-3]}.{name[5:]}', getattr(module, name)))
    return found


def run_one(name, function, kernel, iso):
    """Run one test, giving (name, result, seconds, message): result is PASS, FAIL or SKIP."""
    t = Test(name, kernel, iso)
    start = time.time()
    message = ''
    try:
        function(t)
        result = 'PASS'
    except Skip as e:
        result = 'SKIP'
        message = str(e)
    except Failure as e:
        result = 'FAIL'
        message = str(e)
    except Exception:
        result = 'FAIL'
        message = traceback.format_exc()
    kept = t.finish(keep=result == 'FAIL')
    if kept:
        message += f'\n(its files, with serial.log, are in {kept})'
    return name, result, time.time() - start, message


def missing_tools():
    """The programs the tests need that aren't installed."""
    needed = [os.environ.get('QEMU', 'qemu-system-i386'), 'mformat', 'mcopy']
    return [tool for tool in needed if shutil.which(tool) is None]


def main():
    parser = argparse.ArgumentParser(description='Run AerOS in QEMU and check what it does.')
    parser.add_argument('names', nargs='*', help='only run tests with one of these in their name')
    parser.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 1,
                        help='how many tests run at once (every processor core unless you say)')
    parser.add_argument('-l', '--list', action='store_true', help='list the tests and stop')
    parser.add_argument('--kernel', default=os.path.join(REPO, 'src', 'iso', 'boot', 'main.elf'))
    parser.add_argument('--iso', default=os.path.join(REPO, 'src', 'main.iso'))
    args = parser.parse_args()

    tests = find_tests()
    if args.names:
        tests = [(n, f) for n, f in tests if any(part.lower() in n.lower() for part in args.names)]
    if args.list:
        for name, _ in tests:
            print(name)
        return 0
    if not tests:
        print('No tests match.')
        return 1

    missing = missing_tools()
    if missing:
        print('These are needed to run the tests: ' + ', '.join(missing))
        print('On Ubuntu or WSL: sudo apt install qemu-system-x86 mtools')
        return 1
    for path in (args.kernel, args.iso):
        if not os.path.exists(path):
            print(f'{path} is missing: build AerOS first (make, in src/).')
            return 1

    print(f'Running {len(tests)} test(s), {args.jobs} at a time.\n')
    start = time.time()
    failed = []
    skipped = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        jobs = [pool.submit(run_one, name, function, args.kernel, args.iso) for name, function in tests]
        for job in concurrent.futures.as_completed(jobs):
            name, result, seconds, message = job.result()
            note = f'  ({message})' if result == 'SKIP' else ''
            print(f'{result}  {name}  ({seconds:.0f}s){note}', flush=True)
            if result == 'FAIL':
                failed.append((name, message))
            elif result == 'SKIP':
                skipped += 1

    for name, message in failed:
        print(f'\n===== {name} failed =====\n{message}')
    passed = len(tests) - len(failed) - skipped
    print(f'\n{passed} passed, {len(failed)} failed, {skipped} skipped, in {time.time() - start:.0f}s.')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
