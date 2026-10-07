# Conformance results — 2026-10-08

The unmodified `json2dir-tester` reports **367/367 passed, 0 failed, 0 skipped**.
Full machine-readable output: [results.json](results.json).

| Suite | Passed |
| --- | ---: |
| conformance/core | 52/52 |
| conformance/overwrite | 16/16 |
| security/core | 10/10 |
| upstream/cli | 2/2 |
| upstream/core | 255/255 |
| upstream/overwrite | 32/32 |

The original module passed 64/68 conformance cases. It failed embedded NUL file
content, 64-level nesting, invalid UTF-8, and NUL in a member name.

The final run used `testing/run-tests.sh --verbose`, Linux **7.2.1**, QEMU
**10.2.4** with TCG, .NET SDK **10.0.400**, and a persistent guest with a fresh
filesystem sandbox for every case. The LKM, rather than a userspace model,
parsed the input and performed filesystem operations. The guest writer ran as
uid 1000/gid 100 without capabilities, using each case's umask.

Reproduce with the [test instructions](README.md). The exact host paths are in
`.build/vm-config.json`; logs are in `.build/reports/`. The kernel build tree and
boot image must match; 7.2.9 and 9p mentioned in the tester's existing descriptor
are not required by this adapter.

Reference revisions:

- awesome-json2dir: `8cbaecd62fe582084cbbb7be2813f7e481d0582f`
- json2dir-tester: `f06440f17b7b3ebff43931f40b1a63c9c3fd2e9e`
- Tested `json2dir.c` SHA-256: `e4d67af7a01f757d26e7e1ccf04e70c8b247cae9b45739232ee1f32a95ea9d5f`

The implementation deliberately rejects duplicate keys and names with trailing
slashes, both permitted by RFC J2D-1. Such cases may allow rejection in the
original suites; those expectations were not changed. The result covers every
case applicable to `json2dir-syscall`; implementation-specific cases for other
projects are excluded by the original tester's `only` selection.

Resource limits remain explicit: 4 MiB per character-device request, 16384 JSON
values, and 128 open containers including the root. The sysfs compatibility
interface remains limited to one page. Symlink races involving concurrently
modified ancestor paths remain outside the implementation's protection.
