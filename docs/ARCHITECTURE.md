# Architecture

DictScribe uses one lightweight desktop controller, one persistent ASR worker,
and an optional lazily loaded rewrite worker. Process isolation is a correctness
boundary, not a network-service architecture.

```text
Desktop controller
  |-- stdin/stdout JSONL --> ASR worker
  |                         |-- miniaudio microphone capture
  |                         `-- NeMo-Speech.cpp SDK + NeMo GGML
  `-- stdin/stdout JSONL --> rewrite worker
                            `-- llama.cpp + llama GGML
```

## Why the workers are separate

NeMo-Speech.cpp and llama.cpp pin independent GGML revisions. Their dynamic
libraries use overlapping filenames and exported symbol names, while their ABIs
evolve independently. Loading both into one process would make correctness
depend on platform loader behavior, link ordering, visibility patches, and CUDA
backend compatibility.

Each worker is therefore built in its own CMake tree:

- NeMo is configured as a top-level project and installed as its official SDK.
  The ASR worker links only the installed `NeMoSpeech::ASR` C ABI.
- llama.cpp is added only to the rewrite-worker project and linked statically.
- The future UI/controller links neither inference library.

This also provides independent crash recovery, explicit model lifetimes, and
separate CPU/GPU placement without opening a local port.

## Runtime ownership

The ASR worker owns microphone capture and emits cumulative transcript updates.
No audio is persisted or transferred to the controller. The rewrite worker owns
one loaded GGUF model and accepts text-only rewrite jobs. Standard output is
reserved for protocol messages; diagnostics use standard error.

The controller stabilizes cumulative ASR hypotheses into immutable spans. It
owns frozen output, a bounded editable tail, a stable raw backlog, and the
current unstable ASR suffix. Rewrite requests contain at most a short read-only
suffix, 192 approximate editable tokens, and 128 approximate new-ASR tokens,
with independent byte limits. At most one rewrite is active and one newer state
is coalesced, so slow inference cannot create an unbounded queue.

Stopping never waits for cleanup and there is no final whole-transcript pass.
The controller composes accepted cleanup with all remaining raw text
immediately. `Off` is the default cleanup mode and does not discover, start, or
load the rewrite worker. Enabling `AI cleanup` starts it on demand; every rewrite
failure falls open to raw ASR without affecting speech recognition.
