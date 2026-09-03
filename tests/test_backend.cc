// Faithful port of Golden-Agent/tests/test_backend.py (245 lines, 2026-09-02
// snapshot). 15 tests.
//
// Python's _wait_for_server / stop_server tests use unittest.mock against
// http.client and module globals. In C++ the mock seams don't exist, so:
//   - _wait_for_server success is exercised against a real loopback HTTP
//     listener that answers 200 (documented deviation, same observable).
//   - _wait_for_server timeout is exercised against a refused port
//     (Python mocks OSError("refused"), same observable: returns false).
//   - stop_server tests use a real sleep process as the hookpoint sentinel.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "backend.hpp"
#include "config.hpp"
#include "util.hpp"

using namespace ga;

static int passed = 0;
static int failures = 0;
#define CHECK(cond) do { \
    if (cond) { ++passed; } \
    else { ++failures; std::printf("FAIL (line %d): %s\n", __LINE__, #cond); } \
} while (0)

// Exact token match (Python: `x in args`).
static bool contains(const std::vector<std::string>& args, const std::string& tok) {
    for (const auto& a : args) if (a == tok) return true;
    return false;
}

// Value following a flag (Python: args[args.index(flag) + 1]).
static bool next_after(const std::vector<std::string>& args,
                       const std::string& flag, const std::string& want) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == flag) return args[i + 1] == want;
    return false;
}

// Custom ModelSpec matching the Python tests' `ModelSpec(key="test", ...)`.
static ModelSpec test_spec() {
    ModelSpec s;
    s.key = "test";
    s.label = "Test";
    s.repo = "repo/model";
    s.filename = "model.gguf";
    s.size_bytes = 1000;
    return s;
}

// --- _build_server_args ----------------------------------------------------

// Python: test_build_server_args_cpu_no_ngl
static void t_build_cpu_no_ngl() {
    ModelSpec s = test_spec();
    auto args = build_server_args(s, "/path/to/model.gguf", "",
                                  DEFAULT_PORT, "127.0.0.1", Backend::CPU, nullptr);
    CHECK(!contains(args, "-ngl"));
    CHECK(contains(args, "-fa"));
}

// Python: test_build_server_args_with_ctx
static void t_build_with_ctx() {
    ModelSpec s = test_spec();
    s.n_ctx = 128000;
    auto args = build_server_args(s, "/path/to/model.gguf");
    CHECK(contains(args, "-c"));
    CHECK(contains(args, "128000"));
}

// Python: test_build_server_args_with_draft
static void t_build_with_draft() {
    ModelSpec s = test_spec();
    s.spec_type = SPEC_TYPE_DRAFT_DFLASH;
    auto args = build_server_args(s, "/path/to/model.gguf", "/path/to/draft.gguf");
    CHECK(contains(args, "-md"));
    CHECK(contains(args, "/path/to/draft.gguf"));
    CHECK(contains(args, "--spec-type"));
    CHECK(contains(args, "draft-dflash"));
}

// Python: test_build_server_args_no_draft_when_none
static void t_build_no_draft_when_none() {
    ModelSpec s = test_spec();
    s.spec_type = SPEC_TYPE_NONE;
    auto args = build_server_args(s, "/path/to/model.gguf");
    CHECK(!contains(args, "-md"));
    CHECK(!contains(args, "--spec-type"));
}

// Python: test_build_server_args_passes_reasoning_on
static void t_build_passes_reasoning_on() {
    ModelSpec s = test_spec();
    s.reasoning = "on";
    auto args = build_server_args(s, "/path/to/model.gguf");
    CHECK(contains(args, "--reasoning"));
    CHECK(next_after(args, "--reasoning", "on"));
}

// Python: test_build_server_args_reasoning_requires_jinja_and_inline_format
static void t_build_reasoning_requires_jinja() {
    ModelSpec s = test_spec();
    s.reasoning = "on";
    auto args = build_server_args(s, "/path/to/model.gguf");
    CHECK(contains(args, "--jinja"));
    CHECK(contains(args, "--reasoning-format"));
    CHECK(next_after(args, "--reasoning-format", "none"));
}

// Python: test_build_server_args_omits_reasoning_when_auto
static void t_build_omits_reasoning_when_auto() {
    ModelSpec s = test_spec();  // reasoning defaults to "auto"
    auto args = build_server_args(s, "/path/to/model.gguf");
    CHECK(!contains(args, "--reasoning"));
    CHECK(!contains(args, "--reasoning-effort"));
    CHECK(!contains(args, "--jinja"));
}

// Python: test_build_server_args_passes_reasoning_effort
static void t_build_passes_reasoning_effort() {
    ModelSpec s = test_spec();
    s.reasoning = "on";
    s.reasoning_effort = "medium";
    auto args = build_server_args(s, "/path/to/model.gguf");
    CHECK(contains(args, "--reasoning"));
    CHECK(contains(args, "--reasoning-effort"));
    CHECK(next_after(args, "--reasoning-effort", "medium"));
}

// Python: test_build_server_args_omits_reasoning_effort_when_none
static void t_build_omits_reasoning_effort_when_none() {
    ModelSpec s = test_spec();
    s.reasoning = "on";
    auto args = build_server_args(s, "/path/to/model.gguf");
    CHECK(contains(args, "--reasoning"));
    CHECK(!contains(args, "--reasoning-effort"));
}

