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
- Linux/X11: audio, GLFW, and XTEST development libraries
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

The overlay uses a subtle Windows acrylic backdrop with a dark translucent Skia
surface. Systems without backdrop support retain the same dark opaque fallback.
Its header can be dragged without activating the window. After the user moves it, DictScribe stores that screen position and
reuses it for later dictations and application launches. The position is clamped
back onto a visible monitor when the display layout changes. Foreground tracking
changes only the eventual insertion target. The compact transcript type allows the window to grow with its content
up to a bounded height. Longer text can be navigated with the mouse wheel or
the draggable scrollbar without activating the overlay. During recording, the
header shows the selected language and visualizes actual microphone RMS/peak
samples. The language badge opens a menu, and the Settings button opens the
cross-platform settings window. Changing the language during dictation finalizes
the current ASR segment, preserves its text, and immediately starts a new audio
session with the selected language. The body shows one live ASR transcript while recording and one cleaned
result after finalization, never simultaneous raw and rewritten copies. The
footer presents the relevant `Enter`, `Escape`, and `Ctrl+Alt+Space` shortcuts.

Right-click the tray icon to start or stop dictation, open Settings, select
`Auto`, `Deutsch`, or `English`, or quit DictScribe. The
default shortcut intentionally avoids PowerToys Run's `Alt+Space` binding.

Language, overlay position, and the independent CPU/GPU choice for the ASR and
rewrite workers are stored in `%LOCALAPPDATA%\DictScribe\settings.json`.
Changing a device immediately restarts only the affected local worker. Device
controls are disabled during an active dictation so a recording cannot be
discarded by a restart, but remain available after a worker failure for
recovery. A new device is persisted only after that worker reports a successful
startup; a failed GPU attempt therefore retains the previously working device
for the next application launch. Explicit `--language`, `--asr-device`,
`--rewrite-device`, and `--gpu` arguments override the corresponding stored
value for that process.

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

## Linux/X11 voice keyboard

Start the background process after a normal build:

```bash
./scripts/run-ui.sh
```

DictScribe remains hidden until `Ctrl+Alt+Space` starts dictation. It then shows
a compact, always-on-top overlay near the mouse pointer without taking keyboard
focus from the active application. Press `Ctrl+Alt+Space` or `Enter` to finish
and insert, or `Escape` to cancel. `Ctrl+Alt+Q` exits the background process.
The overlay can be dragged, scrolled, and used to select `Auto`, `Deutsch`, or
`English` while recording continues. Its Settings button opens the same settings
surface as Windows. ASR and rewrite can independently use CPU or GPU; changing
one restarts only that worker. Settings are stored in
`$XDG_CONFIG_HOME/dictscribe/settings.json`, or
`~/.config/dictscribe/settings.json` when that variable is unset.

The X11 host remembers the most recent external focus target during dictation.
On completion it owns the X11 clipboard and sends a local `Ctrl+V` through the
XTEST extension to that target. If focus restoration or synthetic input is not
available, the text remains on the clipboard for manual insertion. This first
Linux overlay targets native X11 sessions; a Wayland-native global-shortcut and
insertion implementation is a separate platform milestone. An initial language
can also be supplied with `--language auto|de|en` or `DICTSCRIBE_LANGUAGE`.

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
