"""What to compile with, and what the linker will call the result.

Five tools here compile C for the machine they run on — the two recompilers
build their own output to prove it compiles, and three test harnesses build and
then *run* a program. All five named `clang`, which is one host's compiler
rather than this host's.

`CC` wins when it is set, which is what the Makefile exports so that a
`make CC=...` reaches the recompilers it shells out to. With nothing set, the
first name that exists: `cc` is the system compiler on macOS and Linux and is
the one name absent on MinGW, where it is `gcc`.

EXE matters only to the harnesses that run what they built. GCC on Windows
appends `.exe` to an -o argument with no extension, so a harness that names its
own output has to ask for the name the linker chose. Both non-native Pythons on
Windows are covered: the MSYS and Cygwin builds report a posix `os.name` while
their toolchains still produce `.exe`.
"""

import os
import shutil
import sys

EXE = ".exe" if os.name == "nt" or sys.platform in ("msys", "cygwin") else ""


def host_cc():
    if os.environ.get("CC"):
        return os.environ["CC"]
    for c in ("cc", "gcc", "clang"):
        if shutil.which(c):
            return c
    return "cc"
