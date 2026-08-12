# Worker JSONL protocol

Protocol version: `1`

## Common rules

- UTF-8 JSON Lines over stdin/stdout.
- One object per line and a maximum line size of 1 MiB.
- Every message has `v: 1` and a non-empty `type`.
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

Commands:

```json
{"v":1,"type":"rewrite","id":"request-6","requestId":"rewrite-1","language":"de","text":"Ich will äh das Modell ändern."}
```

Result:

```json
{"v":1,"type":"rewrite_completed","seq":4,"id":"request-6","requestId":"rewrite-1","text":"Ich will das Modell ändern."}
```

The worker uses a constrained built-in cleanup instruction. Product code must
not treat dictated text as instructions to the model.

Explicit spoken corrections and formatting commands are normalized before
inference. Technical literals are protected during generation and restored
unchanged so the model cannot fabricate a plausible replacement identifier or
path.

`language` is the source and required output language. If a generated result
clearly changes that language, the worker retries with a stricter constraint
and rejects a second mismatch instead of returning a translation.