// Python: test_build_server_args_with_sampling_overrides
static void t_build_with_sampling_overrides() {
    ModelSpec s = test_spec();
    s.sampling_overrides = {{"temperature", 0.5}, {"top_p", 0.9}};
    auto args = build_server_args(s, "/path/to/model.gguf");
    CHECK(contains(args, "--temp"));
    CHECK(contains(args, "0.5"));
    CHECK(contains(args, "--top-p"));
    CHECK(contains(args, "0.9"));
}

// Python: test_build_server_args_custom_port
static void t_build_custom_port() {
    ModelSpec s = test_spec();
    auto args = build_server_args(s, "/path/to/model.gguf", "", 9090, "0.0.0.0");
    CHECK(contains(args, "--port"));
    CHECK(contains(args, "9090"));
    CHECK(contains(args, "--host"));
    CHECK(contains(args, "0.0.0.0"));
}

// --- _wait_for_server -------------------------------------------------------

// Bind a free loopback port and return it (probe socket closed afterwards).
static int free_loopback_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = 0;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t len = sizeof(a);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    int port = ntohs(a.sin_port);
    ::close(fd);
    return port;
}

// Loopback TCP listener. mode 0: answer every request with 200 immediately.
// mode 1: accept and hang. `ready` is set after bind+listen so the caller can
// wait for the listener to be live (eliminates bind-vs-first-request races).
static void run_fake_http(int port, int mode, std::atomic<bool>& ready) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::close(fd);
        return;
    }
    ::listen(fd, 8);
    ready = true;
    while (true) {
        int c = ::accept(fd, nullptr, nullptr);
        if (c < 0) break;
        if (mode == 1) {
            ::close(c);
            continue;
        }
        std::string req;
        char buf[1024];
        for (;;) {
            ssize_t n = ::read(c, buf, sizeof(buf));
            if (n <= 0) break;
            req.append(buf, n);
            if (req.size() >= 4 && req.compare(req.size() - 4, 4, "\r\n\r\n") == 0)
                break;
            if (req.size() > 65536) break;
        }
        const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        ::write(c, resp, std::strlen(resp));
        ::close(c);
    }
    ::close(fd);
}

// Python: test_wait_for_server_returns_true_on_200
static void t_wait_true_on_200() {
    int port = free_loopback_port();
    std::atomic<bool> ready{false};
    std::thread t(run_fake_http, port, 0, std::ref(ready));
    while (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(wait_for_server("127.0.0.1", port, 5.0));
    t.detach();
}

// Python: test_wait_for_server_returns_false_on_timeout
// (Python mocks connection refused; a never-bound port is the C++ equivalent.)
static void t_wait_false_on_timeout() {
    int port = free_loopback_port();  // probed then closed -> refused
    CHECK(!wait_for_server("127.0.0.1", port, 0.5));
}

// --- stop_server ------------------------------------------------------------

// Python: test_stop_server_kills_process
static void t_stop_kills_process() {
    auto& st = backend_state();
    st.hookpoint_pid = 0;
    st.server_pid = 0;
    st.server_port = 0;

    // Real hookpoint sentinel, mirroring be._hookpoint_process.
    pid_t hp = spawn_detached(std::vector<std::string>{"sleep", "3600"});
    CHECK(hp > 0);
    st.hookpoint_pid = hp;
    st.server_pid = 12345;
    st.server_port = DEFAULT_PORT;

    stop_server();

    CHECK(!process_alive(hp));
    CHECK(st.hookpoint_pid == 0);
    CHECK(st.server_pid == 0);
    CHECK(st.server_port == 0);
}

// Python: test_stop_server_handles_already_stopped
static void t_stop_already_stopped() {
    auto& st = backend_state();
    st.hookpoint_pid = 0;
    st.server_pid = 0;
    st.server_port = 0;
    stop_server();  // must not crash
    CHECK(st.hookpoint_pid == 0);
    CHECK(st.server_pid == 0);
    CHECK(st.server_port == 0);
}

int main() {
    const struct { const char* name; void (*fn)(); } tests[] = {
        {"build_server_args_cpu_no_ngl", t_build_cpu_no_ngl},
        {"build_server_args_with_ctx", t_build_with_ctx},
        {"build_server_args_with_draft", t_build_with_draft},
        {"build_server_args_no_draft_when_none", t_build_no_draft_when_none},
        {"build_server_args_passes_reasoning_on", t_build_passes_reasoning_on},
        {"build_server_args_reasoning_requires_jinja_and_inline_format",
         t_build_reasoning_requires_jinja},
        {"build_server_args_omits_reasoning_when_auto", t_build_omits_reasoning_when_auto},
        {"build_server_args_passes_reasoning_effort", t_build_passes_reasoning_effort},
        {"build_server_args_omits_reasoning_effort_when_none",
         t_build_omits_reasoning_effort_when_none},
        {"build_server_args_with_sampling_overrides", t_build_with_sampling_overrides},
        {"build_server_args_custom_port", t_build_custom_port},
        {"wait_for_server_returns_true_on_200", t_wait_true_on_200},
        {"wait_for_server_returns_false_on_timeout", t_wait_false_on_timeout},
        {"stop_server_kills_process", t_stop_kills_process},
        {"stop_server_handles_already_stopped", t_stop_already_stopped},
    };
    for (const auto& t : tests) {
        std::printf("== %s\n", t.name);
        t.fn();
    }
    std::printf("%d passed, %d failed\n", passed, failures);
    return failures == 0 ? 0 : 1;
}
