#include "backend.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "archive.hpp"
#include "config.hpp"
#include "http.hpp"
#include "json.hpp"
#include "util.hpp"

namespace ga {

// ---------------------------------------------------------------------------
// Module state (backend.py globals)
// ---------------------------------------------------------------------------
static ServerState g_state;

ServerState& backend_state() { return g_state; }

static void default_log(const std::string& msg) {
    std::fputs(msg.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Directory of the running executable — the C++ stand-in for
// Path(__file__).resolve(). The hookpoint and server_daemon binaries are
// shipped alongside the agent binary.
static std::string exe_dir() {
    std::error_code ec;
    std::filesystem::path p;
#ifdef _WIN32
    char buf[4096] = {};
    DWORD n = ::GetModuleFileNameA(nullptr, buf, sizeof buf - 1);
    p = std::filesystem::path(buf, buf + n);
#else
    p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) p = std::filesystem::current_path();
#endif
    const std::filesystem::path pp = p.parent_path();
    return pp.empty() ? std::string(".") : pp.string();
}

static bool debug_flag() {
    auto d = env_get("GOLDEN_AGENT_DEBUG");
    return d.has_value() && !d->empty();
}

// Plain (non-detached) fork/exec: direct child of the caller, stdio to
// /dev/null. Mirrors subprocess.Popen([cmd], stdin/out/err=DEVNULL) used by
// backend.py for the hookpoint sentinel and by server_daemon.py for the
// server + monitor processes.
static pid_t spawn_child(const std::vector<std::string>& args) {
    if (args.empty()) return 0;
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2) close(devnull);
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    return pid;
}

// ---------------------------------------------------------------------------
// _cache_dir / _server_binary_path / _ensure_server_binary
// ---------------------------------------------------------------------------
std::string cache_dir() {
    std::string base;
#ifdef _WIN32
    auto local = env_get("LOCALAPPDATA");
    base = local.has_value() ? *local : path_join(home_dir(), "AppData", "Local");
#elif defined(__APPLE__)
    base = path_join(home_dir(), "Library", "Caches");
#else
    auto xdg = env_get("XDG_CACHE_HOME");
    base = xdg.has_value() ? *xdg : path_join(home_dir(), ".cache");
#endif
    return path_join(path_join(base, "golden-agent"), "server");
}

// Run a command to completion with its output on our stdout/stderr, so a long
// build shows progress instead of looking hung. Returns the exit status, or
// -1 when the child could not be started.
static int run_and_wait(const std::vector<std::string>& args) {
    if (args.empty()) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static std::string path_dirname(const std::string& p) {
    auto pos = p.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return p.substr(0, pos);
}

// ---------------------------------------------------------------------------
// Adaptive KV streaming
//
// Stock llama.cpp keeps the whole KV cache in VRAM, which caps a 27B model at a
// small context on a 16 GB card. The adaptive-KV fork keeps the authoritative
// tensors in pinned host memory and only a bounded pool of pages on the GPU, so
// the same card holds a six-figure context. That is the difference between "a
// local model" and "a local model you can actually work with", so this is where
// the server comes from when it is available.
//
//   GA_SERVER_BINARY  use exactly this llama-server, skip everything below
//   GA_LLAMA_SRC      where the fork lives (default ~/projects/llama.cpp-adaptive-kv-streaming)
//   GA_LLAMA_REPO     where to clone it from
//   GA_NO_ADAPTIVE_KV set to any value to force the stock download
// ---------------------------------------------------------------------------

static std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

static std::string adaptive_kv_src() {
    return path_expand_user(env_or("GA_LLAMA_SRC",
                                   "~/projects/llama.cpp-adaptive-kv-streaming"));
}

static std::string adaptive_kv_binary() {
    return path_join(path_join(adaptive_kv_src(), "build"), "bin/llama-server");
}

// Clone and build the fork. Returns the binary path, or "" when it could not be
// produced -- the caller then falls back to the stock download rather than
// leaving the user with nothing.
static std::string build_adaptive_kv(LogFn log) {
    LogFn logger = log ? log : default_log;
    const std::string src  = adaptive_kv_src();
    const std::string repo = env_or("GA_LLAMA_REPO",
        "https://github.com/RaymondHuang210129/llama.cpp-adaptive-kv-streaming");

    if (!dir_exists(path_join(src, ".git"))) {
        logger("cloning adaptive-KV llama.cpp into " + src);
        make_dirs(path_dirname(src));
        int rc = run_and_wait({"git", "clone", "--depth", "1", "--branch",
                               "feature/adaptive-kv-stream", repo, src});
        if (rc != 0) {
            logger("clone failed (rc=" + std::to_string(rc) + ") — using stock llama.cpp");
            return {};
        }
    }

    logger("building adaptive-KV llama-server (this takes a while)");
    int rc = run_and_wait({"cmake", "-S", src, "-B", path_join(src, "build"),
                           "-DGGML_CUDA=ON", "-DGGML_CUDA_FA_ALL_QUANTS=ON",
                           "-DCMAKE_BUILD_TYPE=Release"});
    if (rc == 0) {
        rc = run_and_wait({"cmake", "--build", path_join(src, "build"),
                           "--config", "Release", "--target", "llama-server", "-j"});
    }
    if (rc != 0 || !file_exists(adaptive_kv_binary())) {
        logger("adaptive-KV build failed (rc=" + std::to_string(rc) +
               ") — using stock llama.cpp");
        return {};
    }
    logger("adaptive-KV llama-server ready");
    return adaptive_kv_binary();
}

std::string server_binary_path(Backend backend) {
    const char* forced = std::getenv("GA_SERVER_BINARY");
    if (forced && *forced) return path_expand_user(forced);
    if (!std::getenv("GA_NO_ADAPTIVE_KV") && file_exists(adaptive_kv_binary())) {
        return adaptive_kv_binary();
    }
    return server_binary_path_stock(backend);
}

std::string server_binary_path_stock(Backend backend) {
    std::string name = backend_value(backend);
#ifdef _WIN32
    return path_join(cache_dir(), "llama-" + name + ".exe");
#else
    return path_join(cache_dir(), "llama-server-" + name);
#endif
}

std::string ensure_server_binary(Backend backend, LogFn log) {
    LogFn logger = log ? log : default_log;
    std::string binary = server_binary_path(backend);
    if (file_exists(binary)) return binary;

    // Prefer the fork. Only when it cannot be produced do we fetch stock, so a
    // machine without a CUDA toolchain still ends up with a working server.
    if (!std::getenv("GA_SERVER_BINARY") && !std::getenv("GA_NO_ADAPTIVE_KV")) {
        std::string forked = build_adaptive_kv(logger);
        if (!forked.empty()) return forked;
        binary = server_binary_path_stock(backend);
        if (file_exists(binary)) return binary;
    }

    make_dirs(cache_dir());
    std::string url = server_download_url("", backend, "");
    logger("downloading llama-server " + url);

    std::string body;
    try {
        auto resp = http::request("GET", url, "", {}, 120, SIZE_MAX);
        body = std::move(resp.body);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("failed to download llama-server: ") + e.what());
    }

    bool found = false;
#ifdef _WIN32
    auto members = archive::zip_members(body);
    for (const auto& m : members) {
        if (!m.is_file) continue;
        std::string base = path_basename(m.name);
        if (ends_with(base, ".dll")) {
            write_file_text(path_join(cache_dir(), base), m.data);
        } else if (ends_with(base, ".exe")) {
            binary = path_join(cache_dir(), base);
            write_file_text(binary, m.data);
            found = true;
        }
    }
#else
    std::string tar = archive::decompress_stream(body);
    auto members = archive::tar_members(tar);
    for (const auto& m : members) {
        if (!m.is_file) continue;
        std::string base = path_basename(m.name);
        if (base.find("llama-server") != std::string::npos || starts_with(base, "llama-")) {
            binary = path_join(cache_dir(), base);
            write_file_text(binary, m.data);
            found = true;
        }
    }
#endif
    if (!found) {
        throw std::runtime_error("failed to locate llama-server binary in release archive");
    }
#ifdef _WIN32
    (void)chmod;  // permissions inherited on Windows
#else
    chmod(binary.c_str(), 0755);
#endif
    logger("llama-server downloaded");
    return binary;
}

// ---------------------------------------------------------------------------
// _wait_for_server
// ---------------------------------------------------------------------------
bool wait_for_server(const std::string& host, int port, double timeout_sec) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(timeout_sec));
    std::string url = "http://" + host + ":" + std::to_string(port) + "/health";
    for (;;) {
        try {
            auto resp = http::request("GET", url, "", {}, 2);
            if (resp.status == 200) return true;
        } catch (const std::exception&) {
            // connection refused / timeout — keep polling until the deadline
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// ---------------------------------------------------------------------------
// _send_to_daemon / _ensure_daemon
// ---------------------------------------------------------------------------
json::Value send_to_daemon(const json::Value& payload, int retries,
                           double retry_delay_sec) {
    std::string last_error = "unknown";
    std::string payload_str = json::dumps(payload);
    for (int attempt = 0; attempt < retries; ++attempt) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            last_error = std::string(strerror(errno));
            std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(retry_delay_sec)));
            continue;
        }
        struct timeval tv;
        tv.tv_sec = static_cast<long>(retry_delay_sec);
        tv.tv_usec = static_cast<long>((retry_delay_sec - static_cast<double>(tv.tv_sec)) * 1e6);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr.s_addr);
        addr.sin_port = htons(CONTROL_PORT);

        bool ok = true;
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
            last_error = std::string(strerror(errno));
            ok = false;
        } else {
            std::string data = payload_str + "\n";
            size_t off = 0;
            while (ok && off < data.size()) {
                ssize_t n = send(fd, data.data() + off, data.size() - off, 0);
                if (n <= 0) {
                    last_error = std::string(strerror(errno));
                    ok = false;
                } else {
                    off += static_cast<size_t>(n);
                }
            }
            if (ok) {
                std::string resp_data;
                char buf[8192];
                for (;;) {
                    ssize_t n = recv(fd, buf, sizeof buf, 0);
                    if (n < 0) {
                        last_error = std::string(strerror(errno));
                        ok = false;
                        break;
                    }
                    if (n == 0) break;
                    resp_data.append(buf, static_cast<size_t>(n));
                    if (resp_data.size() > 65536) { ok = false; break; }
                }
                if (ok) {
                    try {
                        json::Value v = json::parse(resp_data);
                        close(fd);
                        return v;
                    } catch (const std::exception& e) {
                        last_error = e.what();
                        ok = false;
                    }
                }
            }
        }
        close(fd);
        std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(retry_delay_sec)));
    }
    throw std::runtime_error("server daemon unreachable: " + last_error);
}

