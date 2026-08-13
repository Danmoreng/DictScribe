# Semantic cleanup model benchmark

Date: 2026-08-13

This is the first Windows benchmark for the semantic incremental cleanup plan.
It is a development result, not a model-selection decision.

## Environment

- CPU: AMD Ryzen 9 9955HX3D, 16 cores / 32 logical processors
- rewrite threads: 4
- context: 2,048 tokens
- decoding: greedy, grammar-constrained one-field JSON
- hard timeout: 5 seconds
- worker: pinned repository llama.cpp revision
- primary candidate: `ggml-org/Qwen3.5-0.8B-GGUF`, Q8_0
- comparison: `unsloth/Qwen3.5-2B-GGUF`, Q8_0

Both files report the `qwen35` architecture and expose a GGUF chat template.
Neither run leaked `<think>` content.

## Prompt variants

`P0`, the zero-shot semantic prompt, was fast with the legacy unconstrained
output path, but did not reliably interpret lists, paragraph requests, or
dictated technical separators.

`P1` adds one short German list example and one short English
correction/paragraph example. With strict JSON grammar, Qwen3.5-0.8B completed
the five seed cases in approximately 1.8 to 3.0 seconds. It produced the German
shopping list correctly and preserved no-op prose. It failed the other seed
requirements:

- the replacement value `achthundert Megabyte` became `achtzehn Megabyte`;
- the English three-step request remained inline prose instead of an ordered
  list;
- the dictated Windows path was not assembled from the spoken separators.

`P2` added a third short Spanish ordered-list example. It did not improve the
held-out German correction, English list, or technical path cases, so the
shorter `P1` prompt remains the current source candidate.

## 2B comparison

Under the same P1 contract, Qwen3.5-2B Q8_0 completed the cases in approximately
2.7 to 4.2 seconds. It improved the English request to a correct numbered list,
but still failed two safety-critical cases:

- `achthundert` was split into `ach, hundert`, leaving the superseded value in
  the output;
- the spoken technical path was not converted to the intended path.

A follow-up generic instruction about compound replacement values and dictated
technical separators regressed list behavior in both models and was rejected.

## Automated quality gate

The seed scenarios now live in
`tests/data/rewrite_tail_benchmark.json`. The benchmark runner evaluates exact
or normalized preservation, required and forbidden content, paragraph breaks,
ordered and unordered list shapes, technical-path structure, prompt-injection
handling, and incremental list continuation. It exits non-zero when any case
fails and can write a machine-readable JSON report.

The first scored Qwen3.5-0.8B Q8_0 run passed 3 of 7 cases:

- passed: German shopping list, no-op prose, prompt-injection-as-content;
- failed: German correction/paragraph, English ordered list, dictated technical
  path, incremental list continuation.

The worker now also applies a model-independent safety validator before
returning a tail. It rejects invalid UTF-8, model commentary, substantial
read-only suffix repetition, new or duplicated digit anchors, and technical
anchor components absent from editable input. These failures are recoverable,
so the controller retains raw ASR text. Language-independent validation cannot
prove that one spelled-out number word was not changed into another; the scored
correction case therefore remains a required model-quality gate.

## Current decision

Runtime compatibility and CPU latency are good enough to continue developing
the bounded protocol and model-independent validation. Neither Q8_0 model is
yet approved for automatic semantic cleanup. In particular, the application
must not commit a rewrite that changes a corrected numeric value or loses
technical anchors.

Before the Phase-3 controller migration becomes the active default:

1. improve the failing scored cases without adding language-specific command
   rules;
2. decide whether prompt work is sufficient or a task-specific LoRA/SFT or a
   different small Apache-2.0 model is required.

Q8_0 remains the development quantization for the current implementation
milestones. A Q4_0 comparison is deferred until a later performance and
packaging optimization milestone; it is not a prerequisite for the bounded
controller migration.

Run the current manual smoke corpus with:

```powershell
python scripts/smoke-rewrite-model.py `
  --worker build/rewrite-worker/bin/dictscribe-rewrite-worker.exe `
  --model C:/path/to/Qwen3.5-0.8B-Q8_0.gguf `
  --json-report build/rewrite-q8-quality-report.json
```
