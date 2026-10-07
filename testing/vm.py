#!/usr/bin/env python3
"""Run one unmodified JSON input through the LKM in an isolated QEMU guest."""
import base64
import gzip
import io
import json
import os
from pathlib import Path
import re
import shutil
import time
import stat
import subprocess
import sys
import tarfile
import tempfile

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent
BUILD = PROJECT / '.build'
CONFIG = BUILD / 'vm-config.json'


def archive(entries):
    out = bytearray()
    for index, (name, data, mode) in enumerate(entries + [('TRAILER!!!', b'', 0)]):
        name = name.encode() + b'\0'
        fields = [index + 1, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name), 0]
        out += b'070701' + ''.join(f'{x:08x}' for x in fields).encode() + name
        out += b'\0' * (-len(out) % 4)
        out += data
        out += b'\0' * (-len(out) % 4)
    return gzip.compress(bytes(out), mtime=0)


def prepare(server=False):
    config = json.loads(CONFIG.read_text())
    subprocess.run(['gcc', '-O2', '-Wall', '-Wextra', str(HERE / 'client.c'), '-o', str(BUILD / 'client')], check=True)
    entries = [(d, b'', stat.S_IFDIR | 0o755) for d in ['bin', 'proc', 'sys', 'dev', 'tmp']]
    files = {'bin/busybox': Path(config['busybox']), 'client': BUILD / 'client',
             'init': HERE / ('vm-server-init' if server else 'vm-init'), 'json2dir.ko': PROJECT / 'json2dir.ko'}
    ldd = subprocess.check_output(['ldd', str(BUILD / 'client')], text=True)
    for path in re.findall(r'/[^\s()]+', ldd):
        files[path.lstrip('/')] = Path(path)
    dirs = set()
    for name, path in files.items():
        for parent in reversed(Path(name).parents):
            if str(parent) != '.' and str(parent) not in dirs and str(parent) not in ['bin']:
                entries.append((str(parent), b'', stat.S_IFDIR | 0o755))
                dirs.add(str(parent))
        entries.append((name, path.read_bytes(), stat.S_IFREG | 0o755))
    (BUILD / ('server.cpio.gz' if server else 'base.cpio.gz')).write_bytes(archive(entries))


def run():
    if len(sys.argv) != 1:
        print('Usage: json2dir < document.json', file=sys.stderr)
        return 1
    config = json.loads(CONFIG.read_text())
    # Capture the runner's umask, but keep infrastructure readable in the guest.
    mask = os.umask(0o022)
    data = sys.stdin.buffer.read()
    root = Path.cwd()
    sandbox = root.parent
    temp_root = BUILD / 'tmp'
    temp_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='vm-', dir=temp_root) as td:
        tmp = Path(td)
        setup = io.BytesIO()
        with tarfile.open(fileobj=setup, mode='w') as tf:
            def owner(info):
                info.uid, info.gid = 1000, 100
                info.uname = info.gname = ''
                return info
            for p in sandbox.iterdir():
                tf.add(p, arcname=p.name, filter=owner)
        if os.environ.get('JSON2DIR_VM_PIPE'):
            request = io.BytesIO()
            with tarfile.open(fileobj=request, mode='w:gz') as tf:
                for name, content in [('setup.tar', setup.getvalue()), ('input', data), ('mask', f'{mask:o}'.encode())]:
                    info = tarfile.TarInfo(name)
                    info.size = len(content)
                    tf.addfile(info, io.BytesIO(content))
            request_path = tmp / 'request.tar.gz'
            request_path.write_bytes(request.getvalue())
            deadline = time.monotonic() + 55
            while True:
                try:
                    fd = os.open(os.environ['JSON2DIR_VM_PIPE'], os.O_WRONLY | os.O_NONBLOCK)
                    break
                except OSError:
                    if time.monotonic() > deadline:
                        raise RuntimeError('VM server unavailable')
                    time.sleep(.01)
            with os.fdopen(fd, 'w') as pipe:
                pipe.write(str(request_path) + '\n')
            response_path = request_path.with_suffix('.json')
            while not response_path.exists():
                if time.monotonic() > deadline:
                    raise RuntimeError('VM request timed out')
                time.sleep(.01)
            response = json.loads(response_path.read_text())
            status = int(response['J2D_RESULT'])
            sys.stdout.buffer.write(base64.b64decode(response['J2D_STDOUT']))
            sys.stderr.buffer.write(base64.b64decode(response['J2D_STDERR']))
            result = io.BytesIO(base64.b64decode(response['J2D_TREE']))
            console = ''
        else:
            extra = archive([(name, content, stat.S_IFREG | 0o644) for name, content in
                             [('setup.tar', setup.getvalue()), ('input', data), ('mask', f'{mask:o}'.encode())]])
            initrd = tmp / 'initrd.gz'
            initrd.write_bytes((BUILD / 'base.cpio.gz').read_bytes() + extra)
            log = tmp / 'console.log'
            result = tmp / 'result.tar'
            cmd = [config['qemu'], '-accel', 'tcg', '-m', '256M', '-smp', '1', '-display', 'none',
                   '-monitor', 'none', '-serial', f'file:{log}', '-serial', f'file:{result}',
                   '-kernel', config['kernel'], '-initrd', str(initrd),
                   '-append', 'console=ttyS0 rdinit=/init panic=1 quiet', '-no-reboot', '-nic', 'none']
            try:
                proc = subprocess.run(cmd, capture_output=True, timeout=45)
            except subprocess.TimeoutExpired:
                print('QEMU timed out', file=sys.stderr)
                return 126
            console = log.read_text(errors='replace')
            match = re.search(r'J2D_RESULT=(\d+)', console)
            if proc.returncode or not match or 'J2D_DONE' not in console or 'J2D_INFRA_FAILURE' in console:
                print(console[-6000:], file=sys.stderr)
                print(proc.stderr.decode(errors='replace'), file=sys.stderr)
                return 126
            status = int(match[1])
            if status == 126:
                print(console, file=sys.stderr)
                return 126
        # Export the entire sandbox, including siblings of root, for escape checks.
        with (tarfile.open(fileobj=result) if isinstance(result, io.BytesIO) else tarfile.open(result)) as tf:
            members = tf.getmembers()
            symlinks = {Path(m.name) for m in members if m.issym()}
            for member in members:
                parts = Path(member.name).parts
                if member.name.startswith('/') or '..' in parts or member.islnk() or member.isdev():
                    raise RuntimeError('unsafe VM result archive')
                for parent in Path(member.name).parents:
                    if parent in symlinks:
                        raise RuntimeError('archive path traverses symlink')
            for p in sandbox.iterdir():
                if p.is_dir() and not p.is_symlink():
                    shutil.rmtree(p)
                else:
                    p.unlink()
            # Symlink targets are opaque format data, including absolute targets.
            tf.extractall(sandbox, members=members, filter='fully_trusted', numeric_owner=False)
        if status:
            print('\n'.join(line for line in console.splitlines() if line.startswith('json2dir:')), file=sys.stderr)
        return status

if __name__ == '__main__':
    if sys.argv[1:] in (['--prepare'], ['--prepare-server']):
        prepare(server=sys.argv[1] == '--prepare-server')
    else:
        try:
            sys.exit(run())
        except Exception as exc:
            print(f'VM infrastructure: {exc}', file=sys.stderr)
            sys.exit(126)
