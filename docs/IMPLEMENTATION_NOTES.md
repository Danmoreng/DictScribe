# Implementation notes

## 2026-08-13: lazy cleanup lifecycle and bounded controller

Cleanup is now a persisted two-state setting. `Off` is the default and starts
only the ASR worker; the rewrite executable and model are optional in this mode.
Selecting `AI cleanup` starts Qwen on demand without blocking recording or ASR
readiness. Disabling cleanup terminates the rewrite worker, and model load,
generation, validation, timeout, or worker failures retain a fully usable raw
dictation path.

The controller no longer sends cumulative whole transcripts to the rewrite
worker. A model-independent `SemanticTranscript` owns frozen output, a bounded
editable tail, immutable stable ASR spans, and the unstable cumulative suffix.
It conservatively promotes only text confirmed across later hypotheses while
protecting the latest words. Requests use protocol version 2, bounded context
and new-ASR fields, stable-span IDs, session generation, and tail revision.
Append-only ASR updates do not invalidate an active request; changes to the
captured tail or spans do. One active request and one coalesced successor are
allowed. A detected revision inside already promoted ASR text disables cleanup
for that dictation and returns to the complete raw transcript.

Finalization and language changes preserve every raw ASR segment. Enter never
waits for Qwen, late results from completed or cancelled sessions are ignored,
and long inputs are split into bounded stable spans before dispatch.

## 2026-08-13: shared settings and independent worker devices

Windows and Linux now render the same Skia settings surface from their native
window hosts. Language and independent ASR/rewrite CPU or GPU choices use one
versioned settings format. A device change is accepted only while dictation is
idle and immediately restarts only the selected worker; the other inference
process remains loaded. Failed worker startup remains a recoverable settings
state, and a device choice is committed to disk only after the worker reports
ready, preventing a failing GPU backend from becoming a persistent startup
loop. The settings surface displays the discovered local
model filenames but intentionally does not allow arbitrary model selection.

## 2026-08-13: Linux/X11 overlay

The former large Linux test window is now a compact GLFW/Skia overlay matching
the Windows dictation surface. A separate X11 control connection owns global
hotkeys and clipboard selection requests while GLFW keeps ownership of the
rendering window. `Ctrl+Alt+Space` toggles dictation, Enter and Escape are
grabbed only during an active session, and `Ctrl+Alt+Q` exits because the first
Linux slice does not yet include a tray icon.

The overlay is undecorated, topmost, omitted from taskbar/pager lists, and
declares that it does not accept keyboard focus. It is placed near the pointer,
because X11 has no general cross-toolkit caret-position contract. The most
recent external active/focus windows are retained as insertion targets. Final
text is served through the local X11 clipboard and pasted with XTEST; when that
cannot be done safely, it remains available on the clipboard. Explicit line
breaks are preserved by the Linux wrapper in preparation for structured
semantic cleanup output.

## 2026-08-12: incremental cleanup direction (implemented in Phase 3)

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

The first UI milestone was a deliberately small Linux/X11 Skia test window. It
acted as the controller for both workers while preserving their process and
GGML isolation. It has since been replaced by the Linux/X11 overlay described
above.

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
- the Qwen3.5-0.8B Q8_0 rewrite model from the official ggml-org Hugging Face
  repository.

Both models loaded concurrently in their separate persistent workers. ASR
responded to its protocol ping, llama.cpp completed a real German rewrite, and
both workers acknowledged shutdown and exited normally. `ldd` confirms that
only the ASR worker loads NeMo's `libggml.so.0`; the statically linked rewrite
worker has no GGML shared-library dependency.

The original whole-transcript path used a language guard, retry, and
language-specific deterministic normalizer. The semantic incremental path no
longer calls those components. It uses a version-2 structured tail request,
grammar-constrained JSON output, greedy decoding, and raw ASR fallback for
every rewrite failure. Technical literals remain protected and restored.

Live cleanup is scheduled only from conservatively stabilized Nemotron spans.
Updates are coalesced for 700 ms but forced after at most two seconds of
continuous stable input. Only one llama.cpp request may be in flight; newer
stable spans form one coalesced successor instead of accumulating a queue.
Finalization never waits for a cleanup request, and all unprocessed final ASR
text is appended raw.
The initial 4B Q8_0 rewrite model could not keep up with live dictation on the
development CPU. Runtime discovery is now fixed to the benchmark candidate
`Qwen3.5-0.8B-Q8_0.gguf` in the standard Hugging Face Hub cache. No other GGUF is
selected as a fallback, and the UI deliberately has no model selector.
Qwen3.5's chat template is explicitly continued with its non-thinking assistant
prefix: reasoning traces are both unnecessary and far too expensive for live
cleanup. Generation is greedy with no sampling or presence penalties. CPU
inference defaults to four threads, a 2,048-token context, a dynamically capped
output, and a five-second deadline. Any reasoning prefix is stripped (or
rejected when incomplete) before a result can reach the UI.
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
