// Re-ported from Golden-Agent/src/golden_agent/download.py
// (drifted snapshot, 2026-09-02). Chunked download with HTTP-Range resume,
// size validation against the MODELS catalogue entry, and retry/backoff on
// 429/5xx + transport errors.
#pragma once
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ga::download {

// ---- Module constants (identical to download.py) ------------------------
constexpr std::size_t CHUNK_SIZE = 1024 * 1024;
constexpr int MAX_ATTEMPTS = 5;
constexpr double BACKOFF_BASE_SECONDS = 1.0;
constexpr double BACKOFF_CAP_SECONDS = 30.0;

// Model-file constants derived from MODELS[DEFAULT_MODEL_KEY]. The Python
// module derives them from the catalogue; here they are the pinned values.
const std::string& model_repo();
const std::string& model_filename();
long long model_size_bytes();

// model_url: "https://huggingface.co/{repo}/resolve/main/{filename}".
std::string model_url(const std::string& repo = "", const std::string& filename = "");

// ---- Errors (mirror the Python exception hierarchy) ---------------------
// Mirrors download.ModelDownloadError (size mismatch, fatal).
class ModelDownloadError : public std::runtime_error {
public:
    explicit ModelDownloadError(const std::string& msg) : std::runtime_error(msg) {}
};
// Mirrors urllib.error.HTTPError: server answered >= 400.
class HttpStatusError : public std::runtime_error {
public:
    int code;
    HttpStatusError(const std::string& msg, int c) : std::runtime_error(msg), code(c) {}
};
// Mirrors OSError / http.client.HTTPException (transport-level failure).
class TransportError : public std::runtime_error {
public:
    explicit TransportError(const std::string& msg) : std::runtime_error(msg) {}
};

// ---- Opened-response handle (mirrors urllib.response usage) --------------
// download.py consumes `response.status` and `response.read(n)`.
struct ModelReader {
    virtual ~ModelReader() = default;
    virtual int status() const = 0;
    // Up to `chunk_size` bytes; "" at EOF.
    virtual std::string read(std::size_t chunk_size) = 0;
    virtual bool failed() const = 0;
    virtual const std::string& error() const = 0;
};

using HeaderVec = std::vector<std::pair<std::string, std::string>>;
// urllib.request.Request + urlopen stand-in.
using OpenFn = std::function<std::unique_ptr<ModelReader>(const std::string& url,
                                                           const HeaderVec& headers)>;
using ProgressFn = std::function<void(long long, long long)>;
using SleeperFn = std::function<void(double)>;

// Default opener: libcurl-backed urlopen equivalent. Follows redirects
// (HuggingFace 302 -> CDN), raises HttpStatusError on HTTP >= 400,
// TransportError on transport failure.
OpenFn default_opener();

// True for HttpStatusError(429/5xx) and TransportError; anything else
// (e.g. ModelDownloadError) is fatal.
bool is_retryable(const std::exception& error);

// _stream_attempt: opens `url` via `opener`, streams to `part_path`.
// A .part larger than expected_size is treated as stale -> full restart.
// Throws ModelDownloadError on size mismatch; propagates opener errors.
long long stream_attempt(const OpenFn& opener,
                         const std::string& url,
                         const std::string& part_path,
                         const std::string& dest_dir,
                         long long expected_size,
                         const ProgressFn& progress,
                         std::size_t chunk_size = CHUNK_SIZE);

// model_cached: True when `filename` exists in dest_dir with exactly
// expected_size bytes (dest_dir == "" -> cache_models_dir()).
bool model_cached(const std::string& dest_dir, const std::string& filename,
                   long long expected_size);

// ensure_model_downloaded: mirrors download.py line for line.
// dest_dir == "" -> cache_models_dir(). Returns the final dest path.
// Pass an empty opener to use the default libcurl opener.
std::string ensure_model_downloaded(
    const std::string& dest_dir,
    const OpenFn& opener,
    const std::string& repo,
    const std::string& filename,
    long long expected_size,
    const ProgressFn& progress = {},
    std::size_t chunk_size = CHUNK_SIZE,
    int max_attempts = MAX_ATTEMPTS,
    const SleeperFn& sleeper = {});

// Convenience: downloads the DEFAULT_MODEL_KEY model with the catalogue
// constants and default_opener().
std::string ensure_default_model(const std::string& dest_dir = "",
                                 const ProgressFn& progress = {},
                                 const SleeperFn& sleeper = {});

// ensure_draft_model_downloaded: mirrors download.py (downloads a draft
// model GGUF if not already cached); no retry policy beyond the defaults.
std::string ensure_draft_model_downloaded(const std::string& dest_dir,
                                          const std::string& repo,
                                          const std::string& filename,
                                          long long expected_size,
                                          const ProgressFn& progress = {});

// cache_models_dir: platform cache root (mirrors the Python function).
// platform == "" -> this host's platform. env: explicit map replaces the
// process environment (full replacement; missing key -> unset); empty map
// -> real process environment.
std::string cache_models_dir(const std::string& platform = "",
                             const std::map<std::string, std::string>& env = {});

}  // namespace ga::download
