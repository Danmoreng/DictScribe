# VoxType vs. DictScribe: Architecture, ASR, and Implementation Analysis

**Purpose:** Technical comparison to guide the next stages of DictScribe development  
**Analysis date:** August 13, 2026  
**VoxType snapshot:** commit `a3c092b` (`Merge pull request #521 from OldJobobo/fix/whisper-degenerate-retry`, August 6, 2026)  
**DictScribe snapshot:** uploaded repository state including `INCREMENTAL_CLEANUP_DESIGN.md` and the supplementary `SEMANTIC_INCREMENTAL_CLEANUP_PLAN.md`

---

## 1. Executive Summary

VoxType is not simply “DictScribe with a different ASR model.” The most important difference is the **product and backend architecture**:

- **DictScribe** is currently a deliberately small, tightly focused live-dictation pipeline built around a persistent Nemotron ASR worker and a separate llama.cpp rewrite worker.
- **VoxType** is a broader Linux dictation framework with multiple interchangeable ASR backends, many text-output methods, configurable profiles, optional batch post-processing, VAD, lazy loading, and extensive Linux integration.

The ASR option most prominently advertised by VoxType is currently **Cohere Transcribe 2B**. However, the source code shows several important details:

1. The default configuration still uses **Whisper `base.en`**.
2. VoxType's Cohere backend is implemented as a **batch transcriber**, not as a native continuous streaming pipeline.
3. Cohere Transcribe is a 2B model, expects a selected language, does not provide built-in automatic language detection, is described as inconsistent for code-switching, and benefits from an upstream VAD/noise gate.
4. For DictScribe's core requirements—low live latency, cumulative partial hypotheses, and multilingual automatic recognition—the current **Nemotron 3.5 ASR Streaming 0.6B is architecturally a better fit**.

The primary ASR recommendation is therefore:

> **Do not replace Nemotron yet.** The uploaded VoxType snapshot does not contain an ASR path that is clearly better suited to DictScribe's live use case. Cohere Transcribe may be useful later as an optional batch-quality benchmark, but it should not replace the default pipeline without measured evidence.

The most valuable ideas to take from VoxType are elsewhere:

1. **Backend and capability abstraction** instead of hard-wiring assumptions about one model.
2. A **model registry with metadata** such as file list, size, supported languages, streaming capability, and license.
3. **Lazy loading and `prepare()`/prewarming**, allowing model load time to overlap with recording.
4. **Language-independent VAD** to reduce silence hallucinations and improve segmentation/pause detection.
5. **Explicit streaming event types** for partial, final, and revised output.
6. **Robust output abstraction and fallback chains**, especially for multiline lists and paragraphs.
7. **Session/cleanup profiles** so users can choose different semantic cleanup goals without language-specific rule sets.
8. **Broader scenario and failure-mode testing**.

The following VoxType ideas should **not** be copied into DictScribe's default architecture:

- hard-coded English spoken-punctuation and filler-word rules,
- arbitrary shell/LLM post-processing as the primary cleanup mechanism,
- directly typing unstable partial hypotheses into another application and correcting them with backspaces,
- automatically carrying the previous dictation into the next prompt as context,
- or adopting VoxType's entire compile-time backend matrix before DictScribe has a real need for multiple production ASR backends.

DictScribe's planned **bounded semantic incremental cleanup** is conceptually safer and more suitable for live cleanup than VoxType's current post-processing approach. It should remain the main cleanup direction, while selected infrastructure ideas from VoxType should be incorporated around it.

---

## 2. Scope of the Comparison

### 2.1 DictScribe files reviewed

The analysis focuses in particular on:

- `README.md`
- `docs/ARCHITECTURE.md`
- `docs/INCREMENTAL_CLEANUP_DESIGN.md`
- `docs/SEMANTIC_INCREMENTAL_CLEANUP_PLAN.md`
- `src/app/app_controller.cpp`
- `src/app/model_discovery.cpp`
- `src/workers/asr/transcription_engine.cpp`
- `src/workers/asr/runtime_controller.cpp`
- `src/workers/rewrite/*`
- `src/platform/win/win_text_injector.cpp`
- `src/platform/win/win_main.cpp`

### 2.2 VoxType files reviewed

The analysis focuses in particular on:

- `README.md`
- `config/default.toml`
- `Cargo.toml`
- `src/transcribe/mod.rs`
- `src/transcribe/streaming.rs`
- `src/transcribe/cohere.rs`
- `src/transcribe/parakeet.rs`
- `src/transcribe/parakeet_streaming.rs`
- `src/transcribe/worker.rs`
- `src/model_manager.rs`
- `src/setup/model.rs`
- `src/vad/*`
- `src/text/mod.rs`
- `src/output/post_process.rs`
- `src/output/streaming.rs`
- the output backends under `src/output/`
- `src/daemon.rs`
- the engine/model/integration documentation

### 2.3 Important note about source consistency

VoxType's README, older documentation, comments, configuration, and current implementation do not always describe exactly the same state. Examples:

- The README prominently highlights Cohere Transcribe as a current high-performance backend.
- `config/default.toml` still defaults to Whisper `base.en`.
- A comment in `src/transcribe/mod.rs` still describes Cohere support as a partially wired proof of concept, while the factory can now instantiate it.
- Some older Parakeet documentation mentions English-only behavior, whereas the general Parakeet v3 batch model supports 25 languages. The special streaming model bundled by VoxType is a different English-only export.

For this analysis, evidence is prioritized in this order:

1. executable/current source code,
2. current model cards from model providers,
3. current configuration,
4. README and older project documentation.

---

## 3. High-Level Comparison

| Area | DictScribe | VoxType | Relevance for DictScribe |
|---|---|---|---|
| Primary product goal | Live overlay and final text insertion on Windows and Linux/X11 | Broad Linux dictation utility with many engines and integrations | DictScribe's narrower scope is beneficial for reliable live dictation |
| Default ASR | Nemotron 3.5 ASR Streaming 0.6B | Config still defaults to Whisper `base.en`; README highlights Cohere 2B | No immediate reason to switch |
| ASR architecture | Native cache-aware streaming with cumulative hypotheses | Mix of batch, eager chunking, and selected streaming backends | Adopt capability abstraction, not necessarily another model |
| Model selection | Fixed model paths and roles | Large engine factory and model catalog | Adopt a model registry; keep the runtime factory small initially |
| Process model | Controller plus two separate persistent workers | Central daemon; some backends in-process, some worker/subprocess based | DictScribe's process isolation is cleaner for incompatible GGML runtimes |
| Cleanup | Current whole-transcript rewrite is weak; bounded semantic tail is planned | English regex rules plus optional batch shell/LLM postprocessor | DictScribe's new plan is stronger; borrow profiles and fail-open behavior |
| Streaming cleanup | Planned with bounded tail and revisions | Post-processing is intentionally disabled during streaming | DictScribe is conceptually ahead here |
| VAD | No dedicated abstract VAD path | Energy and model-based VAD abstraction | Worth adopting in a language-neutral form |
| Text output | Windows Unicode injection + clipboard fallback; Linux/X11 UI | Many Wayland/X11 backends, clipboard, file output, fallbacks, Shift+Enter newlines | Strong source of ideas for lists and paragraphs |
| Model lifecycle | ASR and rewrite currently loaded at startup | Preload, on-demand, prepare, idle eviction, GPU isolation | Especially useful for rewrite worker |
| Tests | Smaller and focused | Extensive unit/integration coverage | Adopt scenario-driven testing, not all complexity |
| Application license | Apache 2.0 | MIT | Both open; model licenses must be handled separately |

