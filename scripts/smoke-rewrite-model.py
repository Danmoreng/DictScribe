#!/usr/bin/env python3
"""Run the scored semantic rewrite model quality gate."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path


DEFAULT_CORPUS = (
    Path(__file__).resolve().parent.parent
    / "tests"
    / "data"
    / "rewrite_tail_benchmark.json"
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


def normalized(text: str) -> str:
    return " ".join(text.casefold().split())


def evaluate_check(output: str, check: dict) -> str | None:
    check_type = check["type"]
    comparable = normalized(output)
    if check_type == "contains":
        value = normalized(check["value"])
        return None if value in comparable else f"missing required text: {check['value']}"
    if check_type == "containsAny":
        values = check["values"]
        return None if any(normalized(value) in comparable for value in values) else (
            "missing every accepted alternative: " + ", ".join(values)
        )
    if check_type == "notContains":
        value = normalized(check["value"])
        return None if value not in comparable else f"contains forbidden text: {check['value']}"
    if check_type == "equalsNormalized":
        return None if comparable == normalized(check["value"]) else "normalized output changed"
    if check_type == "paragraphBreak":
        return None if re.search(r"\n\s*\n", output) else "missing paragraph break"
    if check_type == "regex":
        return None if re.search(check["value"], output, re.IGNORECASE) else (
            f"did not match regex: {check['value']}"
        )
    if check_type == "unorderedList":
        lines = [
            normalized(match.group(1))
            for line in output.splitlines()
            if (match := re.match(r"^\s*-\s+(.+?)\s*$", line))
        ]
        expected = [normalized(item) for item in check["items"]]
        if len(lines) < len(expected):
            return f"expected at least {len(expected)} unordered list items, got {len(lines)}"
        for item in expected:
            if not any(item in line for line in lines):
                return f"missing unordered list item: {item}"
        return None
    if check_type == "orderedList":
        lines = [
            normalized(match.group(1))
            for line in output.splitlines()
            if (match := re.match(r"^\s*\d+[.)]\s+(.+?)\s*$", line))
        ]
        expected = [normalized(item) for item in check["items"]]
        if len(lines) < len(expected):
            return f"expected at least {len(expected)} ordered list items, got {len(lines)}"
        for index, item in enumerate(expected):
            if item not in lines[index]:
                return f"ordered list item {index + 1} does not contain: {item}"
        return None
    raise ValueError(f"unknown benchmark check type: {check_type}")


def evaluate_case(output: str, case: dict) -> list[str]:
    return [
        failure
        for check in case["checks"]
        if (failure := evaluate_check(output, check)) is not None
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--worker", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS)
    parser.add_argument("--json-report", type=Path)
    args = parser.parse_args()
    if not args.worker.is_file():
        parser.error(f"worker not found: {args.worker}")
    if not args.model.is_file():
        parser.error(f"model not found: {args.model}")
    if not args.corpus.is_file():
        parser.error(f"benchmark corpus not found: {args.corpus}")

    corpus = json.loads(args.corpus.read_text(encoding="utf-8"))
    if corpus.get("schemaVersion") != 1 or not isinstance(corpus.get("cases"), list):
        parser.error("unsupported benchmark corpus schema")

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

        results = []
        for number, case in enumerate(corpus["cases"], start=1):
            request_id = f"smoke-{number}"
            started = time.perf_counter()
            output = ""
            failures = []
            try:
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
                        "languageHint": case["languageHint"],
                        "readOnlyContext": case["readOnlyContext"],
                        "editableTail": case["editableTail"],
                        "newAsrText": case["newAsrText"],
                    },
                )
                result = wait_for(process, "rewrite_tail_completed")
                output = result.get("replacementTail", "")
                if not output:
                    failures.append("worker returned an empty tail")
                else:
                    failures.extend(evaluate_case(output, case))
            except RuntimeError as exception:
                failures.append(str(exception))
            elapsed = time.perf_counter() - started
            status = "PASS" if not failures else "FAIL"
            print(f"\n[{status}] {case['name']} ({elapsed:.2f}s)\n{output or '<no output>'}")
            for failure in failures:
                print(f"  - {failure}")
            results.append(
                {
                    "id": case["id"],
                    "name": case["name"],
                    "passed": not failures,
                    "elapsedSeconds": round(elapsed, 3),
                    "output": output,
                    "failures": failures,
                }
            )

        passed = sum(1 for result in results if result["passed"])
        report = {
            "model": str(args.model.resolve()),
            "architecture": architecture,
            "chatTemplateAvailable": has_template,
            "passed": passed,
            "total": len(results),
            "cases": results,
        }
        print(f"\nQuality gate: {passed}/{len(results)} cases passed")
        if args.json_report:
            args.json_report.parent.mkdir(parents=True, exist_ok=True)
            args.json_report.write_text(
                json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
        return 0 if passed == len(results) else 1
    finally:
        if process.poll() is None:
            try:
                send(process, {"v": 1, "type": "shutdown", "id": "smoke-shutdown"})
                process.wait(timeout=2)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                process.kill()
                process.wait()
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
