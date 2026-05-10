This repository is a minimal C HTTP server demo. The guidance below tells an AI coding agent how the project is structured, how to build and run it locally, and what conventions to follow when making changes.

Key facts (big picture)
- Single executable C program at `main.c` that implements a tiny blocking HTTP server listening on port 8080.
- No build system or project metadata found in the repo root (no Makefile, no CMakeLists.txt). The canonical compile command is a single gcc invocation.
- The server handles simple GET-style requests by parsing the request line and matching the path for `/` and `/health`.

How to build and run (developer workflows)
- Build (local, macOS/linux):

  gcc -o rinha main.c

- Run (foreground):

  ./rinha

- Quick smoke test (from another terminal):

  curl http://localhost:8080/
  curl http://localhost:8080/health

What you'll find in `main.c` (patterns to follow)
- network: uses POSIX sockets (socket(), bind(), listen(), accept(), recv(), write(), close()). Keep changes portable to POSIX APIs.
- request parsing: very small and fragile — it reads the entire request into a buffer and uses sscanf(buffer, "%s %s", method, path). Don't assume robust parsing or HTTP compliance.
- response construction: the code writes raw HTTP responses as strings. When modifying responses, keep newlines and headers exact (CRLF sequences are used in examples).
- concurrency: the server is single-threaded and blocking (one connection at a time). If adding concurrency, update README and test instructions.

Project-specific conventions
- Keep changes minimal and explicit. The repository demonstrates a tiny example; prefer clarity over clever shortcuts.
- Avoid introducing heavy external dependencies or complex build infra unless the change clearly requires it. If adding a Makefile or CMake, include simple targets: `build`, `clean`, `run` and update this file.

Integration points and external dependencies
- None external — the binary depends only on the system C library and POSIX socket APIs.
- If you add libraries (libevent, openssl), document why and include build instructions.

Examples for common tasks
- Add a JSON endpoint `/version` that returns `{ "version": "0.1" }`:

  1. Locate the path handling in `handle_request`.
  2. Add an else-if branch matching `strcmp(path, "/version") == 0` and write the JSON response similar to `/health`.

- Make the server multi-connection capable (simple approach):

  1. Wrap `accept()`+`handle_request()` in a thread (pthread) or fork a process. Keep the API usage to POSIX libraries.
  2. Ensure sockets are closed in both parent and child; update cleanup.
  3. Add a simple sanity check in the README or this file explaining how to test concurrency (multiple curl requests in parallel).

Testing and validation guidance
- There are no automated tests in the repo. For any change, run the build command above and manually exercise endpoints with `curl`.
- When editing `main.c`, ensure there are no obvious memory or buffer issues (fixed-size buffers for method/path). If you change buffer sizes, update BUFFER_SIZE constant and validate with long-path requests.

Style and commits
- Keep commits small and focused. Use descriptive commit messages: "add /version endpoint" or "make server multi-threaded".

Files to open first
- `main.c` — single source of truth for behavior.

If you modify build or CI
- Add a simple `Makefile` and document usage here. Keep CI minimal: build and run the binary (no test harnesses required).

When to ask the human
- If you plan to add external dependencies or a new build system, ask for confirmation before proceeding.

If anything here is unclear or you'd like more detail (Makefile, CI, or a thread-pool example), tell me what you want updated and I'll extend this file.
