// server_daemon.cc — always-on background daemon that owns the llama-server
// lifecycle.  Faithful port of Golden-Agent/src/golden_agent/server_daemon.py
// (205-line snapshot, 2026-09-02).
//
// Behavior notes kept faithful to the Python source:
//   * Exactly one daemon per runtime dir, enforced by <runtime-dir>/daemon.pid
//     (an alive pid-file owner makes a new instance exit 0 immediately).
//   * The daemon listens on 127.0.0.1:<control-port> for newline-terminated
//     JSON commands: PING, START, STOP.  START spawns the llama-server child
//     plus a "monitor" co-process of this same binary that watches the
//     hookpoint sentinel.
//   * monitor mode is parsed by the SAME outer argparse (control-port and
//     runtime-dir are required), so — exactly like the Python — any monitor
//     invocation fails with exit code 2 ("unrecognized arguments").
//     monitor_loop() below is the direct equivalent of _monitor_loop and is
//     still implemented for completeness / direct testing.
#include "server_daemon.hpp"

#include "json.hpp"
#include "util.hpp"

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ga::daemon {

namespace {

// Still-active children map: port -> (server_pid, monitor_pid).
// Mirrors server_daemon.py `_serve.sessions`.
struct Session {
    pid_t server_pid;
    pid_t monitor_pid;
};

// Best-effort full send (Python conn.sendall).
bool sendall(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
#ifdef _WIN32
        int n = send(fd, data.data() + off, static_cast<int>(data.size() - off), 0);
        if (n < 0) {
            if (WSAGetLastError() == WSAEINTR) continue;
            return false;
        }
#else
        ssize_t n = send(fd, data.data() + off, data.size() - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
#endif
        off += static_cast<size_t>(n);
    }
    return true;
}

// Path of the running executable — the daemon re-invokes itself for the
// "monitor" mode, exactly like Python [sys.executable, daemon_script, ...].
std::string self_exe() {
#ifdef _WIN32
    char buf[4096];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf - 1);
    if (n == 0 || n >= sizeof buf) n = 0;
    return std::string(buf, n);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        return buf;
    }
    // Fallback (we are always built from src/golden_agent/server_daemon.cc).
    return path_join(current_dir(), "server_daemon");
#endif
}

void usage() {
    std::fprintf(stderr,
                 "usage: server_daemon [-h] --control-port CONTROL_PORT\n"
                 "                        --runtime-dir RUNTIME_DIR [mode]\n");
}

void usage_required(const bool need_port, const bool need_dir) {
    std::fprintf(stderr, "server_daemon: error: the following arguments are required: ");
    if (need_port) std::fprintf(stderr, "%s", need_dir ? "--control-port, " : "--control-port");
    if (need_dir) std::fprintf(stderr, "--runtime-dir");
    std::fprintf(stderr, "\n");
}

void usage_unrecognized(int argc, char** argv, int from) {
    std::fprintf(stderr, "server_daemon: error: unrecognized arguments: ");
    for (int i = from; i < argc; ++i) {
        if (i > from) std::fputc(' ', stderr);
        std::fputs(argv[i], stderr);
    }
    std::fprintf(stderr, "\n");
}

}  // namespace

// ---------------------------------------------------------------------------
// _alive / _kill
// ---------------------------------------------------------------------------

bool alive(pid_t pid) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(0x0400, /*inherit=*/FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    bool result = false;
    DWORD code = 0;
    if (GetExitCodeProcess(h, &code)) result = (code == STILL_ACTIVE);
    CloseHandle(h);
    return result;
#else
    if (kill(pid, 0) == 0) return true;
    if (errno == ESRCH) return false;
    return true;  // EPERM: exists, not ours
#endif
}

void kill_pid(pid_t pid) {
    if (pid <= 0) return;
#ifdef _WIN32
    std::string cmd = "taskkill /PID " + std::to_string(static_cast<long>(pid)) + " /F /T";
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    (void)CreateProcessA(nullptr, const_cast<char*>(cmd.c_str()), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread) CloseHandle(pi.hThread);
#else
    if (kill(pid, 9) == 0) return;
    if (errno == ESRCH) return;
#endif
}

// ---------------------------------------------------------------------------
// poll_child
// ---------------------------------------------------------------------------

