# Worker JSONL protocol

Protocol versions: `1` for the existing worker lifecycle and ASR messages,
`2` for semantic tail rewrites.

## Common rules

- UTF-8 JSON Lines over stdin/stdout.
- One object per line and a maximum line size of 1 MiB.
- Every message has a supported integer `v` and a non-empty `type`.
- Commands have a unique `id`.
- Worker events have a monotonically increasing `seq`.
- Standard output contains protocol data only. Logs use standard error.
- Unknown fields are ignored; unknown command types are rejected.

Common commands:

```json
{"v":1,"type":"ping","id":"request-1"}
{"v":1,"type":"shutdown","id":"request-2"}
```

## ASR worker

Start the worker with:

```text
dictscribe-asr-worker --stdio --model MODEL.gguf --protocol-version 1
```

Commands:

```json
{"v":1,"type":"start","id":"request-3","sessionId":"session-1","language":"auto"}
{"v":1,"type":"stop","id":"request-4","sessionId":"session-1"}
{"v":1,"type":"cancel","id":"request-5","sessionId":"session-1"}
```

Important events:

```json
{"v":1,"type":"ready","seq":2,"engine":"nemo-speech.cpp"}
{"v":1,"type":"recording_started","seq":4,"sessionId":"session-1"}
{"v":1,"type":"audio_level","seq":5,"sessionId":"session-1","rms":0.08,"peak":0.31}
{"v":1,"type":"transcript_update","seq":6,"sessionId":"session-1","text":"Hallo"}
{"v":1,"type":"recording_finalized","seq":7,"sessionId":"session-1","text":"Hallo"}
```

Interim NeMo hypotheses are cumulative and may be revised. A
`transcript_update` therefore replaces the visible hypothesis.
`audio_level` is emitted approximately every 50 ms while recording. Its
normalized `rms` and `peak` fields describe actual microphone samples and are
intended for a responsive input meter; they are not synthesized UI animation.

## Rewrite worker

Start the worker with:

```text
dictscribe-rewrite-worker --stdio --model MODEL.gguf --protocol-version 1
```

The whole-transcript version-1 command remains temporarily available during
the controller migration:

```json
{"v":1,"type":"rewrite","id":"request-6","requestId":"rewrite-1","language":"de","text":"Ich will äh das Modell ändern."}
```

Result:

```json
{"v":1,"type":"rewrite_completed","seq":4,"id":"request-6","requestId":"rewrite-1","text":"Ich will das Modell ändern."}
```

New controller code must use the version-2 bounded tail command:

```json
{"v":2,"type":"rewrite_tail","id":"request-7","requestId":"rewrite-2","sessionId":"session-1","tailRevision":9,"firstStableSpanId":31,"lastStableSpanId":34,"languageHint":"de","readOnlyContext":"Der Cleanup soll lokal bleiben.","editableTail":"Dafür testen wir ein kleineres Modell.","newAsrText":"einkaufsliste doppelpunkt brot mehl milch müsli"}
```

Successful result:

```json
{"v":2,"type":"rewrite_tail_completed","seq":5,"id":"request-7","requestId":"rewrite-2","sessionId":"session-1","tailRevision":9,"firstStableSpanId":31,"lastStableSpanId":34,"replacementTail":"Dafür testen wir ein kleineres Modell.\n\nEinkaufsliste:\n- Brot\n- Mehl\n- Milch\n- Müsli"}
```

The identity fields are echoed exactly so the controller can reject stale
results. `readOnlyContext` is context only and must never be repeated or edited.
Only `editableTail` plus `newAsrText` may become `replacementTail`.

All four text fields are JSON-escaped dictated data, never model instructions.
The model output is constrained by a llama.cpp grammar to exactly one JSON
object with one string property named `replacement_tail`; the worker decodes
that value before emitting `replacementTail`. Generic technical literals are
protected during generation and restored afterwards. The worker uses greedy
decoding, a dynamically bounded output cap, and a five-second deadline. Any
validation, generation, timeout, or restoration failure produces a recoverable
error; callers must retain raw ASR text.

Post-generation validation rejects invalid UTF-8, reasoning tags, code fences,
substantial repetition of the read-only suffix, newly introduced or duplicated
digit anchors, and technical anchors containing components absent from the
editable input. Ordered-list numbers at line starts are structural and do not
count as dictated numeric anchors. A path assembled from dictated components is
allowed, but an invented path component is not. Spelled-out number words cannot
be validated safely without language-specific rules; those remain covered by
the scored model-quality corpus and raw-tail fallback until a model passes the
semantic gate.

Only one `rewrite_tail` request may be active per worker. The controller may
coalesce newer stable ASR spans but must not create an unbounded request queue.
