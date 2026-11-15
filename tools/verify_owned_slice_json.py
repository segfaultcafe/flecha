#!/usr/bin/env python3
"""Audit library JSON owned arrays and every runtime allocation failure (GNU linker)."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--json-root', type=Path, required=True)
    parser.add_argument('--std-root', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--cxx', default='c++')
    args = parser.parse_args()
    fixtures = Path(__file__).resolve().parents[1] / 'src/tests/fixtures'
    with tempfile.TemporaryDirectory(prefix='flecha-json-owners-') as temporary:
        output = Path(temporary) / 'audit'
        obj = output.with_suffix('.o')
        subprocess.run([str(args.compiler.resolve()), '--emit-object',
                        str(fixtures / 'json_owned_slice_library.fl'),
                        '--lib', f'std={args.std_root.resolve()}',
                        '--lib', f'json={args.json_root.resolve() / "src"}',
                        '-o', str(obj)], check=True)
        subprocess.run([args.cxx, str(fixtures / 'json_owned_slice_library_harness.cpp'),
                        str(obj), str(args.runtime.resolve()),
                        '-Wl,--wrap=flecha_runtime_v1_alloc_array,--wrap=flecha_runtime_v1_alloc,--wrap=flecha_runtime_v1_free',
                        '-o', str(output)], check=True)
        environment = dict(os.environ)
        environment.pop('FAIL_AT', None)
        result = subprocess.run([str(output)], env=environment, capture_output=True, text=True)
        if result.returncode != 42:
            raise RuntimeError(f'baseline returned {result.returncode}: {result.stderr}')
        count = int(re.search(r'allocation_calls=(\d+)', result.stderr)[1])
        for index in range(1, count + 1):
            result = subprocess.run([str(output)], env={**environment, 'FAIL_AT': str(index)},
                                    capture_output=True, text=True)
            if result.returncode != 42:
                raise RuntimeError(f'failure position {index} returned {result.returncode}: {result.stderr}')
        print(f'Passed baseline and {count} allocation failure positions; no leaked or double-freed tracked allocations.')


if __name__ == '__main__':
    main()