// poll()-style: returns the exit status if the child has exited (and was
// reaped here) — matching Python proc.poll() semantics — or -1 if still
// running.  For pids that are not our children, falls back to liveness:
// 0 if gone, -1 if alive.
int poll_child(pid_t pid) {
    if (pid <= 0) return 0;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return 0;  // gone
    DWORD code = 0;
    bool ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    if (!ok) return 0;
    return code == STILL_ACTIVE ? -1 : static_cast<int>(code);
#else
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return -1;
    }
    // Not our child (or un-reapable): liveness probe.
    if (r < 0 && errno == ECHILD) return 0;
    return (kill(pid, 0) == 0 || errno == EPERM) ? -1 : 0;
#endif
}

// ---------------------------------------------------------------------------
// _monitor_loop
// ---------------------------------------------------------------------------

void monitor_loop(pid_t hookpoint_pid, pid_t server_pid) {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!alive(hookpoint_pid)) {
            kill_pid(server_pid);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// _serve
// ---------------------------------------------------------------------------

int serve(int control_port, const std::string& runtime_dir) {
    (void)runtime_dir;  // reserved for pid-file / runtime bookkeeping in main()
    std::map<long, Session> sessions;
    const std::string daemon_script = self_exe();

    // _stop_session(port)
    auto stop_session = [&](long port) {
        auto it = sessions.find(port);
        if (it == sessions.end()) return;
        Session s = it->second;
        sessions.erase(it);
        if (s.server_pid > 0 && poll_child(s.server_pid) < 0) {
            kill_pid(s.server_pid);
            for (int i = 0; i < 30 && poll_child(s.server_pid) < 0; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (s.monitor_pid > 0 && poll_child(s.monitor_pid) < 0)
            kill_pid(s.monitor_pid);
    };

#ifdef _WIN32
    WSADATA wsa;
    (void)WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    int srv = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (srv < 0) return 1;
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
#ifdef _WIN32
    addr.sin_addr.S_un.S_addr = htonl(0x0100007F);
#else
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr.s_addr);
#endif
    addr.sin_port = htons(static_cast<uint16_t>(control_port));
    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        ::close(srv);
        return 1;
    }
    listen(srv, 8);
    timeval tv{1, 0};
    setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);

    for (;;) {
        // Reap finished sessions.
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (poll_child(it->second.server_pid) >= 0 && poll_child(it->second.monitor_pid) >= 0)
                it = sessions.erase(it);
            else
                ++it;
        }

        int conn = static_cast<int>(accept(srv, nullptr, nullptr));
        if (conn < 0) {
            int e = errno;
#ifdef _WIN32
            if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) continue;
#else
            if (e == EAGAIN || e == EWOULDBLOCK) continue;
#endif
            ::close(srv);
            return 1;
        }

        {
            // One bounded read window (Python: conn.recv(65536)); a 500 ms
            // receive timeout keeps this from blocking forever.
            timeval ctv{0, 500000};
            setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ctv), sizeof ctv);
            std::string raw;
            char buf[8192];
            for (;;) {
#ifdef _WIN32
                int n = recv(conn, buf, sizeof buf, 0);
                if (n < 0) {
                    int e = WSAGetLastError();
                    if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) break;
                    break;
                }
                if (n == 0) break;
                raw.append(buf, static_cast<size_t>(n));
                if (raw.size() >= 65536) break;
#else
                ssize_t n = recv(conn, buf, sizeof buf, 0);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    raw.clear();
                    break;
                }
                if (n == 0) break;
                raw.append(buf, static_cast<size_t>(n));
                if (raw.size() >= 65536) break;
