# json2dir-syscall

A Linux loadable kernel module (LKM) for x86-64 that turns a JSON object
into a directory tree, entirely in kernel space. It is a kernel-space
re-implementation of the conversion scheme used by the
[json2dir](https://github.com/alurm/json2dir) CLI.

Instead of reading JSON from stdin, userspace hands the JSON bytes to the
module through a sysfs attribute. The module parses the JSON and creates
files, directories, symlinks and executable scripts relative to a base
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

- A key must be exactly one normal path component. Keys containing `/`,
  `.` or `..` (or an empty key) are rejected.
- Before creating an entry, any existing file or symlink at that path is
  removed (errors are ignored), mirroring the original `json2dir`.
- Objects may be nested to any depth up to the module's limit (64); the
  JSON parser also caps the total number of nodes (4096) and the payload
  is limited to a single page (4096 bytes) because it arrives through a
  regular sysfs attribute.

## Interface

Write to `/sys/kernel/json2dir/data` with the payload:

```
<base-path>\0<json>
```

`<base-path>` is the directory under which the tree is created (absolute,
or relative to the writing process's cwd) and `<json>` is a JSON object.
The two parts are separated by a single NUL byte. A successful write
returns the number of bytes written; on failure a negative errno is
returned.

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

## Requirements

- Linux x86-64 kernel matching `linuxPackages_latest` (7.2.x at the time
  of writing)
- Nix (for `shell.nix`) or a matching kernel `build` tree and toolchain
- QEMU (`qemu_test`) for the VM smoke test

## Limitations

- Payload limited to one page (4096 bytes) by the sysfs write path.
- No TOCTOU protection against symlink races, matching the original
  tool's documented caveat.
- `script` entries are created with `0666` and then have the execute bits
  added with `notify_change`, matching the original's `mode | 0111`.
