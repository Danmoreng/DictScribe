# DictScribe

DictScribe is a local desktop voice keyboard for Windows and Linux/X11. It
streams microphone audio through NVIDIA Nemotron ASR, incrementally cleans the
live transcript with a small local language model, and inserts the result into
the application that was active when dictation started.

The project includes a native Windows tray application with a no-focus Skia
overlay and a Linux/X11 Skia development UI. No cloud inference, telemetry,
transcript history, or local HTTP service is used.

## Native architecture

Two inference workers are kept in separate processes because NeMo-Speech.cpp
and llama.cpp use independently versioned GGML runtimes:

```text
dictscribe (Win32/Skia tray overlay or Linux/X11 UI and controller)
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
- Windows: Visual Studio 2022 Build Tools with the C++ workload and vcpkg
- Linux: audio and X11 development libraries supported by miniaudio and GLFW
- a compatible local Skia checkout for the desktop UI
- optional: a compatible CUDA toolkit for `--cuda`

The Linux build automatically compiles the SentencePiece revision selected by
the current NeMo-Speech.cpp checkout into `build/nemo-dependencies`. The Windows
build uses the repository's vcpkg manifest to install a compatible static
SentencePiece package. `scripts/build.ps1` automatically detects vcpkg bundled
with Visual Studio Build Tools; set `VCPKG_ROOT` only when using a separate
vcpkg checkout.

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

On Windows, run the CPU build from a regular PowerShell prompt (the script
loads the Visual Studio compiler environment automatically):

```powershell
.\scripts\build.ps1
ctest --test-dir build\core --output-on-failure
```

The Windows build reuses the compiled Skia checkout from the neighboring
`simple-markdown-viewer` repository by default. Set `DICTSCRIBE_SKIA_DIR` and
`DICTSCRIBE_SKIA_OUT_DIR` to use another checkout, or pass `-SkipUi` when only
the workers and protocol tests are needed:

```powershell
.\scripts\build.ps1 -SkipUi
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

On Windows the executable names have an `.exe` suffix. Required NeMo runtime
DLLs are copied next to `dictscribe-asr-worker.exe` during the build.

The NeMo SDK is built independently under `build/nemo-sdk` and installed into
`build/nemo-install`. This separation is intentional and must not be collapsed
into the llama.cpp CMake build.

## Windows voice keyboard

Start the background application after a normal Windows build:

```powershell
.\build\core\dictscribe.exe
```

DictScribe adds a notification-area icon and loads both local models once. In
any text field, press `Ctrl+Alt+Space` to start dictation. A topmost,
non-activating overlay appears above the active text caret; if Windows does not
expose a caret rectangle, it appears near the mouse pointer instead. Press
`Ctrl+Alt+Space` or `Enter` to finish, or `Escape` to cancel. Completed text is
typed into the most recently active non-DictScribe window. This also covers a
recording started from the tray before a text field was selected: click the
destination field at any time before finalization completes. DictScribe never
uses one of its own windows as an insertion target. Direct Unicode input leaves
the existing clipboard untouched. If Windows blocks safe focus restoration or
no external target exists, the completed text is copied to the clipboard and a
notification explains that it can be inserted with `Ctrl+V`.

The overlay is visually opaque. Its header can be dragged without activating
the window; the transcript and footer remain click-through so the underlying
application stays usable. During recording, the header meter visualizes actual
microphone RMS/peak samples. The body shows one live ASR transcript while
recording and one cleaned result after finalization, never simultaneous raw and
rewritten copies.

Right-click the tray icon to start or stop dictation, select `Auto`, `Deutsch`,
or `English`, enable or disable the final cleanup pass, or quit DictScribe. The
default shortcut intentionally avoids PowerToys Run's `Alt+Space` binding.

To verify model discovery and both worker startups without activating the
microphone, run:

```powershell
$process = Start-Process .\build\core\dictscribe.exe -ArgumentList --smoke-test -PassThru -Wait
$process.ExitCode
```

## Models

DictScribe uses `Qwen3.5-2B-Q8_0.gguf` as its fixed rewrite model. It discovers
that exact file in the Hugging Face Hub cache and never falls back to a larger
or unrelated GGUF. The cache follows the standard Hugging Face environment
variables: `HF_HUB_CACHE`, then `HF_HOME/hub`, then
`XDG_CACHE_HOME/huggingface/hub`. The fallback is
`%USERPROFILE%\.cache\huggingface\hub` on Windows and
`~/.cache/huggingface/hub` elsewhere.

Download the selected quantization into the standard cache with:

```bash
hf download unsloth/Qwen3.5-2B-GGUF Qwen3.5-2B-Q8_0.gguf
```

For a manually downloaded copy, placing the file directly at the cache root as
`Qwen3.5-2B-Q8_0.gguf` is also supported. The `DICTSCRIBE_REWRITE_MODEL`
environment variable and `--rewrite-model` remain development overrides; there
is intentionally no model selector in the UI.

## Linux/X11 development UI

Start the UI after a normal build:

```bash
./scripts/run-ui.sh
```

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
