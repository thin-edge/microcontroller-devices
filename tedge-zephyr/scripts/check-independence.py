#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check that tedge-zephyr stays self-contained.

The module must be able to move into its own repository unchanged, so no code
or build file in it may reach outside its directory, or name the internals of
the repository that incubates it. The script checks:

- relative paths ("../...") that resolve outside the module directory;
- references to the incubating repository's modules (lib/common, lib/opcua,
  lib/modbus, lib/snmp, lib/mcuboot-hooks, lib/frontend-template) or apps/;
- includes of lib/common headers by their bare names (net.h, identity.h, ...).

Documentation (*.md) is not checked; prose may talk about host applications.

Exit status is 0 when clean, 1 when any violation is found.
"""

import re
import sys
from pathlib import Path

MODULE = Path(__file__).resolve().parent.parent
SELF = Path(__file__).resolve()

CODE_SUFFIXES = {
    ".c", ".h", ".cmake", ".conf", ".yml", ".yaml", ".overlay", ".dts",
    ".dtsi", ".sh", ".py", ".ld",
}
CODE_NAMES = {"CMakeLists.txt", "Kconfig"}

RELATIVE = re.compile(r"(?:\.\./)+[^\s\"'()<>;]*|(?<![\w.])\.\.(?=[\s\"')]|$)")
# "apps/" only counts as a repository path, not inside a URL such as
# Cumulocity's .../apps/devicemanagement/... registration link.
HOST_PATHS = re.compile(
    r"\blib/(?:common|opcua|modbus|snmp|mcuboot-hooks|frontend-template)\b"
    r"|(?<![\w/-])apps/"
)
HOST_HEADERS = re.compile(
    r"#\s*include\s*[<\"](?:net|identity|data_source|controls|status_led"
    r"|liveness|diag|display|boot_request|button_gesture|prov_\w+)\.h[>\"]"
)


def is_code(path: Path) -> bool:
    return path.name in CODE_NAMES or path.name.startswith("Kconfig") \
        or path.suffix in CODE_SUFFIXES


def check_file(path: Path) -> list[str]:
    problems = []
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return problems
    rel = path.relative_to(MODULE)

    for lineno, line in enumerate(text.splitlines(), start=1):
        for match in RELATIVE.finditer(line):
            target = (path.parent / match.group(0)).resolve()
            if target != MODULE and MODULE not in target.parents:
                problems.append(
                    f"{rel}:{lineno}: path '{match.group(0)}' leaves the module"
                )
        if HOST_PATHS.search(line):
            problems.append(
                f"{rel}:{lineno}: references the host repository: {line.strip()}"
            )
        if HOST_HEADERS.search(line):
            problems.append(
                f"{rel}:{lineno}: includes a host-repository header: {line.strip()}"
            )
    return problems


def main() -> int:
    problems = []
    for path in sorted(MODULE.rglob("*")):
        if not path.is_file() or path == SELF or not is_code(path):
            continue
        problems.extend(check_file(path))

    for problem in problems:
        print(problem)
    if problems:
        print(f"tedge-zephyr is not self-contained: {len(problems)} problem(s)")
        return 1
    print("tedge-zephyr is self-contained")
    return 0


if __name__ == "__main__":
    sys.exit(main())
