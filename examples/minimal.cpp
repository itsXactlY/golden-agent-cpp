// examples/minimal.cpp — the smallest complete program you can build against
// libga. This is exactly what a real agent backend entry point looks like.
//
// Build (from the repo root, after 'make'):
//
//   g++ -std=c++26 -O2 -Wall -Wextra -Isrc/golden_agent
//       examples/minimal.cpp build/libga.a -lcurl -lz -lpthread
//       -o build/minimal
//
// Run:
//
//   ./build/minimal
//
// First run downloads two things (then caches them forever):
//   1. the llama-server binary   -> ~/.cache/golden-agent/server/llama-server-cpu
//   2. the default model "lite"  -> ~/.cache/golden-agent/models/
//      (Ornith 1.5 9B, ~5.6 GB, from Hugging Face)
//
// Every later run skips both downloads and just starts the server.

#include <cstdio>
#include <string>

#include "backend.hpp"
#include "config.hpp"
#include "download.hpp"
#include "http.hpp"

int main() {
    // 1. Make sure the model exists locally.
    //    Downloads it on first run (resumable, progress on stderr);
    //    returns the path to the .gguf file.
    std::string model_path = ga::download::ensure_default_model();
    std::printf("[1] model ready: %s\n", model_path.c_str());

    // 2. Look up its spec (the "lite" catalogue entry: n_ctx, draft model,
    //    sampling overrides, ...). model_spec() returns nullptr if unknown.
    const ga::ModelSpec* spec = ga::model_spec(ga::DEFAULT_MODEL_KEY);
    if (spec == nullptr) {
        std::fprintf(stderr, "unknown model key: %s\n", ga::DEFAULT_MODEL_KEY);
        return 1;
    }

    // 3. Start the server. One call does the whole lifecycle:
    //      - stops any old server still bound to the port,
    //      - makes sure the supervisor daemon is alive (port 2012),
    //      - downloads the llama-server binary if missing,
    //      - spawns llama-server with the model (CPU backend),
    //      - waits for http://127.0.0.1:2011/health to answer,
    //      - falls back GPU -> CPU if the GPU load fails.
    ga::start_server(
        *spec, model_path,         // model + its spec
        std::string(""),           // draft_path: empty unless you want spec-decode
        ga::DEFAULT_PORT,          // 2011
        ga::DEFAULT_HOST,          // "127.0.0.1"
        ga::Backend::CPU);
    std::printf("[2] server up at %s\n", ga::server_url().c_str());

    // 4. Check health the way an agent would before sending prompts.
    ga::http::HttpResponse health = ga::http::get(ga::server_url() + "/health");
    std::printf("[3] health: %d %s\n", health.status, health.body.c_str());

    // 5. Ask it one question (OpenAI-compatible endpoint).
    std::string prompt =
        "{\"messages\":[{\"role\":\"user\",\"content\":\"Say hi in one word.\"}],"
        "\"stream\":false}";
    ga::http::HttpResponse resp = ga::http::request(
        "POST", ga::server_url() + "/v1/chat/completions",
        prompt, {{"content-type", "application/json"}}, 120);
    std::printf("[4] completion (status %d):\n%s\n", resp.status, resp.body.c_str());

    // 6. Shut everything down: daemon STOP, hookpoint cleanup, state cleared.
    ga::stop_server();
    std::printf("[5] server stopped\n");
    return 0;
}
