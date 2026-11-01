#!/usr/bin/env python3
"""CLI contracts, output failures, and POSIX replay cancellation; no third-party modules."""

import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import threading


SANITIZER = re.compile(
    r"AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|LeakSanitizer|runtime error:",
    re.IGNORECASE,
)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def check_diagnostics(stdout, stderr):
    check(not SANITIZER.search(stdout + stderr), "sanitizer diagnostic:\n" + stdout + stderr)


def invoke(executable, arguments, **kwargs):
    completed = subprocess.run(
        [str(executable), *arguments], capture_output=True, text=True, timeout=20, **kwargs
    )
    check_diagnostics(completed.stdout, completed.stderr)
    return completed


def invalid_inputs(cli):
    cases = [
        ["--workers", "0"],
        ["--not-an-option", "1"],
        ["--events"],
        ["--events", "-1"],
        ["--events", "123x"],
        ["--events", "18446744073709551616"],
        ["--journal", ""],
        ["--journal", "first", "--replay", "second"],
        ["--queue-capacity", "3", "--batch-size", "4"],
    ]
    for arguments in cases:
        result = invoke(cli, arguments)
        check(result.returncode == 2, f"{arguments}: expected exit 2, got {result.returncode}: {result.stderr}")
        check(result.stdout == "", f"{arguments}: unexpected stdout: {result.stdout}")
        document = json.loads(result.stderr)
        check(isinstance(document.get("error"), str) and document["error"], "missing JSON error")


def output_failure(executable, benchmark=False):
    arguments = ["--events", "23"]
    if benchmark:
        arguments += ["--repeats", "3", "--warmups", "1"]
    with open("/dev/full", "wb") as full:
        result = subprocess.run(
            [str(executable), *arguments], stdout=full, stderr=subprocess.PIPE,
            text=True, timeout=20,
        )
    check_diagnostics("", result.stderr)
    check(result.returncode == 1, f"{executable}: stdout failure returned {result.returncode}")
    check("failed writing stdout" in result.stderr, "output failure lacked diagnostic")


def replay_interrupt(cli, directory):
    journal = directory / "source.journal"
    recorded = invoke(cli, ["--events", "17", "--series", "4", "--journal", str(journal)])
    check(recorded.returncode == 0, "cannot prepare journal: " + recorded.stderr)
    source_result = json.loads(recorded.stdout)
    payload = journal.read_bytes()
    # This one small frame fits POSIX PIPE_BUF. Keeping the writer open prevents
    # EOF and successful replay completion until the test sends SIGINT.
    check(len(payload) <= 4096, "handshake frame exceeds POSIX PIPE_BUF")
    fifo = directory / "ready.fifo"
    os.mkfifo(fifo)
    process = subprocess.Popen(
        [str(cli), "--series", "4", "--replay", str(fifo)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    ready, release = threading.Event(), threading.Event()
    writer_failure = []

    def writer():
        try:
            with open(fifo, "wb", buffering=0) as stream:
                stream.write(payload)
                ready.set()  # A reader opened the FIFO after installing SIGINT.
                check(release.wait(10), "writer release deadline exceeded")
        except BaseException as failure:
            writer_failure.append(failure)
            ready.set()

    feeding = threading.Thread(target=writer, daemon=True)
    feeding.start()
    try:
        check(ready.wait(10), "CLI never reached replay FIFO handshake")
        check(not writer_failure, f"FIFO writer failed: {writer_failure}")
        check(process.poll() is None, "CLI exited before the interruption handshake")
        process.send_signal(signal.SIGINT)
        release.set()
        stdout, stderr = process.communicate(timeout=20)
    finally:
        release.set()
        if process.poll() is None:
            process.kill()
            process.communicate(timeout=5)
        feeding.join(timeout=2)
    check(not writer_failure, f"FIFO writer failed: {writer_failure}")
    check_diagnostics(stdout, stderr)
    check(process.returncode == 130, f"ready replay SIGINT exited {process.returncode}: {stdout} {stderr}")
    document = json.loads(stdout)
    result = document["run"]
    check(result["cancelled"] is True, "replay SIGINT omitted cancellation")
    check(0 <= result["accepted"] <= 17, "replay accepted count outside source prefix")
    check(result["accepted"] == result["processed"] + result["discarded"], "replay cancellation lost accepted events")
    check(isinstance(document.get("error"), str), "replay cancellation omitted reason")
    check("truncated_tail" in document, "replay tail status omitted")
    if document["replay_complete"]:
        check(document["truncated_tail"] is False, "complete FIFO replay tail status")
    else:
        check(document["truncated_tail"] is None, "cancelled prefix must not claim known tail status")

    full = invoke(cli, ["--series", "4", "--replay", str(journal)])
    check(full.returncode == 0, "uninterrupted replay failed")
    replay = json.loads(full.stdout)
    check(replay["run"]["fingerprint"] == source_result["fingerprint"], "source/replay payload fingerprints differ")
    check(replay["run"]["processed"] == 17 and replay["replay_complete"] is True, "full replay accounting")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--benchmark", type=Path)
    settings = parser.parse_args()
    cli = settings.cli.resolve()
    invalid_inputs(cli)
    if os.name == "posix" and Path("/dev/full").exists():
        output_failure(cli)
        if settings.benchmark:
            output_failure(settings.benchmark.resolve(), benchmark=True)
    else:
        print("POSIX /dev/full checks unavailable on this platform")
    if os.name == "posix" and hasattr(os, "mkfifo"):
        with tempfile.TemporaryDirectory(prefix="strata-cli-audit-") as directory:
            replay_interrupt(cli, Path(directory))
    else:
        print("POSIX FIFO/SIGINT check unavailable on this platform")
    print("CLI audit contracts passed")


if __name__ == "__main__":
    main()
