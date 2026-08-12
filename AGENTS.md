# DictScribe Repository Guide

## Product boundaries

DictScribe is a small, fully local system-wide voice keyboard. Keep the product
focused on microphone capture, streaming transcription, constrained local text
cleanup, a compact overlay, and insertion into the previously active text
field. Do not add cloud inference, accounts, telemetry, transcript history,
provider marketplaces, HTTP servers, or plugin systems unless explicitly
requested.

## Architecture

- `dictscribe`: future UI/controller process; it must not link either inference
  runtime.
- `dictscribe-asr-worker`: persistent NeMo-Speech.cpp process that owns audio
  capture and its private GGML runtime.
- `dictscribe-rewrite-worker`: persistent llama.cpp process with a separate
  GGML runtime.
- Workers communicate through versioned JSONL on stdin/stdout. Logs go to
  stderr. Do not introduce localhost HTTP for internal communication.
- NeMo-Speech.cpp is built and installed as a standalone SDK before the ASR
  worker is configured. llama.cpp is compiled in a separate CMake build tree.

## Working rules

- Check `git status` before editing and preserve user-owned changes.
- Do not edit files inside `third_party/`. Update dependency pins through Git
  submodule commits instead.
- Keep repository-facing code, comments, documentation, and product strings in
  English.
- Prefer small interfaces and targeted tests over framework abstractions.
- Models, build output, logs, databases, and downloaded runtime files are not
  source artifacts.

## Build and test

Linux CPU development build:

```bash
./scripts/build.sh
./scripts/test.sh
```

Optional CUDA build:

```bash
./scripts/build.sh --cuda
```

The CUDA path applies NeMo's own pinned GGML patch series. CPU builds use the
stock pinned NeMo GGML with patched-only code paths disabled.
