# json2dir-syscall

A Linux loadable kernel module (LKM) for x86-64 that turns a JSON object
into a directory tree, entirely in kernel space. It is a kernel-space
re-implementation of the conversion scheme used by the
[json2dir](https://github.com/alurm/json2dir) CLI.

Instead of reading JSON from stdin, userspace hands the JSON bytes to the
module through a character device or a sysfs attribute. The module parses the
JSON and creates files, directories, symlinks and executable scripts relative to a base
path embedded in the payload.

## Conversion scheme

The top-level JSON value must be an object. Each entry maps to a
filesystem object under the base directory:

| JSON value                | Result                              |
| ------------------------- | ----------------------------------- |
| object                    | directory (recurse into it)         |
| string                    | regular file with the string as its contents |
| `["link", "target"]`      | symbolic link pointing to `target`  |
| `["script", "contents"]`  | executable file containing `contents` (mode `+x`) |

- A key must be exactly one normal path component. Empty names, `.` and `..`,
  and names containing `/` or NUL are rejected. Other dots are allowed.
- Existing files and symlinks are removed before replacement; failures are
  reported. Existing directories are merged only with object values.
- Input must be valid UTF-8 and JSON. Duplicate object keys are rejected,
  including keys spelled with different JSON escapes. Parsing and validation
  finish before filesystem changes begin.
- Strings retain their decoded byte lengths: embedded NULs are preserved in
  file/script content and rejected in names, link targets and array kinds.
- Up to 128 simultaneously open JSON containers (including the root), 16384
  JSON values, and 4 MiB per request are supported. This includes the required
  64 nested directories with file, script and link leaves. Parsing, destruction
  and tree traversal do not recurse on the kernel stack.

## Interface

Write one complete request to `/dev/json2dir` (mode `0600`) with the payload:

```
<base-path>\0<json>
```

`<base-path>` is the directory under which the tree is created (absolute,
or relative to the writing process's cwd) and `<json>` is a JSON object.
The two parts are separated by a single NUL byte. A successful write
returns the number of bytes written; on failure a negative errno is
returned. Each write is a separate request, not a stream fragment. The 4 MiB
limit includes the base path and NUL separator.

The original `/sys/kernel/json2dir/data` endpoint remains available for requests
that fit within one sysfs page. Use `/dev/json2dir` for larger documents; a
userspace program must assemble the complete payload before calling `write`.

Example (as root):

```sh
# mkdir -p /tmp/tree
printf '/tmp/tree\0{"greeting":"Hello, world!","dir":{"subfile":"Content","subdir":{}},"symlink":["link","target path"],"script":["script","echo Howdy!"]}' \
    > /sys/kernel/json2dir/data
```

This creates, under `/tmp/tree`:

- `greeting` — regular file with contents `Hello, world!`
- `dir/subfile` — file with contents `Content`
- `dir/subdir` — empty directory
- `symlink` — symlink to `target path`
- `script` — executable file containing `echo Howdy!`

## Building

The module targets the kernel provided by `linuxPackages_latest`
(`linuxPackages_latest.kernel`). On NixOS / with Nix, a development shell
with the kernel headers, `make` and `gcc` is provided by `shell.nix`:

```sh
nix-shell --run make
```

This produces `json2dir.ko`. Without Nix, build against your kernel
headers in the usual way:

```sh
make KDIR=/lib/modules/$(uname -r)/build
```

Loading/unloading:

```sh
insmod json2dir.ko
rmmod json2dir
```

## Testing in QEMU

Because the built module is tied to the exact kernel version it was
compiled against (`linuxPackages_latest`), the module is exercised in a
QEMU VM booting that same kernel. `test.sh` builds the module, assembles a
minimal initramfs (static busybox + the module + a payload), boots it and
prints the resulting tree:

```sh
nix-shell --run ./test.sh
```

The init script (`initramfs/init`) mounts proc/sysfs/devtmpfs, loads the
module with `insmod`, writes the payload to the sysfs attribute and dumps
the created tree, the file contents and the file modes, then powers the VM
off. A successful run prints something like:

```
== insmod ==
insmod exit=0
== write payload ==
store exit=0
== tree ==
/work
/work/dir
/work/dir/subdir
/work/dir/subfile
/work/greeting
/work/script
/work/symlink
== greeting content ==
Hello, world!
== script content ==
echo Howdy!
== modes ==
...
-rwxr-xr-x ... script
lrwxrwxrwx ... symlink -> target path
== DONE ==
```

## Conformance tests

See [testing/README.md](testing/README.md) for the QEMU adapter and full tester
workflow:

```sh
nix-shell --run './testing/run-tests.sh --verbose'
```

The module is loaded only inside QEMU. The unmodified sibling
`json2dir-tester` supplies the inputs and judges the resulting filesystem trees.
The [recorded results](testing/results.md) cover all 367 applicable tests.

## Requirements

- Linux x86-64 kernel matching `linuxPackages_latest` (7.2.x at the time
  of writing)
- Nix (for `shell.nix`) or a matching kernel `build` tree and toolchain
- QEMU (`qemu_test`) for the VM smoke test

## Limitations

- Requests are limited to 4 MiB through `/dev/json2dir`; the compatibility
  sysfs endpoint retains its one-page limit.
- No TOCTOU protection against symlink races, matching the original
  tool's documented caveat.
- `script` entries are created with `0666` and then have the execute bits
  added with `notify_change`, matching the original's `mode | 0111`.
