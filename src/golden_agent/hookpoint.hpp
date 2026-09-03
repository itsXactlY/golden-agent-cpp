// Hookpoint sentinel spawned as a direct child of the golden-agent.
//
// Its only job is to exist while the agent is alive. The server daemon's
// monitor watches this process; when it disappears (agent crashed / killed),
// the daemon reaps the orphaned llama-server. Because this is a child of the
// agent it is reaped with the agent, and as a belt-and-suspenders check it
// also exits on its own if it can no longer see the agent process.
#pragma once
#include <sys/types.h>
#include <cstdint>

namespace ga::hookpoint {

// Windows exit code for a running process.
constexpr int STILL_ACTIVE = 259;

// True if the process with `pid` is still alive.
bool agent_alive(pid_t pid);

// Sentinel main: exit 0 when the agent is gone, else keep sleeping.
// Mirrors hookpoint.py::main — agent_pid is argv[1] if present.
int run(int argc, char** argv);

}  // namespace ga::hookpoint
