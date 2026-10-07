#!/usr/bin/env bash
# Build and test the real LKM without loading it into the host kernel.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
cd "$project"
: "${KDIR:?Set KDIR to the matching kernel build tree (or enter nix-shell)}"
: "${BZIMAGE:?Set BZIMAGE to the matching kernel bzImage}"
: "${BUSYBOX:?Set BUSYBOX to a static busybox binary}"
export QEMU="${QEMU:-$(command -v qemu-system-x86_64)}"
export JSON2DIR_TESTER="${JSON2DIR_TESTER:-$project/../json2dir-tester}"
export JSON2DIR_BUILD="$project/.build"
export DOTNET_CLI_HOME="$JSON2DIR_BUILD/dotnet-home"
export NUGET_PACKAGES="$JSON2DIR_BUILD/nuget"
export DOTNET_CLI_TELEMETRY_OPTOUT=1
export DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
export DOTNET_GENERATE_ASPNET_CERTIFICATE=false
export PYTHONDONTWRITEBYTECODE=1
mkdir -p "$JSON2DIR_BUILD"/{tmp,sandboxes,reports,runtimes/json2dir-syscall-vm}
make KDIR="$KDIR"
dotnet build "$JSON2DIR_TESTER/src/Json2dirTester/Json2dirTester.csproj" \
    -c Release --artifacts-path "$JSON2DIR_BUILD/tester" -p:UseAppHost=false --nologo -v minimal
python3 - <<'PY'
import json, os
from pathlib import Path
config = {key: str(Path(os.environ[env]).resolve()) for key, env in
          [('kernel', 'BZIMAGE'), ('busybox', 'BUSYBOX'), ('qemu', 'QEMU')]}
Path(os.environ['JSON2DIR_BUILD'], 'vm-config.json').write_text(json.dumps(config, indent=2) + '\n')
PY
python3 testing/vm.py --prepare-server
ln -sfn ../../../testing/vm.py "$JSON2DIR_BUILD/runtimes/json2dir-syscall-vm/run.sh"
export JSON2DIR_VM_PIPE="$JSON2DIR_BUILD/vm.pipe"
rm -f "$JSON2DIR_VM_PIPE"
python3 testing/vm-server.py > "$JSON2DIR_BUILD/reports/vm.log" 2>&1 &
vm_pid=$!
cleanup() { kill "$vm_pid" 2>/dev/null || true; wait "$vm_pid" 2>/dev/null || true; }
trap cleanup EXIT
for ((attempt=0; attempt<600; attempt++)); do
    [[ ! -p "$JSON2DIR_VM_PIPE" ]] || break
    if ! kill -0 "$vm_pid" 2>/dev/null; then cat "$JSON2DIR_BUILD/reports/vm.log" >&2; exit 1; fi
    sleep 0.1
done
[[ -p "$JSON2DIR_VM_PIPE" ]] || { echo 'VM did not become ready' >&2; exit 1; }
# The unmodified tester must read files with mode 0000. A private tmpfs and
# capabilities confined to its user namespace allow this without host root.
# Guest writes still use uid 1000 with no capabilities, including permission tests.
unshare -c --keep-caps -m bash -c '
    set -euo pipefail
    mount -t tmpfs tmpfs "$JSON2DIR_BUILD/sandboxes"
    export TMPDIR="$JSON2DIR_BUILD/sandboxes"
    cd "$JSON2DIR_TESTER"
    exec dotnet "$JSON2DIR_BUILD/tester/bin/Json2dirTester/release/json2dir-tester.dll" \
        run json2dir-syscall --runtimes "$JSON2DIR_BUILD/runtimes" \
        --json "$JSON2DIR_BUILD/reports/results.json" "$@"
' _ "$@" | tee "$JSON2DIR_BUILD/reports/results.log"
