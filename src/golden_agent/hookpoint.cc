#include "hookpoint.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>

namespace ga::hookpoint {

bool agent_alive(pid_t pid) {
    if (pid <= 0) return true;
    if (kill(pid, 0) == 0) return true;
    if (errno == ESRCH) return false;  // ProcessLookupError
    return true;                      // EPERM == PermissionError -> alive
}

int run(int argc, char** argv) {
    long agent_pid = 0;
    bool have_pid = argc > 1 && argv[1] != nullptr;
    if (have_pid) agent_pid = std::strtoul(argv[1], nullptr, 10);
    while (true) {
        if (have_pid && !agent_alive(static_cast<pid_t>(agent_pid))) {
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

}  // namespace ga::hookpoint
