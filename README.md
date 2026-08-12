# DictScribe

DictScribe is a local desktop voice keyboard for Windows and Linux/X11. It
streams microphone audio through NVIDIA Nemotron ASR, incrementally cleans the
live transcript with a small local language model, and inserts the result into
the application that was active when dictation started.

The project is in its architecture/bootstrap phase and includes a Linux/X11
Skia test UI for exercising the complete local dictation flow. No cloud
inference, telemetry, transcript history, or local HTTP service is used.

## Native architecture

Two inference workers are kept in separate processes because NeMo-Speech.cpp
and llama.cpp use independently versioned GGML runtimes:

```text
dictscribe (Linux/X11 Skia test UI and controller)
  |-- JSONL pipes --> dictscribe-asr-worker
  |                   `-- NeMo-Speech.cpp + its GGML
  `-- JSONL pipes --> dictscribe-rewrite-worker
                      `-- llama.cpp + its GGML
```

The ASR worker owns the microphone, so audio never crosses the process boundary.
Only commands, status events, and transcript text use JSONL pipes.

## Prerequisites

- CMake 3.26 or newer
- Ninja (recommended)
- a C++20 compiler
- Git
- Linux audio development libraries supported by miniaudio
- Linux/X11 development libraries and a compatible local Skia checkout for the
  test UI
- optional: a compatible CUDA toolkit for `--cuda`

The Linux build automatically compiles the SentencePiece revision selected by
the current NeMo-Speech.cpp checkout into `build/nemo-dependencies`. Windows
developers currently need a compatible static SentencePiece package available
to CMake. The NeMo documentation recommends vcpkg; set `VCPKG_ROOT`, install
`sentencepiece:x64-windows-static-md`, and run `scripts/build.ps1`.

Clone recursively:

```bash
git clone --recurse-submodules git@github.com:Danmoreng/DictScribe.git
cd DictScribe
```

For an existing checkout:

```bash
git submodule update --init
git -C third_party/NeMo-Speech.cpp submodule update --init ggml
```

## Build

```bash
./scripts/build.sh
./scripts/test.sh
```

The default Linux build includes the Skia test UI. It uses the checkout named
by `DICTSCRIBE_SKIA_DIR` when that environment variable is set; otherwise it
reuses the Skia checkout from the neighboring `simple-markdown-viewer`
repository. To build only the workers and protocol tests, pass `--skip-ui`:

```bash
./scripts/build.sh --skip-ui
```

Build products are placed under `build/`:

```text
build/asr-worker/dictscribe-asr-worker
build/rewrite-worker/bin/dictscribe-rewrite-worker
build/core/dictscribe-protocol-tests
build/core/dictscribe
```

The NeMo SDK is built independently under `build/nemo-sdk` and installed into
`build/nemo-install`. This separation is intentional and must not be collapsed
into the llama.cpp CMake build.

## Live dictation test UI

Start the UI after a normal build:

```bash
./scripts/run-ui.sh
```

DictScribe uses `Qwen3.5-2B-Q8_0.gguf` as its fixed rewrite model. It discovers
that exact file in the Hugging Face Hub cache and never falls back to a larger
or unrelated GGUF. The cache follows the standard Hugging Face environment
variables: `HF_HUB_CACHE`, then `HF_HOME/hub`, then
`XDG_CACHE_HOME/huggingface/hub`, and finally `~/.cache/huggingface/hub`.

Download the selected quantization into the standard cache with:

```bash
hf download unsloth/Qwen3.5-2B-GGUF Qwen3.5-2B-Q8_0.gguf
```

For a manually downloaded copy, placing the file directly at
`~/.cache/huggingface/hub/Qwen3.5-2B-Q8_0.gguf` is also supported. The
`DICTSCRIBE_REWRITE_MODEL` environment variable and `--rewrite-model` remain
development overrides; there is intentionally no model selector in the UI.

Press Enter or Space to start and stop dictation. The upper panel shows the
cumulative raw Nemotron transcript. The lower panel updates with debounced live
llama.cpp cleanup while recording continues. Escape cancels an active
dictation; when idle, Escape closes the window.

The language button above the transcript cycles through `Auto`, `Deutsch`, and
`English` (keyboard shortcut: `L`). The selected language is sent to both ASR
and rewrite processing. `Auto` lets the models infer it from the utterance.
Explicit German or English is safer for short dictations containing many
foreign-language technical terms. An initial value can also be supplied with
`--language auto|de|en` or `DICTSCRIBE_LANGUAGE`.

`Final pass: On/Off` controls whether stopping dictation launches one additional
cleanup over the complete final ASR transcript. With it disabled, the UI keeps
the most recent live cleanup (or the raw final transcript when no live result
exists). Keyboard shortcut: `F`.

Live requests are bounded to one in-flight rewrite. New ASR partials are
coalesced for roughly 700 ms, with a two-second maximum wait, so continuous
speech still produces updates without building an unbounded LLM queue. On CPU,
the cleaned panel can visibly trail the raw panel by the current model latency;
the fixed 2B rewrite model is selected to keep that latency suitable for live
use. Qwen3.5 is forced into its non-thinking mode for this mechanical cleanup
task and uses Qwen's recommended non-thinking sampling parameters:
`temperature=0.7`, `top_p=0.8`, `top_k=20`, and `presence_penalty=1.5`. The CPU
worker uses at most eight threads and abandons a request after 15
seconds in total, including a possible language-guard retry. This lets the
controller retain the raw transcript instead of letting a pathological request
monopolize the machine.

## Worker protocol

Both workers use protocol version 1. One JSON object is written per line.
Standard output is reserved for protocol messages and logs belong on standard
error. See [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## Development plan

The full product roadmap is in
[`VOICE_KEYBOARD_DEVELOPMENT_PLAN.md`](VOICE_KEYBOARD_DEVELOPMENT_PLAN.md).
Implementation findings and current dependency pins are recorded in
[`docs/IMPLEMENTATION_NOTES.md`](docs/IMPLEMENTATION_NOTES.md).
