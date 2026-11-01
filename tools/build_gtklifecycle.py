#!/usr/bin/env python3
"""Build the GTK window-lifecycle guard and link it into Buoy Linux GUI programs.

The guard is a C object (libraries/gtklifecycle/native/gtk_lifecycle.c) that
corrects three defects of the Buoy 0.72 Linux UI bridge at the GTK call boundary.
It is linked into the executable with the compiler's --extra-obj flag, so the
fix needs no environment variables or extra runtime files.

    python3 tools/build_gtklifecycle.py                      # build, print the object path
    python3 tools/build_gtklifecycle.py app.bui -o app [...]  # build app with the guard

Non-Linux targets compile unchanged: the defects and the guard are Linux-only.
"""
import argparse
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LIBRARY = ROOT / 'libraries/gtklifecycle'
SOURCE = LIBRARY / 'native/gtk_lifecycle.c'
TRIPLES = {'x86_64-linux': 'x86_64-unknown-linux-gnu', 'arm64-linux': 'aarch64-unknown-linux-gnu'}
CLANG_TARGETS = {'x86_64-linux': 'x86_64-linux-gnu', 'arm64-linux': 'aarch64-linux-gnu'}


def host_target():
    machine = {'aarch64': 'arm64', 'amd64': 'x86_64'}.get(platform.machine().lower(), platform.machine().lower())
    return f'{machine}-linux' if sys.platform.startswith('linux') else None


def object_path(target):
    return LIBRARY / 'build' / TRIPLES[target] / 'gtk_lifecycle.o'


def build_object(target=None, cc=None, sysroot=None):
    """Compile the guard for a Linux target and return the object path."""
    target = target or host_target()
    if target not in TRIPLES:
        raise RuntimeError(f'gtklifecycle only applies to Linux targets, not {target}')
    output = object_path(target)
    output.parent.mkdir(parents=True, exist_ok=True)
    compiler = shlex.split(cc or os.environ.get('CC', 'cc'))
    # Buoy links external objects against its libc stubs, which do not export
    # glibc's fortified __fprintf_chk alias (Ubuntu enables it by default).
    flags = ['-std=c11', '-O2', '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=0', '-Wall', '-Wextra', '-Werror', '-fPIC', '-c']
    if target != host_target():
        # Cross builds need a target sysroot with glibc headers.
        if not sysroot:
            raise RuntimeError(f'cross-building gtklifecycle for {target} needs --sysroot <glibc sysroot>')
        compiler = shlex.split(cc or 'clang')
        flags += [f'--target={CLANG_TARGETS[target]}', f'--sysroot={sysroot}']
    # Studio and preview builds share this cache. Keep the previous complete
    # object available until compilation succeeds, then publish atomically on
    # the same filesystem. A unique directory also isolates concurrent builds.
    with tempfile.TemporaryDirectory(prefix='.gtk-lifecycle-', dir=output.parent) as directory:
        staged = Path(directory) / output.name
        subprocess.run(compiler + flags + [str(SOURCE), '-o', str(staged)], check=True)
        staged.replace(output)
    return output


def linux_target(arguments):
    """The Linux target a buoy command line builds for, or None for other targets."""
    target = host_target()
    for index, argument in enumerate(arguments):
        if argument == '--target' and index + 1 < len(arguments):
            target = arguments[index + 1]
        elif argument.startswith('--target='):
            target = argument.split('=', 1)[1]
    return target if target in TRIPLES else None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cc', help='C compiler (default: $CC or cc; clang for cross builds)')
    parser.add_argument('--sysroot', help='glibc sysroot for a cross build')
    parser.add_argument('--object-target', help='Linux target for the object alone (default: host)')
    options, buoy_arguments = parser.parse_known_args()
    if not buoy_arguments:
        print(build_object(options.object_target, options.cc, options.sysroot))
        return
    command = [os.environ.get('BUOY', 'buoy'), *buoy_arguments]
    target = linux_target(buoy_arguments)
    if target:
        command.append(f'--extra-obj={build_object(target, options.cc, options.sysroot)}')
    sys.exit(subprocess.run(command).returncode)


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(f'build_gtklifecycle: {error}')
