#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the Smart Functions' tests in a tenant's Data Preparation runtime.

    cumulocity/smart-functions/test.py [rule-dir...]    # default: all of them

Sends each rule's JavaScript and tests/*.yaml to
/service/dataprep/v1/run-tests (nothing is deployed) and compares what it
returns with each test's expectedOutput. Needs a go-c8y-cli session.
"""

import json
import subprocess
import sys
from pathlib import Path

import yaml

HERE = Path(__file__).resolve().parent


def run(rule):
    cfg = yaml.safe_load((rule / "data-prep.yaml").read_text())
    js = (rule / cfg["smartFunctionFile"]).read_text()
    tests = {t.stem: yaml.safe_load(t.read_text())
             for t in sorted((rule / "tests").glob("*.yaml"))}
    body = {"config": {}, "jsCode": js,
            "tests": {n: {"inputs": t["inputs"]} for n, t in tests.items()}}
    # --data, not --file: --file sends a multipart upload.
    out = subprocess.run(["c8y", "api", "POST", "/service/dataprep/v1/run-tests",
                          "--data", json.dumps(body), "--raw", "--force"],
                         capture_output=True, text=True, stdin=subprocess.DEVNULL)
    if out.returncode != 0:
        print(f"{rule.name}: request failed\n{out.stderr}")
        return False
    results = json.loads(out.stdout)
    ok = True
    for name, t in tests.items():
        got = [o for r in results.get(name, []) for o in r.get("outputs", [])]
        errors = [r.get("error") for r in results.get(name, []) if r.get("error")]
        want = t.get("expectedOutput")
        if errors:
            print(f"  FAIL {rule.name}/{name}: {errors}")
            ok = False
        elif want is not None and json.loads(json.dumps(got)) != want:
            print(f"  FAIL {rule.name}/{name}:\n    got  {json.dumps(got)}\n    want {json.dumps(want)}")
            ok = False
        else:
            print(f"  ok   {rule.name}/{name}" + ("" if want is not None else f" -> {json.dumps(got)}"))
    return ok


def main(argv):
    rules = [Path(a) for a in argv] or sorted(p for p in HERE.iterdir()
                                             if (p / "data-prep.yaml").is_file())
    return 0 if all([run(r) for r in rules]) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
