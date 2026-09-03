// Port of Golden-Agent/src/golden_agent/backend.py
// (537-line snapshot, 2026-09-02). Manages the llama-server process lifecycle
// through the server daemon: hookpoint sentinel, daemon socket protocol,
// server spawn, health polling, and CPU-fallback restart.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "config.hpp"
#include "json.hpp"

namespace ga {

// backend.py line 12 — control port the server daemon listens on.
constexpr int CONTROL_PORT = 2012;
// backend.py line 13 — -ngl value used for GPU backends.
constexpr int DEFAULT_NGL_LAYERS = 99;

// backend.py LogFn = Callable[[str], None]
using LogFn = std::function<void(const std::string&)>;

// Module state — backend.py globals _server_pid / _server_port /
// _hookpoint_process. 0 means None.
struct ServerState {
    int server_pid = 0;
    int server_port = 0;
    int hookpoint_pid = 0;
};

// Mutable access to the module globals (start/stop mutate; tests inspect).
ServerState& backend_state();

// _build_server_args(spec, target_path, draft_path="", port, host, backend, inference=None)
std::vector<std::string> build_server_args(const ModelSpec& spec,
                                           const std::string& target_path,
                                           const std::string& draft_path = "",
                                           int port = DEFAULT_PORT,
                                           const std::string& host = DEFAULT_HOST,
                                           Backend backend = Backend::CPU,
                                           const json::Value* inference = nullptr);

// _cache_dir() — server-binary cache root (per-OS like Python).
std::string cache_dir();

// _server_binary_path(backend)
std::string server_binary_path(Backend backend = Backend::CPU);

// _ensure_server_binary(backend, log) — download + extract if missing.
std::string ensure_server_binary(Backend backend = Backend::CPU, LogFn log = nullptr);

// _wait_for_server(host, port, timeout=120)
bool wait_for_server(const std::string& host, int port, double timeout_sec = 120.0);

// _send_to_daemon(payload, retries=25, retry_delay=0.1) — throws std::runtime_error
// after exhausting retries ("server daemon unreachable: ...").
json::Value send_to_daemon(const json::Value& payload, int retries = 25,
                           double retry_delay_sec = 0.1);

// _ensure_daemon() — spawn the server_daemon binary detached if none is alive.
void ensure_daemon();

// start_server(...) — full lifecycle: stop old, hookpoint, daemon, spawn,
// health-wait, GPU->CPU fallback.
void start_server(const ModelSpec& spec, const std::string& target_path,
                   const std::string& draft_path = "", int port = DEFAULT_PORT,
                   const std::string& host = DEFAULT_HOST,
                   Backend backend = Backend::CPU, LogFn log = nullptr);

// stop_server() — daemon STOP, fallback port-kill, hookpoint kill, state clear.
void stop_server();

// server_url(port, host)
std::string server_url(int port = DEFAULT_PORT, const std::string& host = DEFAULT_HOST);

}  // namespace ga