void ensure_daemon() {
    std::string runtime = cache_dir();
    std::string pid_file = path_join(runtime, "daemon.pid");

    long old_pid = 0;
    auto old_text = read_file_text(pid_file);
    if (old_text.has_value()) {
        std::string t = trim(*old_text);
        if (!t.empty()) old_pid = std::atol(t.c_str());
    }
    if (old_pid > 0 && process_alive(static_cast<pid_t>(old_pid))) return;

    make_dirs(runtime);
    std::string daemon = path_join(exe_dir(), "server_daemon");
#ifdef _WIN32
    daemon += ".exe";
#endif
    std::vector<std::string> args = {
        daemon,
        "--control-port", std::to_string(CONTROL_PORT),
        "--runtime-dir", runtime,
    };
    spawn_detached(args, debug_flag());
}

// ---------------------------------------------------------------------------
// _kill_servers_on_port / _safe_kill_pid / _safe_kill / _kill_hookpoint
// ---------------------------------------------------------------------------
// _safe_kill_pid — SIGKILL (POSIX) / taskkill (Windows).
static void safe_kill_pid(pid_t pid) {
    if (pid <= 0) return;
#ifdef _WIN32
    std::string cmd = "taskkill /F /T /PID " + std::to_string(static_cast<long>(pid));
    (void)std::system(cmd.c_str());
#else
    kill(pid, SIGKILL);
#endif
}

