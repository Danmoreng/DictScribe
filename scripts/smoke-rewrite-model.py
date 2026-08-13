#!/usr/bin/env python3
"""Run the Phase-1 rewrite-model compatibility and prompt smoke test."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path


CASES = (
    (
        "German correction and paragraph",
        "de",
        "Das Modell braucht zwei Gigabyte nein achthundert Megabyte neuer Absatz dadurch sollte es auch auf kleineren Geräten laufen",
    ),
    (
        "German shopping list",
        "de",
        "Einkaufsliste Doppelpunkt Brot Mehl Milch Müsli",
    ),
    (
        "English ordered list",
        "en",
        "there are three tasks first load the model second clean the current tail and third insert the result",
    ),
    (
        "no-op prose",
        "en",
        "Qwen3.5-0.8B is the first model we will benchmark.",
    ),
    (
        "mixed technical text",
        "en",
        "Set DICTSCRIBE_REWRITE_MODEL to C colon slash models slash Qwen3.5-0.8B-Q8_0 dot gguf",
    ),
)


def read_message(process: subprocess.Popen[str]) -> dict:
    line = process.stdout.readline()
    if not line:
        raise RuntimeError("rewrite worker exited before returning a protocol message")
    return json.loads(line)


def send(process: subprocess.Popen[str], message: dict) -> None:
    process.stdin.write(json.dumps(message, ensure_ascii=False) + "\n")
    process.stdin.flush()


def wait_for(process: subprocess.Popen[str], expected: str) -> dict:
    while True:
        message = read_message(process)
        if message.get("type") == "error":
            raise RuntimeError(message.get("message", "rewrite worker error"))
        if message.get("type") == expected:
            return message


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--worker", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    args = parser.parse_args()
    if not args.worker.is_file():
        parser.error(f"worker not found: {args.worker}")
    if not args.model.is_file():
        parser.error(f"model not found: {args.model}")

    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")

    process = subprocess.Popen(
        [
            str(args.worker),
            "--stdio",
            "--model",
            str(args.model),
            "--protocol-version",
            "1",
            "--context-size",
            "2048",
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        encoding="utf-8",
    )
    try:
        ready = wait_for(process, "ready")
        architecture = ready.get("modelArchitecture")
        has_template = ready.get("chatTemplateAvailable")
        if architecture != "qwen35":
            raise RuntimeError(f"expected qwen35 architecture, got {architecture!r}")
        if has_template is not True:
            raise RuntimeError("the GGUF does not expose a chat template")
        print(f"Loaded architecture={architecture}, chat_template={has_template}")

        for number, (name, language, transcript) in enumerate(CASES, start=1):
            request_id = f"smoke-{number}"
            started = time.perf_counter()
            send(
                process,
                {
                    "v": 2,
                    "type": "rewrite_tail",
                    "id": request_id,
                    "requestId": request_id,
                    "sessionId": "smoke-session",
                    "tailRevision": number - 1,
                    "firstStableSpanId": number,
                    "lastStableSpanId": number,
                    "languageHint": language,
                    "readOnlyContext": "",
                    "editableTail": "",
                    "newAsrText": transcript,
                },
            )
            result = wait_for(process, "rewrite_tail_completed")
            output = result.get("replacementTail", "")
            if not output:
                raise RuntimeError(f"{name}: empty output")
            if "<think>" in output or "</think>" in output:
                raise RuntimeError(f"{name}: thinking content leaked into the output")
            elapsed = time.perf_counter() - started
            print(f"\n[{name}] {elapsed:.2f}s\n{output}")
    finally:
        if process.poll() is None:
            try:
                send(process, {"v": 1, "type": "shutdown", "id": "smoke-shutdown"})
                process.wait(timeout=2)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                process.kill()
                process.wait()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
