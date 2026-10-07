# LKM conformance tests

From the project root, with the sibling `json2dir-tester` checkout present:

```sh
nix-shell --run './testing/run-tests.sh --verbose'
```

Without Nix, provide `KDIR`, `BZIMAGE`, and `BUSYBOX` (a statically linked
BusyBox), and put GCC, GNU Make, QEMU, Python 3.12+, .NET 10 SDK, gzip and
util-linux (`unshare`, `mount`) on `PATH`. `QEMU` and `JSON2DIR_TESTER` can
override the QEMU binary and tester checkout. The kernel must match `KDIR`
and support modules, initramfs, devtmpfs, procfs, sysfs, tmpfs and serial
consoles. No 9p modules or KVM are needed.

`run-tests.sh` builds the module and the **unmodified** C# tester, then starts
a QEMU TCG guest. It forwards additional arguments such as `--suite conformance`
or `--filter nul` to the tester. Artifacts, temp files and reports stay in
`.build/`; the tester checkout and the test cases are not modified.

The guest loads the module once per campaign. Before every case it recreates
`/work` from the tester's setup tree, including modes and symlinks. The C client
opens the device as guest root, drops all groups and switches to uid 1000/gid
100, sets the case's umask, and writes `.<NUL><stdin>` in **one write**. It does
not parse or transform JSON. The module performs every tested conversion.
The entire guest sandbox is returned as an archive so the tester can compare
both the target tree and any entries created beside it. CLI arguments are
rejected by the transport wrapper with a usage message.

The test reader runs as the calling uid in a private user/mount namespace with
a tmpfs for test trees. Its namespace-local capabilities let the original
tester read mode-0000 files. These capabilities are **not** granted to the guest
writer: all permission tests exercise an ordinary guest user. User namespaces
must be enabled. No sudo or host module loading is used.

The existing tester descriptor mentions kernel 7.2.9 and 9p. Its command path is
reused through `--runtimes`; this adapter instead uses the configured matching
kernel, serial transport, and a fresh tmpfs tree per case. The actual kernel and
QEMU paths are saved in `.build/vm-config.json`.

Results: `.build/reports/results.json` and `results.log`; guest diagnostics:
`.build/reports/vm.log`. Infrastructure failures return 126, which the C# tester
treats as failures even for cases expecting an invalid-document rejection.

For a fresh VM per case, run `python3 testing/vm.py --prepare` after configuring
`.build/vm-config.json`, then use the absolute path to `testing/vm.py` as the
command for `awesome-json2dir/conformance/run.py`, without `JSON2DIR_VM_PIPE`.
This fallback accepts the same stdin/cwd interface but boots more slowly.
