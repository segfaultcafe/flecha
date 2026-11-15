#!/usr/bin/env python3
"""Check migrated JSON library compile-time contracts against a compiler build."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--json-root', type=Path, required=True)
    parser.add_argument('--std-root', type=Path, required=True)
    args = parser.parse_args()
    fixtures = Path(__file__).resolve().parents[1] / 'src/tests/fixtures/json_library_contract'
    cases = {
        'custom_provider': None,
        'duplicate_names': 'FLECHA-S0087',
        'private_field': 'FLECHA-S0088',
        'borrowed_array': 'FLECHA-S0058',
        'old_factory': 'FLECHA-S0018',
    }
    for name, diagnostic in cases.items():
        result = subprocess.run(
            [str(args.compiler.resolve()), 'check', str(fixtures / (name + '.fl')),
             '--lib', f'json={args.json_root.resolve() / "src"}',
             '--lib', f'std={args.std_root.resolve()}'],
            capture_output=True, text=True, timeout=60,
        )
        output = result.stdout + result.stderr
        expected_status = 0 if diagnostic is None else 1
        if result.returncode != expected_status or (diagnostic and diagnostic not in output):
            raise RuntimeError(f'{name}: unexpected status/diagnostic ({result.returncode}):\n{output}')
        print(f'{name}: passed')
    print(f'Passed {len(cases)} JSON library contract checks.')


if __name__ == '__main__':
    main()
