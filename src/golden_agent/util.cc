#include "golden_agent/util.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace ga {

// ---- string helpers -------------------------------------------------------

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && is_space(s[a])) a++;
    while (b > a && is_space(s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string lstrip(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && is_space(s[a])) a++;
    return s.substr(a);
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int count_substring(const std::string& s, const std::string& sub) {
    if (sub.empty()) return 0;
    int n = 0;
    size_t pos = 0;
    while ((pos = s.find(sub, pos)) != std::string::npos) { n++; pos += sub.size(); }
    return n;
}

std::string replace(const std::string& s, const std::string& sub, const std::string& with, int max_count) {
    if (sub.empty() || max_count == 0) return s;
    std::string out;
    size_t pos = 0;
    int done = 0;
    while (true) {
        size_t found = s.find(sub, pos);
        if (found == std::string::npos) break;
        out += s.substr(pos, found - pos);
        out += with;
        pos = found + sub.size();
        if (++done >= max_count) break;
    }
    out += s.substr(pos);
    return out;
}

std::string replace_all(const std::string& s, const std::string& sub, const std::string& with) {
    return replace(s, sub, with, -1);
}

std::vector<std::string> split_lines(const std::string& s) {
    // str.splitlines(): split on \n, \r, \r\n (no trailing empty element)
    std::vector<std::string> out;
    size_t i = 0, n = s.size();
    if (n == 0) return out;
    size_t start = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\r') {
            out.push_back(s.substr(start, i - start));
            i++;
            if (i < n && s[i] == '\n') i++;
            start = i;
        } else if (c == '\n') {
            out.push_back(s.substr(start, i - start));
            i++;
            start = i;
        } else {
            i++;
        }
    }
    if (start < n) out.push_back(s.substr(start));
    return out;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

static void append_hex(std::string& out, unsigned char c) {
    static const char* H = "0123456789abcdef";
    out.push_back(H[c >> 4]);
    out.push_back(H[c & 0xf]);
}

std::string url_encode(const std::string& s, bool encode_slash) {
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(c);
        } else if (c == '/' && !encode_slash) {
            out.push_back(c);
        } else {
            out.push_back('%');
            append_hex(out, c);
        }
    }
    return out;
}

std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() &&
            isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            out.push_back((char)std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

size_t utf8_len(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        size_t len = 1;
        if (c >= 0xf0) len = 4;
        else if (c >= 0xe0) len = 3;
        else if (c >= 0xc0) len = 2;
        i += len;
        n++;
    }
    return n;
}

std::string utf8_slice(const std::string& s, size_t from, size_t to) {
    size_t i = 0, n = 0;
    while (i < s.size() && n < from) {
        unsigned char c = s[i];
        size_t len = (c >= 0xf0) ? 4 : (c >= 0xe0) ? 3 : (c >= 0xc0) ? 2 : 1;
        i += len;
        n++;
    }
    size_t start = i;
    while (i < s.size() && n < to) {
        unsigned char c = s[i];
        size_t len = (c >= 0xf0) ? 4 : (c >= 0xe0) ? 3 : (c >= 0xc0) ? 2 : 1;
        i += len;
        n++;
    }
    return s.substr(start, i - start);
}

// ---- path helpers ---------------------------------------------------------

std::string path_join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::string home_dir() {
    if (auto h = env_get("HOME")) return *h;
    return "/root";
}

std::string path_expand_user(const std::string& s) {
    if (s == "~") return home_dir();
    if (starts_with(s, "~/")) return home_dir() + s.substr(1);
    return s;
}

bool path_is_absolute(const std::string& s) {
    return !s.empty() && s[0] == '/';
}

std::string current_dir() {
    char buf[4096];
    if (getcwd(buf, sizeof(buf))) return buf;
    return ".";
}

bool file_exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

bool dir_exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
}

long file_size(const std::string& p) {
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) return -1;
    return (long)st.st_size;
}

