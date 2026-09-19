#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check that no log statement in the module prints a secret.

The client handles four things that must never reach a log: the
remote-access connection key, the enrollment one-time password (and the
registration URL that carries it, above debug level), the device password
from bootstrap credentials, and the JWT. This catches the easy mistake of
passing one of those variables to a LOG_* macro.

Exit status is 0 when clean, 1 when a log statement looks suspicious.
"""

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src"
# Variables that hold a secret in this module.
SECRETS = re.compile(r"(?:\bconn_key\b|->key\b|\bkey\b|\botp\b|"
                     r"\bpassword\b|\bdev_password\b|\bboot_password\b|"
                     r"\bpsk\b|\bjwt\b|\btoken\b|\bbasic_b64\b|"
                     r"\bauth_hdr\b)")
LOG_CALL = re.compile(r"\bLOG_(ERR|WRN|INF|DBG|HEXDUMP_\w+)\s*\(")


def check(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8")
    problems = []
    for match in LOG_CALL.finditer(text):
        # Take the call's arguments, up to the closing ");" of the statement.
        end = text.find(");", match.end())
        call = text[match.end():end if end > 0 else match.end()]
        # The format string itself may mention the word; only its arguments
        # matter, so drop the quoted parts.
        args = re.sub(r'"(?:[^"\\]|\\.)*"', "", call)
        hit = SECRETS.search(args)
        if hit:
            line = text[:match.start()].count("\n") + 1
            problems.append(f"{path.name}:{line}: log statement uses "
                            f"'{hit.group(0)}'")
    return problems


def main() -> int:
    problems = []
    for path in sorted(SRC.glob("*.c")):
        problems.extend(check(path))
    for problem in problems:
        print(problem)
    if problems:
        print(f"{len(problems)} log statement(s) may print a secret")
        return 1
    print("no log statement prints a secret")
    return 0


if __name__ == "__main__":
    sys.exit(main())
