## 1. Manifest

- [x] 1.1 Rename the variants to `zephyr-standalone`, `zephyr-full`, `zephyr-ota`, `idf-full` and `nuttx-full`, with a `set` field each
- [x] 1.2 Add the `apps:` section, naming `tedge-agent-idf` `tedge-agent`
- [x] 1.3 `matrix.py`: the app name, `<os>-<set>` checked against the framework and set, unique firmware names, `tedge` from the set, job names `<app>-<os>-<device>`
- [x] 1.4 Tests for the new checks; the existing tests on the new names

## 2. Packaging and notes

- [x] 2.1 Packaging scripts and `meta.json`: the app name, not the directory
- [x] 2.2 Release notes: variants by operating system, and a one-time note on the new names
- [x] 2.3 Re-key `release/size-baseline.json`

## 3. Documentation

- [x] 3.1 README: the naming scheme, the regenerated table, examples
- [x] 3.2 DEVICES.md: a note mapping old names to new ones
- [ ] 3.3 Sync the `firmware-release` spec (when archiving)

## 4. Check

- [ ] 4.1 Release tests pass; a pull request builds every framework under the new names