#endif
            }
            raw = trim(raw);

            json::Value payload;
            bool ok = !raw.empty();
            if (ok) {
                try {
                    payload = json::parse(raw);
                } catch (...) {
                    ok = false;
                }
            }
            const json::Value* cv = (ok && payload.is_object()) ? payload.get("cmd") : nullptr;
            if (ok && cv && cv->is_string()) {
                if (cv->s == "PING") {
                    sendall(conn, "{\"status\":\"ok\"}\n");
                } else if (cv->s == "START") {
                    long port = static_cast<long>(payload.double_of("port"));
                    long hookpoint_pid = static_cast<long>(payload.double_of("hookpoint_pid"));
                    std::vector<std::string> server_args;
                    if (const json::Value* sa = payload.get("server_args"); sa && sa->is_array())
                        for (const json::Value& e : sa->arr)
                            if (e.is_string()) server_args.push_back(e.s);
                    stop_session(port);
                    bool debug = env_get("GOLDEN_AGENT_DEBUG").has_value();
                    pid_t server_pid = spawn_detached(server_args, debug);
                    std::vector<std::string> monitor_args = {
                        daemon_script,
                        "monitor",
                        "--hookpoint-pid", std::to_string(hookpoint_pid),
                        "--server-pid", std::to_string(static_cast<long>(server_pid)),
                    };
                    pid_t monitor_pid = spawn_detached(monitor_args, false);
                    sessions.emplace(port, Session{server_pid, monitor_pid});
                    std::string resp =
                        "{\"status\":\"ok\",\"server_pid\":" + std::to_string(static_cast<long>(server_pid)) + "}\n";
                    sendall(conn, resp);
                } else if (cv->s == "STOP") {
                    long port = static_cast<long>(payload.double_of("port"));
                    if (port != 0) {
                        stop_session(port);
                    } else {
                        std::vector<long> ports;
                        for (const auto& [p, s] : sessions) ports.push_back(p);
                        for (long p : ports) stop_session(p);
                    }
                    sendall(conn, "{\"status\":\"ok\"}\n");
                }
            }
            // Unrecognized / invalid payload: close without responding
            // (Python: `continue` after json decode failure / no branch).
        }
        ::close(conn);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int run(int argc, char** argv) {
    long control_port = 0;
    bool have_control_port = false;
    std::string runtime_dir;
    bool have_runtime_dir = false;
    std::vector<std::string> positionals;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--control-port") {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            control_port = std::atol(argv[++i]);
            have_control_port = true;
        } else if (a == "--runtime-dir") {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            runtime_dir = argv[++i];
            have_runtime_dir = true;
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a.size() >= 2 && a[0] == '-' && a[1] == '-') {
            // argparse: unknown option → exit 2, listing the remaining args.
            usage_unrecognized(argc, argv, i);
            return 2;
        } else {
            positionals.push_back(a);
        }
    }

    const std::string mode = positionals.empty() ? "daemon" : positionals[0];
    if (!have_control_port || !have_runtime_dir) {
        usage_required(!have_control_port, !have_runtime_dir);
        return 2;
    }

    if (mode == "monitor") {
        // Python: `args.mode == "monitor"` can only be reached when the outer
        // parser already accepted everything; the inner parser
        // (argparse of sys.argv[2:] with --hookpoint-pid / --server-pid) then
        // rejects the positional "monitor" and any monitor flags as
        // unrecognized → exit 2.  Reproduce that exactly: every token
        // beyond the recognized flags is an unrecognized remainder.
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "monitor" ||
                (a.size() >= 2 && a[0] == '-' && a[1] == '-' &&
                 a != "--control-port" && a != "--runtime-dir")) {
                usage_unrecognized(argc, argv, i);
                return 2;
            }
        }
        // Unreachable in practice (the loop above always fires): bare
        // "--control-port P --runtime-dir D monitor" is rejected by the
        // inner parser in Python too.  Fall through for structural parity.
        // monitor_loop(hookpoint, server) would be the loop body here.
        return 2;
    }

    // runtime_dir.mkdir(parents=True, exist_ok=True)
    make_dirs(runtime_dir);
    const std::string pid_file = path_join(runtime_dir, "daemon.pid");

    // If an older daemon's pid-file is still alive, do nothing and exit 0.
    if (file_exists(pid_file)) {
        bool ok = false;
        auto text = read_file_text(pid_file, &ok);
        long old_pid = 0;
        if (ok) {
            const std::string t = trim(text.value());
            if (!t.empty()) {
                char* end = nullptr;
                old_pid = std::strtol(t.c_str(), &end, 10);
            }
        }
        if (old_pid > 0 && alive(static_cast<pid_t>(old_pid))) return 0;
    }

    // Write our own pid, serve until killed, unlink the pid-file in `finally`.
    write_file_text(pid_file, std::to_string(static_cast<long>(getpid())));
    int rc = serve(static_cast<int>(control_port), runtime_dir);
    ::remove(pid_file.c_str());  // ignore errors, mirroring os.unlink(ignore_errors=True)
    return rc;
}

}  // namespace ga::daemon

int main(int argc, char** argv) {
    return ga::daemon::run(argc, argv);
}
