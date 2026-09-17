# Golden-Agent C++

A C++26 port of the Golden-Agent backend from the Python codebase of [Golden-Agent](https://github.com/yashneil75/Golden-Agent?utm_source=chatgpt.com).

Golden-Agent manages the complete lifecycle of a local `llama-server`: downloading models and server binaries with resume and retry support, spawning and supervising the server as a daemon, reaping it through a hookpoint sentinel, and automatically falling back from GPU to CPU when a GPU backend is unavailable.

The project originated as a proof of concept exploring how far a local LLM stack can be pushed by deliberately embracing a **less-is-more** philosophy: fewer assumptions, fewer persistent dependencies, and a tightly controlled execution lifecycle.

### Built by an Amnesiac Agent

This C++ port was produced as the result of a **one-shot agentic task** executed by an agent harness deliberately designed to forget its past.

No accumulated project memory.
No iterative hand-holding.
No historical context carried between runs.

**Amnesia by design. Not a bug. A design choice.**

The result is therefore also an experiment in something beyond the implementation itself: whether an agent, given a sufficiently constrained environment and a well-defined objective, can reconstruct and deliver a complete systems-level component without relying on the continuity of previous interactions.

Golden-Agent C++ is both the resulting implementation and a small proof of that concept.


**The server it runs is the adaptive-KV-streaming build, not stock llama.cpp.**
Stock keeps the whole KV cache in graphics memory, which caps a 27B model at a
small context on a 16 GB card. The
[fork](https://github.com/RaymondHuang210129/llama.cpp-adaptive-kv-streaming)
keeps that cache in ordinary system RAM and holds only the pages it is reading
on the GPU, so the same card runs a six-figure context. It is cloned and built
on first use; if that fails — no CUDA toolchain, no network — the stock binary
is downloaded instead, so you still end up with a working server.

| variable | effect |
|---|---|
| `GA_SERVER_BINARY` | run exactly this `llama-server`, skip the rest |
| `GA_LLAMA_SRC` | where the fork lives (default `~/projects/llama.cpp-adaptive-kv-streaming`) |
| `GA_LLAMA_REPO` | where to clone it from |
| `GA_NO_ADAPTIVE_KV` | set to anything to force the stock download |

Everything below is a **complete, copy-paste-able walkthrough** — every command
is one you can run verbatim, and every output is what you will actually see.

---

## Prerequisites

- Linux (or macOS) with `g++` ≥ 14 (C++26 support)
- `libcurl` development headers
- `zlib`
- `make`

That's it. No Python, no pip, no venv.

```sh
# Debian/Ubuntu if you're missing the deps:
sudo apt install g++ libcurl4-openssl-dev zlib1g-dev make
```

---

## Step 1 — Build

From the repository root:

```sh
make
```

Expected output (first run):

```
g++ -std=c++26 -O2 -Wall -Wextra -Isrc -c src/golden_agent/util.cc -o build/util.o
g++ -std=c++26 -O2 -Wall -Wextra -Isrc -c src/golden_agent/json.cc -o build/json.o
g++ -std=c++26 -O2 -Wall -Wextra -Isrc -c src/golden_agent/http.cc -o build/http.o
g++ -std=c++26 -O2 -Wall -Wextra -Isrc -c src/golden_agent/archive.cc -o build/archive.o
g++ -std=c++26 -O2 -Wall -Wextra -Isrc -c src/golden_agent/download.cc -o build/download.o
g++ -std=c++26 -O2 -Wall -Wextra -Isrc -c src/golden_agent/hookpoint.cc -o build/hookpoint.o
g++ -std=c++26 -O2 -Wall -Wextra -Isrc -c src/golden_agent/backend.cc -o build/backend.o
g++ -std=c++26 -O2 -Wall -Wextra -Isrc -c src/golden_agent/config.cc -o build/config.o
ar rcs build/libga.a build/util.o build/json.o build/http.o build/archive.o build/download.o build/hookpoint.o build/backend.o build/config.o
g++ -std=c++26 -O2 -Wall -Wextra -Isrc -Isrc/golden_agent src/golden_agent/server_daemon.cc build/libga.a -lcurl -lz -lpthread -o build/server_daemon
g++ -std=c++26 -O2 -Wall -Wextra -Isrc -Isrc/golden_agent tests/test_backend.cc build/libga.a -lcurl -lz -lpthread -o build/test_backend
g++ -std=c++26 -O2 -Wall -Wextra -Isrc -Isrc/golden_agent tests/test_download.cc build/libga.a -lcurl -lz -lpthread -o build/test_download
g++ -std=c++26 -O2 -Wall -Wextra -Isrc -Isrc/golden_agent tests/test_http.cc build/libga.a -lcurl -lz -lpthread -o build/test_http
```

What you get in `build/`:

| File              | What it is                                                        |
|-------------------|-------------------------------------------------------------------|
| `libga.a`         | Static library — all the API you can link against                  |
| `server_daemon`   | Standalone daemon binary (spawns/watches `llama-server` for you)   |
| `test_backend`    | 41 backend tests (daemon spawn, stop, hookpoint, GPU→CPU fallback) |
| `test_download`   | 30 download tests (resume, retry, size verification)              |
| `test_http`       | 24 http-layer tests (slist lifetime regression, body-cap
|                       enforcement, 1 MiB streaming)                  |

---

## Step 2 — Run the tests

```sh
make run-tests
```

Expected output (tail):

```
== build/test_backend
...
41 passed, 0 failed
== build/test_download
OK: 30 checks passed
== build/test_http
OK: 24 checks passed
ALL TESTS PASSED
```

All three suites run **without network access** — HTTP is served by a local
in-process TCP listener (an ephemeral port on `127.0.0.1`) and the daemon
is exercised for real. You can run this on a plane.

---

## Step 3 — The minimal example (the whole point)

`examples/minimal.cpp` is the smallest complete program that uses the library.
Read it once — after this you understand the whole system:

```cpp
// examples/minimal.cpp — the minimal complete program.
//
// Build it:
//   make example
//   (equivalently, by hand:)
//   g++ -std=c++26 -O2 -Wall -Wextra -Isrc/golden_agent examples/minimal.cpp \
//       build/libga.a -lcurl -lz -lpthread -o build/minimal
//
// Run it:
//   ./build/minimal
//
// First run downloads two things (takes a while, ~5.6 GB model):
//   1. the llama-server binary  -> ~/.cache/golden-agent/server/llama-server-cpu
//   2. the "lite" model (Ornith 1.5 9B, Q4) -> ~/.cache/golden-agent/models/
//
// Every run after that: everything is cached, it just starts the server.

#include <cstdio>
#include <string>

#include "backend.hpp"
#include "config.hpp"
#include "download.hpp"
#include "http.hpp"

int main() {
    using namespace ga;

    // 1. Get the model. Downloads on first run, cached afterwards.
    //    Returns the local path of the .gguf file.
    std::string model_path = download::ensure_default_model();
    std::printf("model: %s\n", model_path.c_str());

    // 2. Look up the spec for the default model (key: "lite").
    const ModelSpec* spec = model_spec(DEFAULT_MODEL_KEY);
    if (!spec) {
        std::fprintf(stderr, "unknown model key: %s\n", DEFAULT_MODEL_KEY);
        return 1;
    }

    // 3. Start the server. This does the whole lifecycle:
    //    - stops any old server on the port
    //    - makes sure the daemon is running (spawns it detached if not)
    //    - asks the daemon to spawn llama-server with this model
    //    - waits for /health to come up (with GPU -> CPU fallback)
    start_server(*spec, model_path,
                 /*draft_path=*/"",
                 DEFAULT_PORT,      // 2011
                 DEFAULT_HOST,      // "127.0.0.1"
                 Backend::CPU);
    std::printf("server up at %s\n", server_url().c_str());

    // 4. Prove it works: ask the health endpoint.
    HttpResponse health = http::get(server_url() + "/health");
    std::printf("health: HTTP %d %s\n", health.status, health.body.c_str());

    // 5. Ask it a question.
    std::string question = R"({"messages":[{"role":"user","content":"What are you?"}],"stream":false})";
    HttpResponse answer = http::request("POST", server_url() + "/v1/chat/completions",
                                        question,
                                        {{"content-type", "application/json"}},
                                        120);
    std::printf("answer: HTTP %d\n%s\n", answer.status, answer.body.c_str());

    // 6. Shut everything down.
    stop_server();
    std::printf("stopped\n");
    return 0;
}
```

Build and run it:

```sh
make example
./build/minimal
```

First run (model gets downloaded):

```
model: /home/YOU/.cache/golden-agent/models/Ornith-1.5-9B-AD-Q4_K-IQ4_XS.gguf
server up at http://127.0.0.1:2011
health: HTTP 200 {"status":"ok"}
answer: HTTP 200
{"id":"...","choices":[{"message":{"role":"assistant","content":"I am Ornith..."}}],...}
stopped
```

Second run (everything cached — no downloads):

```
model: /home/YOU/.cache/golden-agent/models/Ornith-1.5-9B-AD-Q4_K-IQ4_XS.gguf
server up at http://127.0.0.1:2011
...
```

### Talking to the running server with curl

While the server is up (between `start_server` and `stop_server`), a second
terminal can hit it directly:

```sh
# Health check
curl http://127.0.0.1:2011/health
# -> {"status":"ok"}

# Non-streaming chat completion
curl http://127.0.0.1:2011/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"Say hello in one word."}],"stream":false}'

# Streaming (SSE) — same endpoint, "stream": true, watch chunks arrive
curl -N http://127.0.0.1:2011/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"Count to five."}],"stream":true}'

# List loaded models
curl http://127.0.0.1:2011/v1/models
```

---

## Step 4 — How the daemon actually works (and running it by hand)

You normally never touch the daemon: `start_server()` calls `ensure_daemon()`
which spawns it detached if no instance is alive. But here's the full picture,
because that's what makes it robust:

```
your program
   │  start_server()
   ▼
ensure_daemon()  ──spawns detached──►  build/server_daemon
                                          │  listens on 127.0.0.1:2012
                                          │  (newline-terminated JSON commands)
                                          ▼
                                    {"cmd":"START","port":2011,
                                     "hookpoint_pid":12345,
                                     "server_args":[...]}
                                          │
                                          ▼
                                    forks:  llama-server  +  monitor co-process
                                              │
                                              └──► watches the hookpoint sentinel;
                                                  when the sentinel dies,
                                                  the server is reaped
```

- **One daemon per runtime dir** — enforced by `daemon.pid`. A second spawn
  sees the live pid file and exits 0 immediately.
- **`monitor` mode** — the same binary, same argparse, run as a child of
  `server_daemon` to watch the hookpoint sentinel.
- **Hookpoint sentinel** — a dummy process whose death means "the real owner
  is gone"; the monitor kills the server in that case.

Run the daemon by hand, if you want to. Every command below is the
complete, real thing — copy-paste it and it works:

```sh
# 1. start the daemon in the foreground (Ctrl-C to stop)
./build/server_daemon --control-port 2012 --runtime-dir ~/.cache/golden-agent/server

# 2. in another terminal: ping it
printf '{"cmd":"PING"}\n' | nc 127.0.0.1 2012
# -> {"status":"ok"}

# 3. start a llama-server session. `server_args` is the exact argv the
#    daemon will fork — first element is the server binary itself.
printf '%s\n' '{
  "cmd": "START",
  "host": "127.0.0.1",
  "port": 2011,
  "hookpoint_pid": 4321,
  "server_args": [
    "/home/alca/.cache/golden-agent/server/llama-server-cpu",
    "-m", "/home/alca/.cache/golden-agent/models/Ornith-1.5-9B-AD-Q4_K-IQ4_XS.gguf",
    "--host", "127.0.0.1",
    "--port", "2011",
    "-fa", "on",
    "-ctk", "q5_0",
    "-ctv", "q5_0",
    "-c", "4096"
  ]
}' | nc 127.0.0.1 2012
# -> {"status":"ok","server_pid":12345}

# 4. stop that session
printf '{"cmd":"STOP","port":2011}\n' | nc 127.0.0.1 2012
# -> {"status":"ok"}
```

The full command set is `PING`, `START`, `STOP`. The daemon reads the
`cmd` field. `START` payload fields: `port`, `hookpoint_pid`,
`server_args` (array of argv strings).

---

## Where everything lands on disk

| Path (Linux)                                          | What                    |
|-------------------------------------------------------|-------------------------|
| `~/.cache/golden-agent/models/`                        | Downloaded `.gguf` models |
| `~/.cache/golden-agent/server/llama-server-cpu`        | The server binary itself  |
| `~/.cache/golden-agent/server/daemon.pid`              | Daemon liveness file      |

On macOS: `~/Library/Caches/golden-agent/…`; on Windows:
`%LOCALAPPDATA%/golden-agent/…`. Override with `XDG_CACHE_HOME` (Linux) /
`HOME` (macOS) / `LOCALAPPDATA` (Windows).

---

## Model catalogue

What `model_spec(key)` knows about:

| key   | Model                          | Repo (HuggingFace)                  | File downloaded                        | Size  | Notes |
|-------|--------------------------------|-------------------------------------|----------------------------------------|-------|-------|
| `tiny`| Liquid LFM2.5 2.6B             | `LiquidAI/LFM2.5-2.6B-GGUF`          | `LFM2.5-2.6B-QAD-Q4_0.gguf`           | 1.6 GB | fastest |
| `lite`| Ornith 1.5 9B (**default**)    | `AtomicChat/Ornith-1.5-9B-GGUF`      | `Ornith-1.5-9B-AD-Q4_K-IQ4_XS.gguf`   | 5.6 GB | has a DFlash draft model for speculative decoding |
| `pro` | Qwen3.8 27B                    | `sdkyuan/qwen3.8-27B-qat-q2_0-gguf`  | `qwen38-27b-qat-q2_0.gguf`            | 8.8 GB | strongest; also has a DFlash2 draft model |

`DEFAULT_MODEL_KEY = "lite"` — that's what `ensure_default_model()` fetches.

To use a different model, look up its spec and pass the downloaded path:

```cpp
const ModelSpec* spec = model_spec("tiny");
std::string path = ga::download::ensure_model_downloaded(
    /*dest_dir=*/"",        // "" -> ~/.cache/golden-agent/models
    /*opener=*/{},          // {} -> default libcurl opener
    spec->repo, spec->filename, spec->size_bytes);
start_server(*spec, path, /*draft_path=*/"", 2011, "127.0.0.1", Backend::CPU);
```

---

## The API (the four things you actually call)

```cpp
// download.hpp — fetch a model; cached after the first run
std::string ga::download::ensure_default_model();                       // "lite"
std::string ga::download::ensure_model_downloaded(dest_dir, opener, repo,
                                                  filename, expected_size, ...);

// backend.hpp — the whole lifecycle
void      ga::start_server(const ModelSpec& spec, const std::string& target_path,
                           const std::string& draft_path, int port,
                           const std::string& host, Backend backend);
void      ga::stop_server();
std::string ga::server_url(int port, const std::string& host);  // "http://127.0.0.1:2011"

// http.hpp — talk to it
HttpResponse ga::http::get(url);
HttpResponse ga::http::request(method, url, body, headers, timeout_sec, max_body);
// HttpResponse has .status (int) and .body (std::string)
//
// Error contract:
//   HttpError e — transport/HTTP errors (e.status is the HTTP code when the
//                 server answered, 0 when it was a network failure)
//   BodyLimitExceeded (a subclass of HttpError) — the response body exceeded
//                 max_body; thrown instead of silently truncating
//   Streaming (ga::http::curl_reader): cap violations surface as
//                 reader->failed() + cap_exceeded() + error(), so callers can
//                 distinguish "policy" failures (no retry helps) from
//                 "transport" failures (retry).
```

Link against it: `build/libga.a -lcurl -lz -lpthread` (plus `-Isrc/golden_agent`
for the headers).

---

## Gotcha: the libcurl header callback

If you ever read `http.cc` and see the `CURLOPT_HEADERFUNCTION` callback
returning `size * nmemb`, do **not** "fix" it to return `0` — that is the
correct contract. libcurl's header callback must return the number of bytes
consumed (`size * nmemb`); returning 0 makes libcurl think nothing was read.
This exact bug bit the port once, and the tests in `test_download.cc` guard
it.

Two more lifetime/policy gotchas in the same file, both regression-tested by
`test_http.cc`:

- The header `curl_slist` must outlive `curl_easy_perform`. It is owned by
  the RAII `CurlReq` struct (freed in its destructor, *after* the transfer),
  never freed by hand mid-request.
- The write callbacks enforce the body cap by returning `0` (not the number
  of bytes "processed") when the cap is hit. libcurl then aborts with
  `CURLE_WRITE_ERROR`, and the layer converts that into
  `BodyLimitExceeded` / `cap_exceeded()` instead of silently truncating.
