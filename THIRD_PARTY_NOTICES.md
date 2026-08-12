# Third-party notices

DictScribe is Apache-2.0 licensed. Its source checkout contains pinned Git
submodules whose code remains under the respective upstream licenses.

- **NVIDIA NeMo-Speech.cpp** — Apache-2.0. Preserve its `NOTICE` and
  `THIRD_PARTY_NOTICES.md` files in binary distributions.
- **GGML used by NeMo-Speech.cpp** — MIT; pinned by the NeMo submodule.
- **llama.cpp and its GGML copy** — MIT.
- **miniaudio** — MIT-0.
- **JSON for Modern C++ (nlohmann/json)** — MIT.
- **SentencePiece** — Apache-2.0; built as the compatibility revision selected
  by NeMo-Speech.cpp.

Nemotron and rewrite model files are downloaded separately and retain their own
model licenses. A model's code-library license must not be assumed to cover the
model artifact.
