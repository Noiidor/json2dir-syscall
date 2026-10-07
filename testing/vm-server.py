#!/usr/bin/env python3
"""Keep one isolated guest alive; each request starts with a fresh /work tree."""
import base64
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

from vm import BUILD, CONFIG


def main():
    config = json.loads(CONFIG.read_text())
    pipe = Path(os.environ['JSON2DIR_VM_PIPE'])
    pipe.unlink(missing_ok=True)
    cmd = [config['qemu'], '-accel', 'tcg', '-m', '256M', '-smp', '1',
           '-display', 'none', '-monitor', 'none', '-serial', 'stdio',
           '-kernel', config['kernel'], '-initrd', str(BUILD / 'server.cpio.gz'),
           '-append', 'console=ttyS0 rdinit=/init panic=1 quiet', '-no-reboot', '-nic', 'none']
    with subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=sys.stderr) as proc:
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
        try:
            def line():
                value = proc.stdout.readline()
                if not value:
                    raise RuntimeError('guest disconnected')
                return value.rstrip(b'\r\n')
            while line() != b'J2D_READY':
                pass
            os.mkfifo(pipe, 0o600)
            print('VM ready', flush=True)
            while True:
                with pipe.open() as requests:
                    for name in requests:
                        request = Path(name.rstrip('\n'))
                        content = request.read_bytes()
                        if len(content) > 32 * 1024 * 1024:
                            raise RuntimeError('oversized VM request')
                        proc.stdin.write(base64.b64encode(content) + b'\n')
                        proc.stdin.flush()
                        result = {}
                        while True:
                            value = line()
                            if value == b'J2D_READY':
                                break
                            key, _, data = value.partition(b'=')
                            if key in (b'J2D_RESULT', b'J2D_STDERR', b'J2D_STDOUT', b'J2D_TREE'):
                                result[key.decode()] = data.decode()
                            else:
                                print(value.decode(errors='replace'), file=sys.stderr)
                        if len(result) != 4:
                            raise RuntimeError('incomplete guest response')
                        response = request.with_suffix('.json')
                        response.with_suffix('.partial').write_text(json.dumps(result))
                        response.with_suffix('.partial').replace(response)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
            pipe.unlink(missing_ok=True)

if __name__ == '__main__':
    main()
