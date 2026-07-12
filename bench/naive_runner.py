#!/usr/bin/env python3
"""Naive runner (NV): the common agent pattern.

Usage: naive_runner.py command [args...]

On timeout it kills only the direct child, then waits for pipe EOF. It exits
with the child's code, or 128 + N if signal N killed the child.
"""

import subprocess
import sys


def main():
    p = subprocess.Popen(sys.argv[1:], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        out, err = p.communicate(timeout=2)
    except subprocess.TimeoutExpired:
        p.kill()
        out, err = p.communicate()
    sys.stdout.buffer.write(out)
    sys.stderr.buffer.write(err)
    return p.returncode if p.returncode >= 0 else 128 - p.returncode


if __name__ == "__main__":
    sys.exit(main())