// _safe_kill — SIGTERM, wait up to 5s, SIGKILL, wait up to 3s.
static void safe_kill(pid_t pid) {
    if (pid <= 0 || !process_alive(pid)) return;
#ifdef _WIN32
    safe_kill_pid(pid);
    return;
#else
    kill(pid, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        if (waitpid_no_retry(pid) >= 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    safe_kill_pid(pid);
    for (int i = 0; i < 30; ++i) {
        if (waitpid_no_retry(pid) >= 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
}

// Fallback process discovery for _kill_servers_on_port: lsof on POSIX,
// netstat on Windows (psutil is not a C++ dependency).
static std::set<long> pids_on_port(int port) {
    std::set<long> pids;
    std::string cmd;
#ifdef _WIN32
    cmd = "netstat -ano | findstr :" + std::to_string(port);
#else
    cmd = "lsof -ti tcp:" + std::to_string(port);
#endif
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return pids;
    char buf[4096];
    while (fgets(buf, sizeof buf, pipe) != nullptr) {
        std::string line = trim(buf);
        if (line.empty()) continue;
        long pid = 0;
        bool any = false;
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (c >= '0' && c <= '9') {
                pid = pid * 10 + (c - '0');
                any = true;
            } else if (c == ',' || c == ' ' || c == '\t') {
                if (any) break;
            } else {
                break;
            }
        }
        if (any && pid > 0) pids.insert(pid);
    }
    pclose(pipe);
    return pids;
}

static void kill_servers_on_port(int port) {
    for (long pid : pids_on_port(port)) safe_kill_pid(static_cast<pid_t>(pid));
}

// _kill_hookpoint
static void kill_hookpoint() {
    int pid = g_state.hookpoint_pid;
    g_state.hookpoint_pid = 0;
    if (pid > 0) safe_kill(static_cast<pid_t>(pid));
}

// The hookpoint sentinel is spawned as a direct child of the agent (NOT
// detached — it should die together with the agent). Mirrors
// Popen([sys.executable, hookpoint.py, str(os.getpid())]).
static pid_t spawn_hookpoint() {
    std::string hp = path_join(exe_dir(), "hookpoint");
#ifdef _WIN32
    hp += ".exe";
#endif
    std::vector<std::string> args = {hp, std::to_string(static_cast<long>(getpid()))};
    return spawn_child(args);
}

// ---------------------------------------------------------------------------
// _build_server_args
// ---------------------------------------------------------------------------
std::vector<std::string> build_server_args(const ModelSpec& spec,
                                           const std::string& target_path,
                                           const std::string& draft_path,
                                           int port, const std::string& host,
                                           Backend backend,
                                           const json::Value* inference) {
    std::vector<std::string> args = {server_binary_path(backend)};
    args.push_back("-m");
    args.push_back(target_path);
    args.push_back("--host");
    args.push_back(host);
    args.push_back("--port");
    args.push_back(std::to_string(port));
    args.push_back("-fa");
    args.push_back("on");
    args.push_back("-ctk");
    args.push_back("q5_0");
    args.push_back("-ctv");
    args.push_back("q5_0");
    if (backend != Backend::CPU) {
        args.push_back("-ngl");
        args.push_back(std::to_string(DEFAULT_NGL_LAYERS));
    }
    if (spec.n_ctx.has_value()) {
        args.push_back("-c");
        args.push_back(std::to_string(*spec.n_ctx));
    }

    const json::Value* overrides = inference ? inference->get(spec.key) : nullptr;
    if (spec.spec_type != std::string(SPEC_TYPE_NONE) && !draft_path.empty()) {
        args.push_back("-md");
        args.push_back(draft_path);
        args.push_back("--spec-type");
        args.push_back(spec.spec_type);
        const json::Value* n_max = overrides ? overrides->get("spec_draft_n_max") : nullptr;
        if (n_max && n_max->is_number()) {
            args.push_back("--spec-draft-n-max");
            args.push_back(n_max->number_to_string());
        }
    }
    if (spec.reasoning != "auto") {
        args.push_back("--jinja");
        args.push_back("--reasoning");
        args.push_back(spec.reasoning);
        args.push_back("--reasoning-format");
        args.push_back("none");
        std::string effort;
        const json::Value* e = overrides ? overrides->get("reasoning_effort") : nullptr;
        if (e && e->is_string()) effort = e->s;
        if (effort.empty() && spec.reasoning_effort.has_value()) effort = *spec.reasoning_effort;
        if (!effort.empty()) {
            args.push_back("--reasoning-effort");
            args.push_back(effort);
        }
    }
    for (const auto& [name, value] : spec.sampling_overrides) {
        std::string flag;
        if (name == "temperature") flag = "--temp";
        else if (name == "top_p") flag = "--top-p";
        else if (name == "top_k") flag = "--top-k";
        else if (name == "repeat_penalty") flag = "--repeat-penalty";
        else continue;
        args.push_back(flag);
        args.push_back(json::Value::make_double(value).number_to_string());
    }
    return args;
}

// ---------------------------------------------------------------------------
// stop_server / server_url
// ---------------------------------------------------------------------------
void stop_server() {
    int port = g_state.server_port != 0 ? g_state.server_port : DEFAULT_PORT;
    json::Value payload = json::Value::make_object();
    payload.obj.push_back({"cmd", json::Value::make_string("STOP")});
    payload.obj.push_back({"port", json::Value::make_int(port)});
    try {
        send_to_daemon(payload, 2, 0.1);
    } catch (const std::exception&) {
        // daemon offline — fall through to direct process kill
    }
    kill_servers_on_port(port);
    g_state.server_pid = 0;
    g_state.server_port = 0;
    kill_hookpoint();
}

std::string server_url(int port, const std::string& host) {
    return "http://" + host + ":" + std::to_string(port);
}

// ---------------------------------------------------------------------------
// start_server
// ---------------------------------------------------------------------------
void start_server(const ModelSpec& spec, const std::string& target_path,
                   const std::string& draft_path, int port, const std::string& host,
                   Backend backend, LogFn log) {
    LogFn logger = log ? log : default_log;
    stop_server();

    json::Value inference = load_inference();
    std::vector<std::string> args =
        build_server_args(spec, target_path, draft_path, port, host, backend, &inference);
    logger("starting llama-server on " + host + ":" + std::to_string(port) + " ...");

    g_state.hookpoint_pid = static_cast<int>(spawn_hookpoint());

    ensure_daemon();
    json::Value payload = json::Value::make_object();
    payload.obj.push_back({"cmd", json::Value::make_string("START")});
    payload.obj.push_back({"host", json::Value::make_string(host)});
    payload.obj.push_back({"port", json::Value::make_int(port)});
    payload.obj.push_back({"hookpoint_pid", json::Value::make_int(g_state.hookpoint_pid)});
    json::Value args_v = json::Value::make_array();
    for (const auto& a : args) args_v.arr.push_back(json::Value::make_string(a));
    payload.obj.push_back({"server_args", std::move(args_v)});

    json::Value resp;
    try {
        resp = send_to_daemon(payload);
    } catch (const std::exception& e) {
        kill_hookpoint();
        throw std::runtime_error(std::string("server daemon failed to start llama-server: ") + e.what());
    }
    if (resp.str_of("status") != "ok") {
        kill_hookpoint();
        throw std::runtime_error("server daemon failed to start llama-server: " + json::dumps(resp));
    }
    const json::Value* sp = resp.get("server_pid");
    g_state.server_pid = (sp && sp->is_number())
                             ? static_cast<int>(sp->kind == json::Value::Kind::Int ? sp->i : static_cast<long long>(sp->d))
                             : 0;
    g_state.server_port = port;

    if (!wait_for_server(host, port, 120.0)) {
        if (backend != Backend::CPU) {
            logger(std::string(backend_label(backend)) + " backend failed to start; retrying with CPU");
            stop_server();
            ensure_server_binary(Backend::CPU, logger);
            start_server(spec, target_path, draft_path, port, host, Backend::CPU, logger);
            return;
        }
        stop_server();
        throw std::runtime_error("llama-server failed to start on " + host + ":" +
                                 std::to_string(port) + " within 120s; try a different model");
    }
    logger("llama-server ready");
}

}  // namespace ga
