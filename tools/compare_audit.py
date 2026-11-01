"""Interleave two builds of benchmarks/audit's identical measurement harness."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--before-ref', required=True)
    parser.add_argument('--after-ref', required=True)
    parser.add_argument('--repeats', type=int, default=7)
    parser.add_argument('--warmups', type=int, default=1)
    parser.add_argument('--cpu-list', help='optional taskset list, recorded with the run')
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('workload', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not 3 <= args.repeats <= 100 or not 1 <= args.warmups <= 10 or args.timeout <= 0:
        parser.error('repeats=3..100, warmups=1..10, positive timeout required')
    command_args = args.workload[1:] if args.workload[:1] == ['--'] else args.workload
    paths = [args.before.resolve(), args.after.resolve()]
    names = ['before', 'after']
    metadata = {'type': 'environment', 'python': sys.version.split()[0],
                'platform': platform.platform(), 'cpu_count': os.cpu_count(),
                'affinity': sorted(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else None,
                'taskset': args.cpu_list, 'repeats': args.repeats, 'warmups_per_binary': args.warmups,
                'workload': command_args, 'refs': [args.before_ref, args.after_ref],
                'binary_sha256': [sha256(path) for path in paths],
                'harness_sha256': sha256(Path(__file__).resolve().parents[1] / 'benchmarks/audit/ingest.cpp')}
    print(json.dumps(metadata), flush=True)
    rates = [[], []]
    expected_fingerprint = None
    for iteration in range(args.warmups + args.repeats):
        for step in range(2):
            group = (iteration + step) % 2
            command = [str(paths[group]), *command_args]
            if args.cpu_list:
                command = ['taskset', '-c', args.cpu_list, *command]
            completed = subprocess.run(command, text=True, capture_output=True, timeout=args.timeout)
            if completed.returncode != 0 or completed.stderr:
                raise RuntimeError(f'failed {names[group]}: {completed.returncode}: {completed.stderr}')
            run = json.loads(completed.stdout)
            if not run['validated'] or run['processed'] != run['events']:
                raise RuntimeError('unvalidated measurement')
            if expected_fingerprint is None:
                expected_fingerprint = run['fingerprint']
            if run['fingerprint'] != expected_fingerprint:
                raise RuntimeError('different source observations across measurements')
            warmup = iteration < args.warmups
            print(json.dumps({'type': 'sample', 'variant': names[group], 'warmup': warmup,
                              'iteration': iteration - args.warmups, 'order': step, 'run': run}), flush=True)
            if not warmup:
                rates[group].append(run['events_per_second'])
    summaries = []
    for group in rates:
        summaries.append({'median': statistics.median(group), 'minimum': min(group), 'maximum': max(group)})
    print(json.dumps({'type': 'summary', 'before': summaries[0], 'after': summaries[1],
                      'ratio_of_medians': summaries[1]['median'] / summaries[0]['median']}), flush=True)


if __name__ == '__main__':
    main()
