#!/usr/bin/env python3
"""Run a native filter in a new directory with an experiment manifest."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
from datetime import datetime, timezone


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as file:
        for block in iter(lambda: file.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def source_revision():
    """Identify the source checkout when git is available; do not infer binary provenance."""
    try:
        result = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=Path(__file__).resolve().parent,
                                capture_output=True, text=True, timeout=5, check=True)
        status = subprocess.run(['git', 'status', '--porcelain'], cwd=Path(__file__).resolve().parent,
                                capture_output=True, text=True, timeout=5, check=True)
        return {'commit': result.stdout.strip(), 'tracked_or_untracked_changes': bool(status.stdout.strip())}
    except (OSError, subprocess.SubprocessError):
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True, help='new directory for run artifacts')
    parser.add_argument('--build-command', required=True, help='exact compiler command and flags')
    parser.add_argument('--notes', default='', help='hardware and power settings')
    args = parser.parse_args()
    binary, source, output = args.binary.resolve(), args.input.resolve(), args.output_dir.resolve()
    if platform.system() not in ('Linux', 'Windows'):
        parser.error('supported systems are Linux and Windows')
    if not binary.is_file() or not source.is_file():
        parser.error('binary and input must exist')
    try:
        output.mkdir(parents=True, exist_ok=False)
        system = platform.system()
        for relative in ('common', 'logs', f'results/{system}', f'{system}/Kalman_Filter'):
            (output / relative).mkdir(parents=True)
        shutil.copyfile(source, output / 'common/sensor_data.csv')
        manifest = {
            'started_utc': datetime.now(timezone.utc).isoformat(),
            'platform': system, 'platform_description': platform.platform(),
            'machine': platform.machine(), 'processor': platform.processor(),
            'logical_cpus': os.cpu_count(), 'python_version': platform.python_version(),
            'binary': str(binary), 'binary_sha256': sha256(binary),
            'input': str(source), 'input_sha256': sha256(source),
            'build_command': args.build_command, 'source_checkout': source_revision(), 'notes': args.notes,
        }
        manifest_path = output / 'manifest.json'
        manifest_path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        with (output / 'execution.log').open('w', encoding='utf-8') as log:
            result = subprocess.run([str(binary)], cwd=output, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=600, check=False)
        manifest['exit_code'] = result.returncode
        manifest['finished_utc'] = datetime.now(timezone.utc).isoformat()
        manifest_path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        print(f'Experiment directory: {output}; exit code: {result.returncode}')
        return 0 if result.returncode == 0 else 1
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f'Experiment failed: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
