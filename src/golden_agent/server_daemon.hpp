// server_daemon — always-on background daemon that owns the llama-server
// lifecycle.
//
// Mirrors Golden-Agent/src/golden_agent/server_daemon.py. It listens on
// 127.0.0.1:<control-port> (default 2012, see backend.cpp) for newline-
// terminated JSON commands (PING / START / STOP). START spawns the
// llama-server child plus a "monitor" co-process of this same binary that
// watches the hookpoint sentinel: when the hookpoint process dies, the
// monitor kills the server. Exactly one daemon instance is alive at a
// time, enforced via <runtime-dir>/daemon.pid.
//
// Two execution modes, selected by the positional argument:
//   server_daemon --control-port N --runtime-dir DIR      (default: "daemon")
//   server_daemon --control-port N --runtime-dir DIR monitor --hookpoint-pid H --server-pid S
#pragma once

#include <string>
#include <sys/types.h>

namespace ga::daemon {

// STILL_ACTIVE is the Windows GetExitCodeProcess sentinel meaning "still
// running" (server_daemon.py::STILL_ACTIVE).
constexpr int STILL_ACTIVE = 259;

// True if the process `pid` is still alive.  (server_daemon.py::_alive)
bool alive(pid_t pid);

// SIGKILL `pid`; no-op for pid <= 0.  (server_daemon.py::_kill)
void kill_pid(pid_t pid);

// poll()-style check for our own children: exit status (>= 0) if already
// reaped/exit-able, -1 while still running.
int poll_child(pid_t pid);

// Sleep-2s loop; when the hookpoint sentinel dies, SIGKILL the server and
// return.  Runs inside its own subprocess of the daemon
// (server_daemon.py::_monitor_loop).
void monitor_loop(pid_t hookpoint_pid, pid_t server_pid);

// Bind 127.0.0.1:control_port and serve forever.  (server_daemon.py::_serve)
// Returns 0 on clean exit, non-zero on socket failure.
int serve(int control_port, const std::string& runtime_dir);

// Entry point mirroring server_daemon.py::main.  Parses args, dispatches
// "monitor" mode or the default "daemon" mode, enforces the pid-file
// lifecycle.  Returns the process exit code.
int run(int argc, char** argv);

}  // namespace ga::daemon
