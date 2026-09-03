#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ga {

// ---- string helpers -------------------------------------------------------

std::string trim(const std::string& s);
std::string lstrip(const std::string& s);
bool starts_with(const std::string& s, const std::string& prefix);
bool ends_with(const std::string& s, const std::string& suffix);
int count_substring(const std::string& s, const std::string& sub);
std::string replace(const std::string& s, const std::string& sub, const std::string& with,
                    int max_count);
std::string replace_all(const std::string& s, const std::string& sub, const std::string& with);
std::vector<std::string> split_lines(const std::string& s);  // str.splitlines
std::string join(const std::vector<std::string>& parts, const std::string& sep);

// percent-encoding / decoding (UTF-8 aware)
std::string url_encode(const std::string& s, bool encode_slash = true);
std::string url_decode(const std::string& s);

// UTF-8 helpers
size_t utf8_len(const std::string& s);
std::string utf8_slice(const std::string& s, size_t from, size_t to);

// ---- path helpers ---------------------------------------------------------

std::string path_join(const std::string& a, const std::string& b);
std::string path_expand_user(const std::string& s);
bool path_is_absolute(const std::string& s);
std::string home_dir();
std::string current_dir();
bool file_exists(const std::string& p);
bool dir_exists(const std::string& p);
long file_size(const std::string& p);  // -1 if missing
bool make_dirs(const std::string& p);
std::optional<std::string> read_file_text(const std::string& p, bool* ok = nullptr);
bool write_file_text(const std::string& p, const std::string& content);
bool file_replace(const std::string& src, const std::string& dst);  // os.replace
std::string path_basename(const std::string& p);
std::string path_parent(const std::string& p);
// resolve ~ and relative parts; does not require the path to exist
std::string path_resolve(const std::string& p);
// is `candidate` inside `root` (relative_to semantics)
bool path_within(const std::string& candidate, const std::string& root);

// ---- environment ----------------------------------------------------------
std::optional<std::string> env_get(const std::string& name);

// ---- process helpers (POSIX + Windows) ------------------------------------
bool process_alive(pid_t pid);
void process_kill(pid_t pid, int sig = 9);
// spawn a child; returns its pid (0 on failure). When `new_session` the child
// starts its own session (setsid) and gets detached stdio unless `debug`
// (debug: inherit stdout/stderr so logs are visible).
pid_t spawn_detached(const std::vector<std::string>& args, bool debug = false);
int waitpid_no_retry(pid_t pid);

}  // namespace ga
