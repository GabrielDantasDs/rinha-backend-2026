This repository implements a fraud detection HTTP API in C using HNSW (Hierarchical Navigable Small World) for nearest-neighbor search. The guidance below tells an AI coding agent how the project is structured, how to build and run it locally, and what conventions to follow when making changes.

Key facts (big picture)
- HTTP server (`src/main.c`) listening on port 8080 with two main endpoints: `/ready` (health check) and `/fraud-score` (fraud detection).
- Fraud detection pipeline: request → vector normalization (14 dimensions, all [0, 1] range) → HNSW search → nearest-neighbor classification.
- Distance metric: squared Euclidean distance (not sqrt) for both index building (`src/hnsw.c`) and searching (`src/hnsw_search.c`). **Critical**: both files must use the same distance function.
- HNSW index stored in binary format (`hnsw_index.bin`) with header + node array; loaded via mmap in `handle_request`.
- Normalization constants loaded from `normalization_constants.json` at runtime (defaults hardcoded in `src/normalization.c`).

How to build and run (developer workflows)
- Build (local, macOS/linux):

  gcc -o rinha src/main.c src/hnsw_search.c src/hnsw.c src/normalization.c src/pre-proccess.c -lm -Wall -std=c11

- Build index (preprocess training data and build HNSW):

  ./rinha_build  # (must implement main() in src/hnsw.c to accept input file and output index)

- Run server (foreground):

  ./rinha

- Quick smoke test (from another terminal):

  curl http://localhost:8080/ready
  curl -X POST http://localhost:8080/fraud-score -d @transaction.json

What you'll find in key source files (patterns to follow)
- `src/main.c`: HTTP server (POSIX sockets), request parsing, and `/fraud-score` endpoint logic.
  - network: uses POSIX sockets (socket(), bind(), listen(), accept(), recv(), write(), close()). Keep changes portable.
  - request parsing: reads entire request into buffer, uses sscanf(). Fragile; don't assume robust HTTP parsing.
  - response construction: writes raw HTTP strings. Preserve CRLF and headers when modifying.
  - `/fraud-score` handler: loads binary HNSW index via mmap, calls `create_vector_from_request()`, runs search, counts fraud neighbors, returns JSON response.

- `src/hnsw_search.c`: HNSW search functions. **Critical**: uses squared Euclidean distance (no sqrt). Must match `src/hnsw.c`.
  - `dist2(a, b)`: returns sum of squared differences, not sqrt. Ranges from 0 to 14 for normalized [0,1] vectors.
  - `greedy_search_layer()`: layer-wise greedy descent. Validates inputs and neighbor indices; returns -1 on error and prints to stdout.
  - `search()`: top-level search. Returns top 5 nearest neighbors in `idx_out` and `dist_out`. Returns 0 on success, -1 on error.

- `src/hnsw.c`: Index builder. Also uses squared Euclidean distance (must match search).
  - `build_hnsw()`: reads preprocessed binary data, builds HNSW tree, writes `hnsw_index.bin`.
  - Same `dist2()` function as search.c to ensure consistency.

- `src/normalization.c`: Vector normalization and request parsing.
  - `create_vector_from_request()`: parses JSON-like request, extracts 14 fields, normalizes them, fills vector[0..13].
  - `normalize_vector()`: enforces [0, 1] clamps; special case: indices 5, 6 may be -1 (sentinel for missing last transaction).
  - Constants loaded from `normalization_constants.json`; defaults hardcoded.

- `src/pre-proccess.c`: Preprocesses CSV training data into binary format.
  - `preprocess_data()`: reads CSV, writes binary item_t structs (14 floats + 1 uint8_t label per row).

Project-specific conventions
- Keep changes minimal and explicit. The repository demonstrates a tiny example; prefer clarity over clever shortcuts.
- Avoid introducing heavy external dependencies or complex build infra unless the change clearly requires it. If adding a Makefile or CMake, include simple targets: `build`, `clean`, `run` and update this file.

Integration points and external dependencies
- None external — the binary depends only on the system C library and POSIX socket APIs.
- If you add libraries (libevent, openssl), document why and include build instructions.

Examples for common tasks
- Rebuild HNSW index after changing the distance metric or normalization:

  1. Ensure both `src/hnsw.c` and `src/hnsw_search.c` use identical `dist2()` functions (squared Euclidean, no sqrt).
  2. Run the builder: compile with `gcc -o rinha_build src/hnsw.c src/pre-proccess.c -lm` and execute it with training data.
  3. This overwrites `hnsw_index.bin`. Always rebuild when changing distance metric or vector normalization.

- Add a new normalization constant:

  1. Edit `normalization_constants.json` or add a default in `src/normalization.c` (MAX_* variables).
  2. Update `load_constants_once()` to parse the new key.
  3. Use the constant in `create_vector_from_request()` to normalize a new dimension.

Testing and validation guidance
- There are no automated tests in the repo. For any change, run the build command above and manually exercise endpoints with `curl`.
- When editing `src/hnsw_search.c` or `src/hnsw.c`, ensure the `dist2()` function is identical in both files. If you change the distance metric (e.g., add sqrt or change formula), you must rebuild the index.
- Distance debug output: `src/normalization.c` prints normalized vector dimensions to stderr; `src/hnsw_search.c` prints error messages to stdout on validation failures.

Style and commits
- Keep commits small and focused. Use descriptive commit messages: "add /version endpoint" or "make server multi-threaded".

Files to open first
- `main.c` — single source of truth for behavior.

If you modify build or CI
- Add a simple `Makefile` and document usage here. Keep CI minimal: build and run the binary (no test harnesses required).

When to ask the human
- If you plan to add external dependencies or a new build system, ask for confirmation before proceeding.

If anything here is unclear or you'd like more detail (Makefile, CI, or a thread-pool example), tell me what you want updated and I'll extend this file.
