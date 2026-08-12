#!/usr/bin/env python3
"""Load both isolated workers concurrently and exercise their JSONL contracts."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Any, TextIO


ROOT = Path(__file__).resolve().parent.parent


def read_until(stream: TextIO, expected_type: str) -> dict[str, Any]:
    while True:
        line = stream.readline()
        if not line:
            raise RuntimeError(f"worker exited before emitting {expected_type}")
        message = json.loads(line)
        print(json.dumps(message, ensure_ascii=False))
        if message.get("type") == "error":
            raise RuntimeError(message.get("message", "worker error"))
        if message.get("type") == expected_type:
            return message


def send(process: subprocess.Popen[str], message: dict[str, Any]) -> None:
    assert process.stdin is not None
    process.stdin.write(json.dumps(message, ensure_ascii=False) + "\n")
    process.stdin.flush()


def start(command: list[str], library_path: Path | None = None) -> subprocess.Popen[str]:
    environment = os.environ.copy()
    if library_path is not None and sys.platform.startswith("linux"):
        current = environment.get("LD_LIBRARY_PATH", "")
        environment["LD_LIBRARY_PATH"] = str(library_path) + (":" + current if current else "")
    return subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        encoding="utf-8",
        env=environment,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--asr-model", type=Path, required=True)
    parser.add_argument("--rewrite-model", type=Path, required=True)
    arguments = parser.parse_args()

    asr_binary = ROOT / "build/asr-worker/dictscribe-asr-worker"
    rewrite_binary = ROOT / "build/rewrite-worker/bin/dictscribe-rewrite-worker"
    nemo_lib = ROOT / "build/nemo-install/lib"

    asr = start(
        [
            str(asr_binary),
            "--stdio",
            "--model",
            str(arguments.asr_model),
            "--protocol-version",
            "1",
        ],
        nemo_lib,
    )
    rewrite: subprocess.Popen[str] | None = None
    try:
        assert asr.stdout is not None
        read_until(asr.stdout, "ready")

        rewrite = start(
            [
                str(rewrite_binary),
                "--stdio",
                "--model",
                str(arguments.rewrite_model),
                "--protocol-version",
                "1",
            ]
        )
        assert rewrite.stdout is not None
        read_until(rewrite.stdout, "ready")

        send(asr, {"v": 1, "type": "ping", "id": "asr-ping"})
        read_until(asr.stdout, "pong")

        send(
            rewrite,
            {
                "v": 1,
                "type": "rewrite",
                "id": "rewrite-command",
                "requestId": "rewrite-1",
                "language": "de",
                "text": "Ich will äh den Server ändern, nee das Modell ändern.",
            },
        )
        result = read_until(rewrite.stdout, "rewrite_completed")
        if not result.get("text"):
            raise RuntimeError("rewrite worker returned no text")
        if "Ich" not in result["text"]:
            raise RuntimeError("rewrite worker did not preserve the German source language")

        send(
            rewrite,
            {
                "v": 1,
                "type": "rewrite",
                "id": "rewrite-language-guard-command",
                "requestId": "rewrite-language-guard-1",
                "language": "de",
                "text": (
                    "Ein neuer Versuch, was will ich eigentlich ausprobieren? Also ich möchte vor allen Dingen "
                    "testen, wie gut die Transkription grundsätzlich ist und auch wie gut die Reviewqualität ist. "
                    "Ja, also wie gut kann die L L M den Text korrigieren und umschreiben, dass das, was ich wirklich "
                    "gemeint habe, auch am Ende bei rauskommt. Interessant wäre zum Beispiel auch, wenn ich jetzt eine "
                    "Liste eine Aufzählung machen möchte: Erstens Butter, zweitens Milch, drittens Kuchen, viertens Brot."
                ),
            },
        )
        guarded = read_until(rewrite.stdout, "rewrite_completed")
        lowered = guarded.get("text", "").lower()
        if not guarded.get("text") or any(
            marker in lowered
            for marker in ("a new attempt", "i would", "the transcription", "first, butter")
        ):
            raise RuntimeError("rewrite worker returned an English translation of German input")

        send(
            rewrite,
            {
                "v": 1,
                "type": "rewrite",
                "id": "rewrite-technical-terms-command",
                "requestId": "rewrite-technical-terms-1",
                "language": "de",
                "text": (
                    "Ich möchte jetzt llama_rewriter.cpp und language_guard.cpp testen, weil die beiden Dateien "
                    "für den Rewrite wichtig sind und englische technische Begriffe trotzdem erhalten bleiben sollen."
                ),
            },
        )
        technical = read_until(rewrite.stdout, "rewrite_completed")
        technical_text = technical.get("text", "")
        if "llama_rewriter.cpp" not in technical_text or "language_guard.cpp" not in technical_text:
            raise RuntimeError("rewrite worker did not preserve mixed-language technical identifiers")
        if "Ich" not in technical_text:
            raise RuntimeError("rewrite worker did not preserve German around technical identifiers")

        send(asr, {"v": 1, "type": "shutdown", "id": "asr-shutdown"})
        read_until(asr.stdout, "shutdown_complete")
        send(rewrite, {"v": 1, "type": "shutdown", "id": "rewrite-shutdown"})
        read_until(rewrite.stdout, "shutdown_complete")

        if asr.wait(timeout=10) != 0 or rewrite.wait(timeout=10) != 0:
            raise RuntimeError("worker did not exit cleanly")
        print("Concurrent worker smoke test passed.")
        return 0
    finally:
        for process in (asr, rewrite):
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()


if __name__ == "__main__":
    raise SystemExit(main())