---

## 4. Architecture Comparison

### 4.1 DictScribe: explicit process boundaries

DictScribe deliberately separates controller, ASR, and rewrite into three processes:

```text
Desktop controller
  ├── JSONL → ASR worker
  │            └── NeMo-Speech.cpp + its own GGML revision
  └── JSONL → rewrite worker
               └── llama.cpp + its own GGML revision
```

This is not unnecessary microservice complexity. It solves a concrete technical problem: NeMo-Speech.cpp and llama.cpp use independently versioned GGML runtimes that can have colliding library names and symbols.

The isolation also provides:

- separate model lifecycles,
- separate CPU/GPU assignment,
- independent failure handling,
- no local HTTP service,
- a clear IPC boundary.

This is still a good DictScribe design decision. Nothing in VoxType suggests that these boundaries should be removed.

Relevant DictScribe sources:

- `README.md:12-26`
- `docs/ARCHITECTURE.md:1-39`

### 4.2 VoxType: broad daemon with interchangeable backends

VoxType defines a generic `Transcriber` trait:

```rust
pub trait Transcriber: Send + Sync {
    fn transcribe(&self, samples: &[f32]) -> Result<String, TranscribeError>;
    fn transcribe_timed(&self, samples: &[f32]) -> Result<Vec<TimedSegment>, TranscribeError>;
    fn prepare(&self);
    fn as_streaming(&self) -> Option<&dyn StreamingTranscriber>;
    fn last_detected_language(&self) -> Option<String>;
}
```

Depending on Cargo features and configuration, its factory can instantiate backends including:

- Whisper
- Parakeet
- Moonshine
- SenseVoice
- Paraformer
- Dolphin
- Omnilingual
- Cohere
- Soniox

Relevant source:

- `src/transcribe/mod.rs:93-287`

This abstraction is one of VoxType's strongest architectural ideas because it prevents the application from being fundamentally tied to one model vendor.

However, VoxType is not a perfectly unified backend platform either:

- `ModelManager` remains heavily Whisper-oriented.
- Some backends are retained as `Arc<dyn Transcriber>` objects while others are created per recording or in subprocesses.
- Capabilities are only partly expressed through the common interface.
- Model management, the model catalog, and backend construction are not fully unified under one abstraction.
- Some log messages still mention Parakeet even when the same code path now supports other engines.

### Recommendation

DictScribe should adopt **the concept**, not VoxType's complete implementation.

A small capability-aware abstraction is sufficient at first:

```cpp
struct AsrCapabilities {
    bool native_streaming;
    bool cumulative_partials;
    bool revisable_partials;
    bool automatic_language_detection;
    bool language_tags;
    bool punctuation;
    bool timestamps;
    bool diarization;
    bool requires_vad;
};

class AsrBackend {
public:
    virtual ~AsrBackend() = default;
    virtual const AsrCapabilities& capabilities() const = 0;
    virtual bool start_session(const AsrSessionConfig&, std::string& error) = 0;
    virtual bool feed_audio(std::span<const float>, std::string& error) = 0;
    virtual bool finish_session(std::string& error) = 0;
};
```

Initially there would still be only one production implementation: `NemotronAsrBackend`.

This still provides immediate value:

- The controller and protocol no longer silently assume Nemotron-specific behavior.
- A future benchmark backend can be added without rewriting UI logic.
- Features such as streaming, language detection, and timestamps become explicit capabilities instead of implicit assumptions.

---

## 5. Detailed ASR Analysis

### 5.1 What DictScribe currently uses

DictScribe uses NVIDIA Nemotron 3.5 ASR Streaming 0.6B through the NeMo-Speech C API.

The worker configures, among other things:

- 16 kHz mono audio,
- cache-aware streaming,
- `chunk_size = 0.16` seconds,
- left/right CTC context,
- interim results,
- automatic punctuation,
- and `language_code = nullptr` for automatic language mode.

The runtime controller feeds a new audio block roughly every 100 ms and sends cumulative `transcript_update` events containing text plus a `final` flag to the desktop controller.

Relevant sources:

- `src/workers/asr/transcription_engine.cpp`
- `src/workers/asr/runtime_controller.cpp:227-270`

The official Nemotron model card describes the model as:

- approximately 600M parameters,
- Cache-Aware FastConformer-RNNT,
- native streaming,
- configurable 80/160/320/560/1120 ms chunks,
- punctuation and capitalization,
- 40 language-locale tokens across several support tiers,
- 32 locales immediately usable for transcription,
- automatic language recognition and language tags with `target_lang=auto`.

