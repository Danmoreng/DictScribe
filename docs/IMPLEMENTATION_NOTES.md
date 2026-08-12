# Implementation notes

## 2026-08-12: incremental cleanup direction

The current cumulative whole-transcript live rewrite is a prototype, not the
intended production architecture. The agreed replacement uses a frozen prefix,
a bounded read-only context suffix, a small editable cleaned tail, and only the
new stable ASR span. The model will return a grammar-constrained JSON object
containing only the replacement tail. Stop/insertion will not wait for cleanup,
and the target design has no final full-transcript pass.

The full model contract, controller algorithm, fallback rules, GPU/model
benchmark plan, implementation order, and acceptance criteria are documented
in [Incremental cleanup design](INCREMENTAL_CLEANUP_DESIGN.md).

## 2026-08-12: initial dependency and process architecture

The initial bootstrap uses the newest upstream states available on this date:

| Dependency | Pin | Selection rule |
|---|---|---|
| NeMo-Speech.cpp | `9bc876635af36df537d9bc6d3f57ad1b76e4f74a` | current `main`; upstream has no releases |
| llama.cpp | `9558fa44c92746a58dd07ad1bf0c889715b938a6` | current `master`; upstream has no stable release line |
| miniaudio | `0.11.25` (`9634bedb...`) | latest release |
| nlohmann/json | `v3.12.0` (`55f93686...`) | latest release |
| SentencePiece | `17d7580d6407802f85855d2cc9190634e2c95624` | compatibility pin selected by current NeMo |

Pins make builds reproducible; updating to a newer upstream remains an explicit,
tested repository change.

NeMo still pins GGML `c03b4e2b...` while the independent current llama.cpp has a
much newer GGML tree. Both produce similarly named GGML libraries. DictScribe
therefore adopts process isolation immediately.

NeMo is built as a standalone top-level CMake project and installed as an SDK.
This uses the upstream-supported `find_package(NeMoSpeech COMPONENTS ASR)`
contract and avoids carrying the older embedding patch from Talk-to-Pi. CPU
builds set `NEMO_SPEECH_GGML_PATCHED=OFF`; optional CUDA builds apply NeMo's own
current pinned GGML patch series before configuration.

The current NeMo-selected SentencePiece commit predates GCC 16's stricter
transitive-include behavior. DictScribe supplies `-include cstdint` only to that
dependency build instead of modifying vendored source.

The first UI milestone is now a deliberately small Linux/X11 Skia test window.
It acts as the controller for both workers while preserving their process and
GGML isolation. Enter or Space starts and stops dictation, and Escape cancels
an active session or closes the idle window. Live ASR output and the finalized
llama.cpp rewrite are displayed in separate regions so streaming behavior and
the rewrite transition can be inspected directly. Tray integration, global
hotkeys, text insertion, and the production overlay remain later milestones.

`scripts/build.sh` builds this UI by default. `--skip-ui` retains the previous
worker-only build for environments without X11 or Skia development files, and
`scripts/run-ui.sh` launches the test window. Until DictScribe owns a dedicated
Skia dependency, the build resolves Skia from `DICTSCRIBE_SKIA_DIR` first and
otherwise reuses the checkout in the neighboring `simple-markdown-viewer`
repository. Only its thin platform and Skia surface approach is reused; its
document system is not part of DictScribe.

## Validation

The Linux CPU build was validated with GCC 16.1.1 and CMake against:

- the pinned 741,548,352-byte Nemotron 3.5 ASR Streaming Q8_0 model already in
  the Hugging Face cache;
- the Qwen3.5-2B Q8_0 rewrite model from the Hugging Face Hub cache.

Both models loaded concurrently in their separate persistent workers. ASR
responded to its protocol ping, llama.cpp completed a real German rewrite, and
both workers acknowledged shutdown and exited normally. `ldd` confirms that
only the ASR worker loads NeMo's `libggml.so.0`; the statically linked rewrite
worker has no GGML shared-library dependency.

The smoke test exposed and then guarded against an initial translation failure;
the rewrite request now carries its required output language, the prompt repeats
that constraint, and a lightweight language guard rejects clear language
changes. A rejected translation is retried once with a stricter instruction;
if it still fails, the UI displays the original transcript instead of unsafe
rewritten text. The regression suite includes two consecutive German requests,
including the longer list example that originally triggered an English result.
The UI now exposes `Auto`, German, and English rather than hard-coding German;
the selection controls both NeMo's recognition prompt and the required rewrite
language. The guard evaluates the surrounding natural language, not isolated
identifiers, and the model regression test verifies that `llama_rewriter.cpp`
and `language_guard.cpp` survive inside German prose unchanged.

Live cleanup is scheduled in the controller from cumulative Nemotron partials.
Updates are coalesced for 700 ms but forced after at most two seconds of
continuous changes. Only one llama.cpp request may be in flight; newer partials
replace the single pending snapshot instead of accumulating a queue. Completed
live rewrites update the lower debugging panel without leaving the recording
state. A UI switch controls whether the final ASR result receives an additional
full cleanup pass, making live-only and live-plus-final behavior directly
comparable.
The initial 4B Q8_0 rewrite model could not keep up with live dictation on the
development CPU. Runtime discovery is therefore fixed to
`Qwen3.5-2B-Q8_0.gguf` in the standard Hugging Face Hub cache. No other GGUF is
selected as a fallback, and the UI deliberately has no model selector.
Qwen3.5's chat template is explicitly continued with its non-thinking assistant
prefix: reasoning traces are both unnecessary and far too expensive for live
cleanup. Greedy decoding was replaced with Qwen's recommended non-thinking
sampling configuration: temperature 0.7, top-p 0.8, top-k 20, and presence
penalty 1.5. Sampling uses a fixed seed so identical cleanup requests are
reproducible. Before inference, deterministic normalization resolves explicit
correction markers, spoken paragraph and line breaks, colons, numbered list
items, and dictated path symbols. Technical literals are replaced with opaque
placeholders for inference and restored afterwards; the model may omit a
superseded literal but cannot silently rewrite or duplicate it. As a runtime
safety bound, CPU inference uses at most eight threads and
each complete rewrite request has a 15-second deadline. Any reasoning prefix is
stripped (or rejected when incomplete) before a result can reach the UI.
Rewrite quality and self-correction fidelity are not considered selected or
production-ready until the planned model benchmark is complete.

The first recorded-speech benchmark uses three German PCM16 mono recordings:
fillers and self-correction, spoken list formatting, and technical identifiers
and paths. The app-equivalent 1,600-sample streaming path produced useful prose
for the first two recordings but lost or phonetically damaged several technical
tokens in the third. Cleanup now resolves the explicit correction and formats
the list reproducibly without inventing replacement path segments. It cannot
recover information absent from the ASR result; damaged names such as a
misrecognized project name remain visible rather than being guessed.

Microphone activation was intentionally not performed during the automated
bootstrap verification because it records from the user's default input
device. The Linux/X11 test UI and `scripts/run-ui.sh` now provide the intended
manual integration path for live audio capture, streaming transcript display,
and final rewrite inspection.