bool make_dirs(const std::string& p) {
    // mkdir -p semantics; returns true if p exists as a dir afterwards
    std::string cur;
    for (size_t i = 1; i < p.size(); i++) {
        cur += p[i];
        if (p[i] == '/') continue;
        if (!dir_exists(cur)) {
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
        if (p[i] == '/') continue;
    }
    return dir_exists(p);
}

std::optional<std::string> read_file_text(const std::string& p, bool* ok) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { if (ok) *ok = false; return std::nullopt; }
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (ok) *ok = true;
    return content;
}

bool write_file_text(const std::string& p, const std::string& content) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(content.data(), content.size());
    return (bool)f;
}

bool file_replace(const std::string& src, const std::string& dst) {
    return ::rename(src.c_str(), dst.c_str()) == 0;
}

std::string path_basename(const std::string& p) {
    size_t pos = p.find_last_of('/');
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

std::string path_parent(const std::string& p) {
    size_t pos = p.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return p.substr(0, pos);
}

std::string path_resolve(const std::string& p) {
    std::string s = path_expand_user(p);
    if (!path_is_absolute(s)) s = path_join(current_dir(), s);
    // normalize "." and ".." lexically (no symlink resolution; matches our needs)
    std::vector<std::string> parts;
    for (const auto& seg : [&]() {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (c == '/') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
            else cur.push_back(c);
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }()) {
        if (seg == ".") continue;
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(seg);
        }
    }
    if (parts.empty()) return path_is_absolute(s) ? "/" : ".";
    return "/" + join(parts, "/");
}

bool path_within(const std::string& candidate, const std::string& root) {
    std::string c = candidate, r = root;
    if (!r.empty() && r.back() == '/' && r != "/") r.pop_back();
    if (c == r) return true;
    if (r == "/") return c.size() > 1;
    return starts_with(c, r + "/");
}

std::optional<std::string> env_get(const std::string& name) {
    const char* v = std::getenv(name.c_str());
    if (!v) return std::nullopt;
    return std::string(v);
}

// ---- process helpers ------------------------------------------------------

bool process_alive(pid_t pid) {
#ifdef _WIN32
    DWORD code = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return false;
    bool ok = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return ok;
#else
    if (kill(pid, 0) == 0) return true;
    return errno == EPERM;  // exists but not ours -> treat as alive
#endif
}

void process_kill(pid_t pid, int sig) {
#ifdef _WIN32
    std::string cmd = "taskkill /PID " + std::to_string((int)pid) + " /F /T";
    (void)_wsystem(cmd.c_str());
#else
    (void)kill(pid, sig);
#endif
}

pid_t spawn_detached(const std::vector<std::string>& args, bool debug) {
#ifdef _WIN32
    // Break away from any job object so the child survives the agent.
    unsigned int flags = CREATE_NO_WINDOW | 0x01000000u;  // CREATE_BREAKAWAY_FROM_JOB
    std::string cmdline;
    for (size_t i = 0; i < args.size(); i++) {
        if (i) cmdline += ' ';
        const auto& a = args[i];
        if (a.find(' ') == std::string::npos && a.find('"') == std::string::npos) { cmdline += a; continue; }
        cmdline += '"';
        for (char c : a) { if (c == '"') cmdline += '"'; cmdline.push_back(c); }
        cmdline += '"';
    }
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    DWORD out_flags = flags;
    if (!CreateProcessA(nullptr, const_cast<char*>(cmdline.c_str()), nullptr, nullptr, FALSE,
                        out_flags, nullptr, nullptr, &si, &pi)) {
        out_flags &= ~0x01000000u;
        if (!CreateProcessA(nullptr, const_cast<char*>(cmdline.c_str()), nullptr, nullptr, FALSE,
                            out_flags, nullptr, nullptr, &si, &pi)) {
            return 0;
        }
    }
    pid_t child = (pid_t)pi.dwProcessId;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return child;
#else
    pid_t pid = fork();
    if (pid != 0) return pid;
    if (!debug) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2) close(devnull);
        }
        int null_in = open("/dev/null", O_RDONLY);
        if (null_in >= 0) dup2(null_in, 0);
        setsid();
    }
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(args[0].c_str(), argv.data());
    _exit(127);
    return -1;
#endif
}

int waitpid_no_retry(pid_t pid) {
#ifdef _WIN32
    return 0;
#else
    int st = 0;
    while (waitpid(pid, &st, 0) == -1) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}

}  // namespace ga