Reference: [NVIDIA Nemotron 3.5 ASR Streaming 0.6B](https://huggingface.co/nvidia/nemotron-3.5-asr-streaming-0.6b)

For a dictation application, the crucial property is that the architecture processes **new audio frames while reusing encoder context**, instead of repeatedly reprocessing overlapping windows of audio for every update.

### 5.2 What VoxType actually uses by default

Despite the README's emphasis on Cohere Transcribe, VoxType's default configuration is still:

```toml
# engine = "whisper"

[whisper]
model = "base.en"
language = "en"
on_demand_loading = false
```

Sources:

- `config/default.toml:6-9`
- `config/default.toml:81-107`

This means:

- VoxType is not fundamentally a Cohere-based application.
- Cohere is a newer optional backend.
- The default experience remains a conventional push-to-talk batch workflow using Whisper.

This distinction matters because a README claim such as “9–11× realtime” can easily be interpreted as describing a 9–11× faster live streaming pipeline. In practice it mostly describes throughput for already-recorded audio. A high real-time factor does **not** automatically imply:

- low time-to-first-partial,
- stable interim hypotheses,
- low sustained power draw,
- or good behavior during continuous recording.

### 5.3 Cohere Transcribe in VoxType

#### Model characteristics

According to the official model card, Cohere Transcribe 03-2026 is:

- a 2B-parameter ASR model,
- a Conformer encoder with a lightweight autoregressive Transformer decoder,
- Apache-2.0 licensed,
- trained for 14 languages,
- primarily optimized for high-quality offline transcription.

The model card also describes relevant limitations:

- no explicit built-in automatic language detection,
- best results when one language is selected in advance,
- inconsistent code-switching,
- no timestamps,
- no speaker diarization,
- increased tendency to transcribe silence or non-speech,
- therefore an upstream noise gate or VAD is recommended.

Reference: [Cohere Transcribe 03-2026](https://huggingface.co/CohereLabs/cohere-transcribe-03-2026)

#### VoxType implementation

VoxType loads the following files for Cohere:

- `encoder_model.onnx`
- `decoder_model_merged.onnx`
- `tokenizer.json`

The implementation uses:

- 128-bin Mel features,
- a fixed language token selected from 14 supported language codes,
- punctuation and ITN tokens,
- one full encoder pass over the supplied audio,
- followed by autoregressive greedy decoding with KV cache in the text decoder.

Sources:

- `src/config/engines/cohere.rs`
- `src/transcribe/cohere.rs:42-107`
- `src/transcribe/cohere.rs:138-253`
- `src/transcribe/cohere.rs:255-...`

The key point is that this Cohere backend does **not** override `as_streaming()` in the inspected version. From the daemon's point of view, it is a batch transcriber.

#### Model sizes in VoxType's registry

VoxType lists approximately:

- `cohere-transcribe-q4f16`: ~1.5 GB
- `cohere-transcribe-q4`: ~2.0 GB
- `cohere-transcribe-int8`: ~2.9 GB
- larger FP16 variants above that

Source:

- `src/setup/model.rs:513-615`

The quantization labels correspond to particular ONNX exports and should not be compared directly to GGUF file sizes or llama.cpp quantization labels.

#### Suitability for DictScribe

Cohere Transcribe is interesting as an **optional quality benchmark**, but it is not an obvious replacement for Nemotron:

| Criterion | Nemotron 3.5 Streaming 0.6B | Cohere Transcribe 2B in VoxType |
|---|---:|---:|
| Parameters | ~0.6B | 2B |
| Native live-streaming architecture | Yes | No in the inspected VoxType path |
| Cumulative partial hypotheses | Yes | No |
| Automatic language detection | Yes | No |
| Directly relevant language coverage | Broader | 14 |
| Code-switching | Automatic utterance-level detection available | Described as inconsistent |
| Punctuation | Yes | Yes |
| VAD strongly recommended | Not presented as a central limitation | Yes |
| Memory/download footprint | Smaller | Larger |
| Fit for live overlay | High | Lower |

**Conclusion:** Cohere may be more accurate in some completed batch-transcription scenarios. For DictScribe's live requirements, switching would give up several existing capabilities while increasing model size and integration complexity.

### 5.4 Parakeet in VoxType

VoxType contains two different Parakeet paths that should not be conflated.

#### General Parakeet TDT 0.6B v3 batch path

The general model variant supports 25 European languages according to NVIDIA and uses a CC-BY-4.0 license.

Reference: [NVIDIA Parakeet TDT 0.6B v3](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3)

VoxType's registry lists a larger default variant and a smaller INT8 variant. This path is marked as non-streaming in the implementation.

Source:

- `src/setup/model.rs:145-170`

#### Special streaming path

VoxType also contains `ParakeetStreamingTranscriber` with:

- one shared loaded model,
- separate state per session,
- chunk ingestion,
- partial and final events,
- explicit flush behavior on close.

However, this path uses a special model named `parakeet-unified-en-0.6b`, which the model catalog describes as **English-only**. It is not simply the multilingual Parakeet-v3 batch model exposed through a streaming API.

Sources:

- `src/transcribe/parakeet_streaming.rs`
- `src/setup/model.rs:172-217`

#### Suitability for DictScribe

The streaming implementation is technically interesting, but it does not provide a clear model-level advantage over Nemotron:

- similar parameter class,
- larger model artifacts in the VoxType catalog,
- English-only for the particular streaming export,
- while Nemotron is already multilingual, cache-aware, and integrated.

Parakeet should therefore **not** be prioritized as a Nemotron replacement at this stage.

### 5.5 Whisper and “eager” chunking

For batch-only backends, VoxType can accumulate audio windows during recording, transcribe overlapping chunks, and deduplicate the resulting text. This is a useful fallback technique for models that lack a native streaming API.

However, it should not be confused with true cache-aware streaming:

- overlapping windows recompute portions of the audio,
- segment boundaries and deduplication can introduce errors,
- partial text is often less stable,
- compute usage rises because audio is processed repeatedly.

For DictScribe, an eager mode would only be valuable if a future batch ASR model is compelling enough to benchmark. It should not replace Nemotron's native streaming path.

---

## 6. ASR Recommendations for DictScribe

### 6.1 Short term

1. **Keep Nemotron as the default ASR backend.**
2. Make backend capabilities explicit.
3. Remove DictScribe's artificial limitation on available languages.
4. Prepare a benchmark harness without forcing a second ASR backend into the production path.

### 6.2 DictScribe currently restricts language support unnecessarily

The application currently validates essentially only:

- `auto`
- `de`
- `en`

This is much narrower than the actual capability of the Nemotron model. That is a product/configuration limitation, not a model limitation.

Relevant sources:

- `src/app/model_discovery.cpp:184-191`
- language-switching path in `src/app/app_controller.cpp`

One of the most immediately valuable changes would therefore be to:

- load supported language locales from a model manifest,
- keep `auto` as the default,
- expose all production-capable locales supported by the installed model,
- return detected language information in ASR events,
- avoid hard-coded `de/en` branching in the controller.

This aligns directly with the goal of avoiding language-specific hardcoding.

### 6.3 Optional Cohere benchmark

Cohere should only be integrated if the goal is to answer this question with measurements:

> Is a slower batch-finalization step after key release sufficiently more accurate to justify an optional “Quality mode”?

Such a benchmark must measure more than WER. At minimum:

- word error rate and character error rate,
- punctuation quality,
- proper names and technical vocabulary,
- numbers, dates, paths, and IDs,
- German, English, and mixed technical language,
- silence/noise hallucination rate,
- time to first visible text,
- time from key release to final insertion,
- real-time factor,
- average and p95 CPU utilization,
- memory usage,
- model size,
- energy/package power per dictated minute,
- behavior with 2, 4, and 8 CPU threads.

Only a substantial quality improvement on DictScribe's real dictation corpus would justify an optional batch mode.

---

## 7. Streaming and Revision Model

### 7.1 VoxType's streaming event model

VoxType separates batch and streaming transcribers. Its streaming interface can produce events such as:

```rust
Partial { text, segment_id }
Final { text, segment_id }
Replace { backspace, text, segment_id }
Ended
Error(...)
```

Source:

- `src/transcribe/streaming.rs:52-141`

This is a strong architectural idea because it makes different streaming semantics explicit:

- provisional text,
- finalized text,
- revision of previously emitted text,
- end of session,
- failure.

### 7.2 Problematic part: typing partial text directly into target applications

VoxType can type live partials directly into the active application. When a partial is revised, it can issue backspaces and type replacement text.

That is pragmatic for some Linux setups, but it introduces substantial risk:

- The user or target application may move the cursor during recording.
- Backspace may then delete unrelated text.
- Unicode code points and grapheme clusters do not always map cleanly to one backspace per visible character.
- Autocorrect or editor formatting may have changed the text after it was inserted.
- Newlines/Enter events can trigger actions in chat applications.
- A focus switch during dictation can cause corrections to be sent to the wrong window.

DictScribe's overlay model is safer:

- partials remain in DictScribe's own window,
- the target is captured at dictation start,
- only accepted/final text is inserted into the external application.

### Recommendation

DictScribe should adopt **the event semantics**, not the cursor manipulation.

A possible JSONL protocol could look like this:

```json
{
  "v": 2,
  "type": "asr_update",
  "session_id": 42,
  "hypothesis_id": 17,
  "revision": 23,
  "text": "...",
  "final": false,
  "detected_language": "de-DE",
  "stability": "revisable"
}
```

Future backends could optionally add:

```json
{
  "segment_id": 3,
  "replaces_revision": 22,
  "start_ms": 1280,
  "end_ms": 3520
}
```

This gives the controller a clear ordering model and allows stale events to be discarded safely.

### 7.3 Connection to the new cleanup plan

VoxType's explicit segment/revision model fits very well with DictScribe's planned incremental cleanup design:

- ASR hypotheses have revisions.
- Append-only new text should not automatically invalidate an in-flight cleanup.
- Changes inside the span covered by a cleanup request should invalidate that response.
- The controller remains the sole owner of transcript composition.

This combination is stronger than either current implementation by itself:

- VoxType contributes a useful streaming-event abstraction.
- DictScribe contributes the safer bounded semantic tail and revision validation.

---

## 8. Text Processing and LLM Cleanup

### 8.1 VoxType's deterministic text processing

`src/text/mod.rs` contains, among other things:

- a hard-coded English map for spoken punctuation,
- fixed replacements such as `question mark`, `new paragraph`, `period`, `colon`, and `slash`,
- configurable word replacements,
- regex-based filler-word removal,
- a hard-coded `submit` trigger concept.

Primary spoken-command list:

- `src/text/mod.rs:165-225`

Filler-word handling begins around:

- `src/text/mod.rs:228-271`

This is practical for an English-centered desktop workflow, but it conflicts directly with DictScribe's intended principle:

> Semantic interpretation of dictated language should not depend on per-language keyword lists and rules maintained by the application.

DictScribe should therefore **not copy this layer**.

### 8.2 VoxType's external postprocessor

VoxType can pipe transcribed text into an arbitrary shell command:

```toml
[output.post_process]
command = "ollama run ..."
timeout_ms = 30000
```

Text is sent to `sh -c` over `stdin`. Optional context from the previous dictation can be supplied through the `VOXTYPE_CONTEXT` environment variable. On failure, timeout, or empty output, VoxType can fall back to the original text.

Sources:

- `src/output/post_process.rs:1-139`
- `src/daemon.rs:1964-2085`

#### Good ideas in this design

- Post-processing is optional.
- It has a timeout.
- Failure does not disable basic dictation.
- Empty output can fall back to raw transcription.
- Users can integrate external tools.
- Different profiles can choose different post-processing commands.

#### Why this is not suitable as DictScribe's default cleanup architecture

- Arbitrary shell commands create a large security and support surface.
- The result is unconstrained free text.
- There is no grammar/schema validation.
- Numbers, paths, and identifiers are not systematically protected.
- There is no model-bounded editable region.
- Requests do not carry transcript/ASR revision information.
- The model can rewrite the entire text arbitrarily.
- Previous-dictation context can bleed semantically into a new dictation.
- The mechanism is designed for batch output.

VoxType explicitly disables this postprocessor in the streaming path because processing only the newly finalized tail would be inconsistent with already visible text.

Source:

- `src/output/streaming.rs:175-184`

That limitation indirectly supports DictScribe's controller-owned bounded-tail design.

### 8.3 DictScribe's planned cleanup is conceptually stronger

For AI cleanup, the supplementary design should remain the primary direction:

- `docs/SEMANTIC_INCREMENTAL_CLEANUP_PLAN.md`

Key properties:

- no language-specific semantic regex rules,
- clear `Off` and `AI cleanup` modes,
- Qwen3.5-0.8B as the first Apache-2.0 candidate,
- readonly context plus a bounded editable structured tail,
- hard input/output limits,
- only `replacement_tail` is generated by the model,
- grammar-constrained output,
- technical-literal protection,
- session/revision validation,
- immediate raw-text fallback,
- no final whole-transcript rewrite.

This is particularly well suited to lists and paragraphs because the model can make a multilingual semantic decision. For example:

```text
Einkaufsliste Doppelpunkt Brot Mehl Milch Müsli
```

may become:

```text
Einkaufsliste:

- Brot
- Mehl
- Milch
- Müsli
```

without DictScribe hard-coding the German terms “Einkaufsliste” or “Doppelpunkt.”

### 8.4 Useful VoxType idea: cleanup profiles

VoxType supports profiles selected through configuration or hotkey modifiers. This idea maps well to DictScribe as long as profiles define **model behavior**, not language-specific rules.

Potential DictScribe profiles:

- **Normal:** grammar, punctuation, self-corrections, and clearly implied formatting.
- **Notes:** shorter sentences, compact structure, lists where a list is clearly intended.
- **Formal:** complete sentences and a more formal tone without adding information.
- **Verbatim:** only obvious ASR artifacts; minimal rewriting.
- **Code/Technical:** maximum preservation of literals, paths, versions, and identifiers.

Each profile should change only parameters such as:

- system prompt or task/profile ID,
- permitted degree of rewriting,
- tail/output limits,
- literal-protection level.

Profiles should **not** contain separate German, English, French, etc. trigger-word lists.

### 8.5 Context from previous dictations

VoxType can pass the previous dictation to the postprocessor when it is less than 60 seconds old.

Source:

- `src/daemon.rs:2005-2012`

This can help interpret follow-up utterances, but it introduces risks:

- incorrect semantic carry-over,
- accidental repetition,
- topic changes are not reliably detected,
- larger prompts,
- harder-to-explain behavior,
- privacy and expectation issues.

For DictScribe, previous-dictation context should therefore be:

- **off by default**,
- available only as an explicitly enabled profile feature,
- marked read-only,
- tightly bounded,
- never part of the editable region,
- never allowed to inject technical literals from a previous session into the current output.

---

## 9. Model Lifecycle and Resource Usage

### 9.1 VoxType's `prepare()` concept

The `Transcriber` trait contains an optional `prepare()` method. For subprocess backends this allows the application, at the start of recording, to:

- start the worker,
- load the model,
- wait for readiness,

while the user is still speaking.

Source:

- `src/transcribe/mod.rs:115-125`

VoxType's worker protocol loads the model first, writes `READY`, and then accepts binary audio samples. After transcription, the process may terminate and release GPU memory.

Source:

- `src/transcribe/worker.rs`

### 9.2 ModelManager, cache, and idle eviction

VoxType also contains mechanisms for:

- primary/secondary Whisper models,
- limiting the number of loaded models,
- LRU/idle eviction,
- on-demand loading,
- GPU isolation using short-lived subprocesses.

Source:

- `src/model_manager.rs`

The implementation is not fully backend-neutral, but the product-level idea is useful.

### 9.3 Recommendation for DictScribe

#### ASR

Nemotron should initially remain persistent because:

- it continuously streams,
- first-partial latency matters,
- frequent model reloads would harm UX,
- ASR is needed for every session.

An idle-unload option could be tested later for very long periods of inactivity.

#### Rewrite model

VoxType's lifecycle approach is immediately useful for DictScribe's rewrite worker:

1. In `Off` mode, do not launch the rewrite worker.
2. When switching to `AI cleanup`, start it on demand.
3. Alternatively, when an AI-cleanup dictation begins, launch/prewarm Qwen while the user is speaking.
4. A `ready` event indicates whether cleanup is available.
5. If it is not ready, raw transcription remains fully usable and finalizable.
6. After a configurable long idle period, the rewrite worker can be terminated.

This directly addresses one of DictScribe's current resource problems: loading and running a rewrite model even when cleanup is not needed.

#### Suggested lifecycle states

```text
Disabled
  └── AI cleanup disabled; no process

Starting
  └── process launched; model loading

Ready
  └── jobs may be submitted

Busy
  └── one job active; at most one coalesced successor

IdleWarm
  └── keep model resident briefly

Stopping / Failed
  └── raw text remains available
```

---

## 10. Model Registry and License Metadata

### 10.1 VoxType's useful central catalog

`src/setup/model.rs` contains a central catalog including:

- model ID,
- download source,
- expected files,
- approximate sizes,
- engine mapping,
- languages,
- streaming compatibility.

This scales better than DictScribe's current fixed filename discovery.

### 10.2 Current DictScribe limitation

DictScribe currently searches for fixed models/files and treats both roles as required at startup:

- fixed Nemotron model path,
- fixed Qwen rewrite model path,
- rewrite worker launched regardless of cleanup mode.

Relevant sources:

- `src/app/model_discovery.cpp`
- `src/app/app_controller.cpp:43-93`

This makes the following unnecessarily difficult:

- new quantizations,
- model upgrades,
- optional rewrite models,
- displaying license information,
- checking backend/model capabilities,
- reproducible benchmarks.

### 10.3 Suggested manifest format

A DictScribe rewrite-model manifest could look like this:

```json
{
  "schema": 1,
  "id": "qwen3.5-0.8b-q8_0",
  "role": "rewrite",
  "display_name": "Qwen3.5 0.8B Q8_0",
  "backend": "llama.cpp",
  "architecture": "qwen3.5",
  "license": {
    "spdx": "Apache-2.0",
    "source": "https://huggingface.co/..."
  },
  "files": [
    {
      "name": "Qwen3.5-0.8B-Q8_0.gguf",
      "sha256": "...",
      "bytes": 0
    }
  ],
  "capabilities": {
    "structured_output": true,
    "grammar": true,
    "multilingual_cleanup": true
  },
  "runtime": {
    "context_tokens": 2048,
    "recommended_cpu_threads": 4,
    "chat_template": "qwen3.5",
    "gpu_layers": "auto"
  }
}
```

An ASR manifest could look like:

```json
{
  "id": "nemotron-3.5-asr-streaming-0.6b",
  "role": "asr",
  "backend": "nemo-speech",
  "license": {
    "spdx": "OpenMDW-1.1",
    "source": "https://huggingface.co/nvidia/nemotron-3.5-asr-streaming-0.6b"
  },
  "capabilities": {
    "native_streaming": true,
    "automatic_language_detection": true,
    "punctuation": true,
    "timestamps": false,
    "diarization": false
  },
  "languages": ["auto", "de-DE", "en-US", "en-GB", "..."]
}
```

### 10.4 License handling

The source-code license of an application does not determine the license of bundled/downloaded models:

- VoxType code: MIT
- DictScribe code: Apache 2.0
- Cohere Transcribe: Apache 2.0
- Parakeet TDT 0.6B v3: CC-BY-4.0
- Nemotron 3.5 ASR: OpenMDW 1.1
- Qwen3.5-0.8B: Apache 2.0

The model registry should expose license and source metadata for every model. This becomes especially important if DictScribe automatically downloads models or ships them with releases.

This is a technical recommendation, not legal advice.

---

## 11. Voice Activity Detection

### 11.1 VoxType's approach

VoxType abstracts voice activity detection behind a common interface and includes at least:

- simple energy/RMS-based detection,
- model-based VAD variants.

Before batch ASR, this can be used to:

- detect silent recordings,
- skip extremely short utterances,
- reduce hallucinations caused by background noise or silence.

If VAD fails, the inspected path generally falls back to normal processing rather than losing the dictation completely.

Relevant sources:

- `src/vad/mod.rs`
- VAD use in the batch transcription path in `src/daemon.rs`

### 11.2 Why VAD does not violate the no-hardcoding principle

VAD does not interpret words or grammar. It only estimates acoustic state:

- speech present,
- silence/non-speech,
- possible utterance boundary.

It is therefore language-independent signal processing and is compatible with DictScribe's goal of keeping semantic interpretation inside the models.

### 11.3 Recommended use in DictScribe

VAD should not be used as a destructive gate that drops audio before ASR. A safer approach is:

1. Continue sending all audio to Nemotron.
2. Evaluate a lightweight VAD in parallel.
3. Emit VAD hints such as:
   - `speech_started`
   - `speech_paused`
   - `speech_resumed`
   - `long_silence`
4. On longer speech pauses:
   - allow more aggressive ASR stabilization,
   - trigger a bounded cleanup job,
   - do not apply any semantic text rules.
5. For a completely silent session:
   - avoid unnecessary cleanup,
   - avoid inserting hallucinated text where possible,
   - provide a subtle user indication if needed.

#### Initial implementation

A language-neutral energy gate is sufficient for the first version:

- rolling RMS,
- adaptive noise floor,
- hysteresis,
- minimum speech duration,
- minimum pause duration,
- fail-open behavior when uncertain.

Silero VAD or another compact model can be evaluated later if necessary.

---

## 12. Output, Lists, and Paragraphs

### 12.1 Why this area becomes critical

Once Qwen begins producing real structure, output will increasingly contain:

- `\n`
- blank lines,
- bullet points,
- numbered lists,
- indentation.

A dictation application must not only render this correctly in its overlay; it must inject it safely into very different target applications.

### 12.2 VoxType's output abstraction

VoxType implements multiple `TextOutput` strategies and fallback paths, including:

- `wtype`
- `eitype`
- `dotool`
- `ydotool`
- clipboard/paste
- file output

It also handles applications where a normal Enter key submits a message. Some backends therefore support multiline output through **Shift+Enter**.

This is directly relevant to DictScribe.

### 12.3 Current Windows output in DictScribe

DictScribe currently sends the entire UTF-16 string through `SendInput` with `KEYEVENTF_UNICODE`. If that fails, it copies the text to the clipboard.

Relevant sources:

- `src/platform/win/win_text_injector.cpp:110-143`
- `src/platform/win/win_text_injector.cpp:189-216`
- `src/platform/win/win_main.cpp:289-312`

This is sensible for simple single-line text. Multiline structured output needs explicit testing because target applications handle Unicode newline characters and physical Enter key events differently.

### 12.4 Recommended output modes

DictScribe should have a small output abstraction:

```cpp
enum class OutputMode {
    Auto,
    UnicodeTyping,
    ClipboardPaste,
    ShiftEnterMultiline,
    CopyOnly
};
```

#### `Auto`

- single-line text: Unicode injection,
- multiline text: prefer clipboard paste,
- known chat applications: optionally use Shift+Enter mode,
- failure: copy-only fallback plus user notification.

#### `UnicodeTyping`

- good for short single-line dictation,
- does not overwrite the clipboard,
- may be slower in some editors.

#### `ClipboardPaste`

- generally more reliable for multiline Unicode text,
- better preserves list structure,
- requires a clear clipboard preservation/restoration policy.

#### `ShiftEnterMultiline`

- split text on `\n`,
- type or paste each segment,
- send Shift+Enter between lines,
- use only explicitly or through a target-app profile.

#### `CopyOnly`

- safest fallback,
- user pastes manually.

### 12.5 Required target-application tests

At minimum test:

- Windows Notepad
- Microsoft Word
- VS Code
- browser text fields
- browser `contenteditable`
- Slack
- Microsoft Teams
- Discord
- terminal
- Markdown editor

For each application, test:

- one paragraph,
- two paragraphs,
- bullet list,
- numbered list,
- Unicode and emoji,
- technical paths,
- switching target while recording,
- loss of focus immediately before insertion.

VoxType demonstrates an important product lesson here: **text output is part of semantic correctness**. A perfectly formatted LLM result is not useful if injection destroys its structure or submits a chat message accidentally.

---

## 13. Configuration Profiles and UX

VoxType has a large configuration system and can select profiles using hotkey modifiers. DictScribe should implement a smaller, focused version of this idea.

### Suggested minimal model

```text
Ctrl+Alt+Space              → Normal
Ctrl+Alt+Shift+Space        → Notes/List
Ctrl+Alt+Win+Space          → Verbatim
```

Or through the tray menu:

```text
Cleanup mode
  ○ Off
  ● Normal
  ○ Notes
  ○ Formal
  ○ Verbatim
```

Each dictation session should capture an immutable profile ID at start. This prevents a running cleanup request from being interpreted using settings that changed mid-session.

A profile may define:

- cleanup enabled/disabled,
- prompt/profile ID,
- tail limit,
- literal-protection policy,
- output mode,
- optional target-application newline strategy.

A profile should **not** define:

- concrete filler words,
- concrete spoken punctuation commands,
- language-specific list keywords,
- language-specific regex transformations.

---

## 14. What DictScribe Should Adopt from VoxType

### 14.1 Priority P0 — next architecture milestone

| VoxType idea | Recommended adaptation | Benefit |
|---|---|---|
| ASR backend interface | Implement a small capability-based version | Removes Nemotron-specific assumptions from controller logic |
| Capability metadata | Add to worker handshake and model registry | Makes streaming, language support, partial semantics, and VAD explicit |
| Model registry | Shared concept for ASR and rewrite models | Removes fixed filenames; exposes license/size/capabilities |
| Lazy loading / prepare for rewrite | Adopt directly | No rewrite RAM/CPU cost in Off mode; hide load latency behind recording |
| VAD abstraction | Language-neutral and fail-open | Reduces silence hallucination risk and improves pause-triggered cleanup |
| Explicit streaming events | Adapt to DictScribe's cumulative hypotheses | Safer revisions and future backend extensibility |
| Multiline output strategy | Windows first, Linux later | Makes paragraphs and lists actually usable |
| Output fallback chain | Typing → clipboard → copy-only | Higher reliability |
| Scenario tests | Use fake ASR and fake rewrite workers | Reproduce revision, timeout, and output failures deterministically |

### 14.2 Priority P1 — after semantic-cleanup MVP

| Idea | Recommended adaptation | Benefit |
|---|---|---|
| Cleanup profiles | Model-driven profiles without language rules | Lets users choose Notes, Formal, or Verbatim behavior |
| Idle unload | Initially rewrite worker only | Lower sustained RAM and fan/power usage |
| Output backend interface | Platform-neutral controller contract | Brings Windows/Linux behavior closer together |
| Model download and verification | Manifest + hash + atomic install | Reproducible model management |
| ASR benchmark harness | Offline CLI, separate from product path | Objective comparison of future models |
| Detected language in UI | Reported by backend | Better debugging and transparency |

### 14.3 Priority P2 — later experiments

| Idea | Assessment |
|---|---|
| Cohere as batch-quality mode | Benchmark only; do not make default initially |
| Additional ASR backends | Add only for real quality, licensing, or hardware needs |
| External plugin postprocessor | Advanced feature only, with explicit security boundary |
| Meeting mode | Separate product feature; distraction from core dictation for now |
| Cloud/remote backends | Does not fit current local-first core; optional at most later |

---

## 15. What Should Explicitly Not Be Copied

### 15.1 English spoken-punctuation and filler-word rules

Do not copy rules like:

```text
"question mark" → ?
"new paragraph" → \n\n
"period" → .
English filler-word regexes
"submit" as a global spoken trigger
```

Reasons:

- language-dependent,
- ambiguous,
- maintenance-heavy,
- undermines multilingual model behavior,
- can misinterpret ordinary words as commands.

### 15.2 Arbitrary shell post-processing as the default

Do not make the primary product path:

```text
sh -c <arbitrary user command>
```

Reasons:

- large security surface,
- poor reproducibility,
- no structured contract,
- no revision semantics,
- no literal-preservation guarantees,
- difficult to support.

It may be reasonable later as an explicitly enabled advanced integration outside the safe default path.

### 15.3 Direct injection of unstable partials

Do not copy:

- typing partially recognized words directly into another application,
- correcting them later with Backspace.

Reasons:

- cursor and focus risk,
- Unicode/grapheme correctness issues,
- risk of deleting unrelated user text,
- conflicts with DictScribe's safer overlay model.

### 15.4 Automatically using previous dictation as context

Do not enable this by default. If ever supported, require explicit opt-in and strict limits.

### 15.5 The entire backend matrix

DictScribe does not currently need nine ASR backends, a large compile-time feature matrix, and several GPU runtimes. The abstraction is useful; the product complexity is not.

---

## 16. Concrete Implementation Plan for DictScribe

This plan supplements, but does not replace, `SEMANTIC_INCREMENTAL_CLEANUP_PLAN.md`.

### Phase 1: Model and Capability Foundation

#### 1.1 Shared model metadata

Add files such as:

```text
src/models/model_manifest.hpp
src/models/model_manifest.cpp
src/models/model_registry.hpp
src/models/model_registry.cpp
models/registry.json
```

Tasks:

- remove fixed model filenames from `model_discovery.cpp`,
- distinguish ASR and rewrite roles,
- record license, source, size, and hash,
- record supported languages and capabilities,
- do not treat a missing rewrite model as a startup failure when cleanup is Off.

#### 1.2 Extend worker handshake

ASR worker should report something like:

```json
{
  "type": "ready",
  "worker": "asr",
  "backend": "nemo-speech",
  "model_id": "nemotron-3.5-asr-streaming-0.6b",
  "capabilities": {
    "native_streaming": true,
    "cumulative_partials": true,
    "automatic_language_detection": true,
    "language_tags": true,
    "punctuation": true,
    "timestamps": false,
    "diarization": false
  }
}
```

Rewrite worker should report:

```json
{
  "type": "ready",
  "worker": "rewrite",
  "model_id": "qwen3.5-0.8b-q8_0",
  "capabilities": {
    "grammar": true,
    "structured_output": true,
    "max_context_tokens": 2048
  }
}
```

#### 1.3 Open up language configuration

- remove fixed `auto/de/en` validation,
- derive supported locales from the manifest,
- transport `detected_language` in ASR updates,
- build the UI selector dynamically.

### Phase 2: Rewrite Lifecycle

#### 2.1 Start worker only when needed

Primary files likely affected:

- `src/app/app_controller.cpp`
- application configuration / snapshot state
- platform menus

Rules:

- `Off`: no rewrite process.
- `AI cleanup`: start or prewarm the process.
- A worker that is not ready never blocks raw transcription.
- Rewrite failure degrades cleanup only; it must not put the entire application into an error state.

#### 2.2 Prewarm during recording

At the start of an AI-cleanup session:

1. Start ASR recording immediately.
2. Start the rewrite worker in parallel.
3. Allow cleanup jobs only after a `ready` event.
4. Continue maintaining raw transcript and stabilization state while the model loads.

#### 2.3 Idle unload

Optional follow-up:

- terminate after e.g. 10–20 minutes without an AI-cleanup session,
- never terminate during an active job,
- prewarm again on the next dictation.

### Phase 3: ASR Events and Stabilization

#### 3.1 Protocol v2

Every ASR update should include:

- `session_id`
- `hypothesis_id`
- `revision`
- `final`
- `detected_language`
- optional `audio_end_ms`

#### 3.2 Controller state

```cpp
struct TranscriptState {
    std::string frozen_clean_text;
    std::string readonly_context;
    std::string editable_tail;
    std::string stable_raw_pending;
    std::string unstable_raw_suffix;
    uint64_t session_id;
    uint64_t asr_revision;
    uint64_t tail_revision;
};
```

#### 3.3 No direct injection of partials

- Overlay shows the live hypothesis.
- External applications receive accepted/finalized text only.
- Cancel never deletes text in the target application.

### Phase 4: VAD

Potential new files:

```text
src/audio/voice_activity_detector.hpp
src/audio/energy_vad.cpp
```

Initial integration should probably live in the ASR worker because the audio stream already passes through it.

Possible events:

```json
{"type":"speech_state","state":"started"}
{"type":"speech_state","state":"paused","duration_ms":720}
{"type":"speech_state","state":"resumed"}
```

Uses:

- align cleanup debounce with natural pauses,
- detect completely silent sessions,
- no language-specific semantic rules.

### Phase 5: Semantic Cleanup as Already Planned

Implement the bounded semantic tail from the existing design:

- Qwen3.5-0.8B Q8 first,
- Q4 only after establishing a Q8 quality baseline,
- grammar with exactly one `replacement_tail`,
- clear separation between readonly context and editable tail,
- literal protection,
- append-only-safe revision semantics,
- at most one active cleanup job and one coalesced successor,
- no whole-transcript final pass.

VoxType's unconstrained postprocessor should not replace this path.

### Phase 6: Safe Multiline Output

#### 6.1 Output policy abstraction

Potential structure:

```text
src/output/text_output.hpp
src/output/windows_unicode_output.cpp
src/output/windows_clipboard_paste_output.cpp
src/output/windows_shift_enter_output.cpp
```

#### 6.2 Automatic mode selection

```text
no newline        → UnicodeTyping
contains newline  → ClipboardPaste
chat app profile  → ShiftEnterMultiline
failure           → CopyOnly
```

#### 6.3 Gate automatic list formatting on output reliability

The AI should not generate multiline lists by default until output behavior is verified in the most important target applications.

### Phase 7: Profiles

Profiles should be safe prompt/output configuration, not regex bundles:

```json
{
  "id": "notes",
  "cleanup": true,
  "prompt_profile": "notes-v1",
  "editable_tail_tokens": 224,
  "max_output_tokens": 384,
  "literal_policy": "strict",
  "output_mode": "auto"
}
```

### Phase 8: Optional ASR Benchmark Harness

Example CLI:

```text
dictscribe-asr-benchmark \
  --backend nemotron \
  --corpus testdata/asr-manifest.json \
  --threads 4 \
  --output results/nemotron.json
```

Later:

```text
--backend cohere-onnx
--backend parakeet-onnx
```

The benchmark harness should remain independent of the production UI and output pipeline. This allows new ASR models to be evaluated without first integrating them into the full application.

---

## 17. Recommended Tests

VoxType contains extensive unit and integration tests. DictScribe does not need to replicate all of them, but it should systematically cover the highest-risk state transitions.

### 17.1 ASR protocol

- Correct update ordering.
- Duplicate revision is ignored.
- Older revision is ignored.
- New append-only update does not invalidate older cleanup.
- Revision inside the edited span invalidates cleanup.
- Final arrives while cleanup is running.
- ASR worker exits during recording.
- Audio ring buffer reports dropped frames.
- Detected language is forwarded correctly.

### 17.2 VAD

- complete silence,
- keyboard noise without speech,
- very quiet speech,
- background music,
- short hesitation in the middle of a sentence,
- long pause between paragraphs,
- VAD failure falls open to normal ASR behavior.

### 17.3 Cleanup

- no-op on already-good text,
- German and English filler words handled by the model without hard-coded lists,
- self-correction,
- enumeration converted into bullet list,
- numbered steps,
- two paragraphs,
- mixed technical language,
- file paths,
- version numbers,
- URLs,
- email addresses,
- IDs and hashes,
- prompt-injection-like dictation,
- empty/invalid model response,
- timeout,
- rewrite worker crash.

### 17.4 Output

- single-line Unicode,
- multiline clipboard paste,
- Shift+Enter mode,
- clipboard unavailable,
- target application closed,
- target loses focus,
- DictScribe/overlay accidentally becomes target,
- Unicode outside the BMP,
- CRLF/LF normalization,
- long list.

### 17.5 Model lifecycle

- Application starts without rewrite model when mode is Off.
- Switching to AI cleanup starts worker.
- Recording starts while model is still loading.
- Raw text remains available throughout.
- Rewrite worker unloads after idle timeout.
- New session starts/prewarms it again.
- ASR remains independently usable if rewrite fails.

---

## 18. Suggested ASR Benchmark Matrix

| Dimension | Cases |
|---|---|
| Language | German, English, mixed technical German/English |
| Speakers | different genders, accents, speaking speeds |
| Duration | 2 s, 10 s, 30 s, 60 s |
| Environment | quiet, laptop fan, keyboard, music, street noise |
| Content | everyday speech, email, code terms, numbers, paths, lists |
| Pauses | none, short thinking pauses, long paragraph pause |
| Hardware | high-end laptop, 4C/8T CPU, ideally one low-power device |
| Threads | 2, 4, 8 |

### Metrics

```text
WER / CER
Punctuation F1
Exact preservation: numbers, paths, IDs
Silence hallucination rate
First partial latency
Partial revision count
Final latency after stop
Real-time factor
Average and p95 CPU
Peak RAM
Model disk size
Energy or package power per dictated minute
```

### Replacement criteria

A future ASR model should replace Nemotron only if it:

1. covers required languages at least as well,
2. does not materially worsen live latency,
3. provides stable partials or an equivalent UX path,
4. has acceptable total resource usage,
5. does not hallucinate more strongly on silence,
6. provides a clear measured quality improvement on a real DictScribe corpus,
7. has a license compatible with DictScribe's distribution requirements.

Based on the inspected source and model characteristics, Cohere Transcribe does not automatically satisfy several of these criteria. Benchmarking it is reasonable; switching by assumption is not.

---

## 19. Architectural Principle: Stay Small

VoxType is substantially larger than DictScribe. In the uploaded snapshot it contains approximately:

- 176 Rust source files under `src/`,
- numerous compile-time features,
- several local and remote ASR backends,
- multiple GPU execution providers,
- many Linux compositor/output integrations,
- meeting mode,
- TUI configuration,
- model installation and packaging infrastructure.

DictScribe is at a different product and maturity point. The lesson should not be to reproduce VoxType.

The better lesson is:

> **Adopt the small number of stable abstractions that make future decisions cheaper without generalizing today's product pipeline prematurely.**

That means:

- one ASR backend behind one interface,
- one rewrite backend behind a model profile,
- one model manifest/registry,
- one output interface with two to four strategies,
- one VAD interface with one initial implementation,
- no plugin ecosystem and no large backend matrix until real requirements justify them.

---

## 20. Final Recommendation

### ASR

**Keep Nemotron.** Its native cache-aware streaming architecture, smaller parameter class, automatic language detection, and broader language support are a better match for DictScribe's live overlay than VoxType's Cohere batch backend or its English-only Parakeet streaming export.

The next ASR work should focus on:

1. removing the artificial `de/en` product restriction,
2. transporting detected language and backend capabilities through the protocol,
3. adding language-independent VAD,
4. building an independent benchmark harness.

### Cleanup

**Continue with the semantic incremental cleanup design.** VoxType's regex and shell post-processing are not a better replacement. The useful ideas to borrow are:

- fail-open behavior,
- timeouts,
- profiles,
- lazy loading/prewarm,
- and potentially an advanced external-plugin path later.

### Infrastructure priorities

The highest-priority ideas to adopt are:

1. model registry with license/capability metadata,
2. lazy rewrite lifecycle with prewarming,
3. explicit streaming/revision protocol,
4. language-neutral VAD,
5. safe multiline output strategies,
6. cleanup profiles without language-specific rules,
7. scenario-driven integration tests.

### Product impact

This combination keeps DictScribe's current strengths:

- small native application,
- local processing,
- true live ASR,
- safe target-application insertion,
- multilingual model behavior,
- no semantic keyword lists per language,

while adopting the most mature infrastructure ideas from VoxType.

The recommended target architecture is therefore:

```text
Microphone
  ↓
Nemotron ASR worker
  ├── native cumulative streaming
  ├── detected language
  ├── revisioned hypotheses
  └── language-neutral VAD hints
  ↓
Controller-owned TranscriptState
  ├── stability tracking
  ├── frozen text
  ├── bounded semantic tail
  └── session/revision validation
  ↓ optional
Lazy Qwen3.5-0.8B rewrite worker
  ├── profile-driven multilingual task
  ├── grammar-constrained replacement_tail
  ├── literal validation
  └── fail-open to raw text
  ↓
Output strategy
  ├── Unicode typing for single-line text
  ├── clipboard paste for multiline text
  ├── optional Shift+Enter mode
  └── copy-only fallback
```

This is a cleaner and safer synthesis than either DictScribe's current behavior or a direct copy of VoxType's pipeline.

---

## 21. External References

- [VoxType Repository](https://github.com/peteonrails/voxtype)
- [NVIDIA Nemotron 3.5 ASR Streaming 0.6B](https://huggingface.co/nvidia/nemotron-3.5-asr-streaming-0.6b)
- [Cohere Transcribe 03-2026](https://huggingface.co/CohereLabs/cohere-transcribe-03-2026)
- [NVIDIA Parakeet TDT 0.6B v3](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3)
