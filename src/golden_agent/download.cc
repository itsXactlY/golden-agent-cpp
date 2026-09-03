// download.cc — faithful re-port of Golden-Agent/src/golden_agent/download.py
// (drifted snapshot, 2026-09-02).
#include "download.hpp"

#include <cmath>
#include <chrono>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <optional>
#include <thread>
#include "config.hpp"
#include "http.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace ga::download {

// ---- catalogue-derived constants ----------------------------------------
// Python: _spec = MODELS[DEFAULT_MODEL_KEY]; MODEL_REPO = _spec.repo; etc.
// (importlib.metadata version is not needed for the port).
namespace {
const ga::ModelSpec& default_spec() {
    const auto& v = ga::models();
    for (const auto& m : v)
        if (m.key == ga::DEFAULT_MODEL_KEY) return m;
    throw std::runtime_error("default model key not in MODELS");
}
}  // namespace

const std::string& model_repo() { return default_spec().repo; }
const std::string& model_filename() { return default_spec().filename; }
long long model_size_bytes() { return default_spec().size_bytes; }

// model_url(repo = MODEL_REPO, filename = MODEL_FILENAME)
std::string model_url(const std::string& repo, const std::string& filename) {
    return "https://huggingface.co/" + (repo.empty() ? model_repo() : repo)
         + "/resolve/main/" + (filename.empty() ? model_filename() : filename);
}

// ---- default opener -----------------------------------------------------
namespace {

// Adapter around ga::http::Reader presenting the urllib.response surface:
// .status and .read(n) with sub-chunk buffering (urllib.read(n) returns AT
// MOST n bytes and keeps the remainder buffered).
class CurlModelReader : public ModelReader {
public:
    explicit CurlModelReader(std::unique_ptr<ga::http::Reader> reader)
        : r_(std::move(reader)) {}

    int status() const override { return r_->http_status(); }

    std::string read(std::size_t chunk_size) override {
        if (residual_.empty()) {
            std::string out;
            if (!r_->read_some(out)) {
                if (r_->failed()) {
                    if (r_->cap_exceeded()) {
                        // Local policy violation, not a transport problem:
                        // no retry can fix it.
                        throw ModelDownloadError("response body exceeded "
                                                 "max_bytes limit: " + r_->error());
                    }
                    transport_error_ = r_->error();
                    throw TransportError(transport_error_);
                }
                return std::string();  // clean EOF
            }
            residual_ = std::move(out);
        }
        if (residual_.size() <= chunk_size) {
            std::string out = std::move(residual_);
            residual_.clear();
            return out;
        }
        std::string out = residual_.substr(0, chunk_size);
        residual_.erase(0, chunk_size);
        return out;
    }

    bool failed() const override { return r_->failed(); }
    const std::string& error() const override {
        if (!transport_error_.empty()) return transport_error_;
        return r_->error();
    }

private:
    std::unique_ptr<ga::http::Reader> r_;
    std::string residual_;
    std::string transport_error_;
};

// urlopen equivalent: wait for the status line, then hand back the stream.
std::unique_ptr<ModelReader> default_open(const std::string& url, const HeaderVec& headers) {
    auto r = ga::http::curl_reader("GET", url, "", headers, 10, 86400);
    // curl_reader is async: poll until the status line is in (or the
    // transport fails).
    int status = 0;
    for (int i = 0; i < 3000; ++i) {
        if (r->failed()) {
            std::string err = r->error();
            r->close();
            throw TransportError(err.empty() ? "transport error" : err);
        }
        status = r->http_status();
        if (status != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (status == 0) {
        r->close();
        throw TransportError("no HTTP status received");
    }
    if (status >= 400) {
        r->close();
        throw HttpStatusError("HTTP " + std::to_string(status), status);
    }
    return std::make_unique<CurlModelReader>(std::move(r));
}

std::string host_platform() {
#if defined(__APPLE__)
    return "darwin";
#elif defined(_WIN32)
    return "win32";
#else
    return "linux";
#endif
}

std::optional<std::string> env_get(const std::map<std::string, std::string>& env,
                                   const char* key) {
    if (!env.empty()) {
        auto it = env.find(key);
        if (it == env.end() || it->second.empty()) return std::nullopt;
        return it->second;
    }
    const char* v = std::getenv(key);
    if (!v || *v == '\0') return std::nullopt;
    return v;
}

}  // namespace

OpenFn default_opener() {
    return [](const std::string& url, const HeaderVec& headers) -> std::unique_ptr<ModelReader> {
        return default_open(url, headers);
    };
}

// ---- is_retryable --------------------------------------------------------
bool is_retryable(const std::exception& error) {
    if (const auto* h = dynamic_cast<const HttpStatusError*>(&error))
        return h->code == 429 || h->code >= 500;
    if (dynamic_cast<const TransportError*>(&error))
        return true;
    return false;
}

// ---- stream_attempt ------------------------------------------------------
long long stream_attempt(const OpenFn& opener,
                         const std::string& url,
                         const std::string& part_path,
                         const std::string& dest_dir,
                         long long expected_size,
                         const ProgressFn& progress,
                         std::size_t chunk_size) {
    // offset = 0; if part_path.exists(): offset = stat().st_size
    long long offset = 0;
    if (fs::exists(part_path)) offset = (long long)fs::file_size(part_path);
    if (offset > expected_size) offset = 0;

    HeaderVec headers;
    if (offset != 0)
        headers.push_back({"Range", "bytes=" + std::to_string(offset) + "-"});

    // dest_dir.mkdir(parents=True, exist_ok=True)
    std::error_code ec;
    fs::create_directories(dest_dir, ec);

    auto response = opener(url, headers);
    if (response->status() != 206) offset = 0;
    const bool append = offset != 0;

    std::ofstream sink(part_path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!sink) {
        fs::remove(part_path, ec);
        throw ModelDownloadError("cannot open " + part_path + " for writing");
    }

    long long written = offset;
    std::string chunk;
    while (!(chunk = response->read(chunk_size)).empty()) {
        sink.write(chunk.data(), chunk.size());
        written += (long long)chunk.size();
        if (progress) progress(written, expected_size);
    }
    sink.close();
    if (progress) progress(written, expected_size);

    if (written != expected_size) {
        fs::remove(part_path, ec);  // unlink(missing_ok=True)
        throw ModelDownloadError("size mismatch for " + fs::path(part_path).filename().string()
                                 + ": downloaded " + std::to_string(written)
                                 + " bytes, expected " + std::to_string(expected_size));
    }
    return written;
}

// ---- model_cached --------------------------------------------------------
bool model_cached(const std::string& dest_dir, const std::string& filename,
                   long long expected_size) {
    std::string dir = dest_dir.empty() ? cache_models_dir() : dest_dir;
    fs::path dest = fs::path(dir) / filename;
    std::error_code ec;
    if (!fs::is_regular_file(dest, ec)) return false;
    return (long long)fs::file_size(dest, ec) == expected_size;
}

// ---- cache_models_dir ----------------------------------------------------
std::string cache_models_dir(const std::string& platform,
                             const std::map<std::string, std::string>& env) {
    std::string plat = platform.empty() ? host_platform() : platform;
    fs::path base;
    if (plat == "win32") {
        auto v = env_get(env, "LOCALAPPDATA");
        base = v ? fs::path(*v) : fs::path(ga::home_dir()) / "AppData" / "Local";
    } else if (plat == "darwin") {
        auto v = env_get(env, "HOME");
        base = fs::path(v ? *v : ga::home_dir()) / "Library" / "Caches";
    } else {
        auto v = env_get(env, "XDG_CACHE_HOME");
        base = v ? fs::path(*v) : fs::path(ga::home_dir()) / ".cache";
    }
    return (base / "golden-agent" / "models").string();
}

// ---- ensure_model_downloaded ---------------------------------------------
std::string ensure_model_downloaded(
    const std::string& dest_dir,
    const OpenFn& opener,
    const std::string& repo,
    const std::string& filename,
    long long expected_size,
    const ProgressFn& progress,
    std::size_t chunk_size,
    int max_attempts,
    const SleeperFn& sleeper) {
    std::string final_dir = dest_dir.empty() ? cache_models_dir() : dest_dir;
    if (model_cached(final_dir, filename, expected_size))
        return (fs::path(final_dir) / filename).string();

    // Python: opener = opener or _http_opener
    OpenFn open = opener ? opener : default_opener();
    std::string url = model_url(repo, filename);
    std::string part_path = (fs::path(final_dir) / (filename + ".part")).string();

    // Python retry loop: for attempt in range(1, max_attempts + 1)
    std::string last_error;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        try {
            stream_attempt(open, url, part_path, final_dir, expected_size,
                           progress, chunk_size);
            break;
        } catch (const std::exception& error) {
            if (!is_retryable(error))
                throw;
            last_error = error.what();
            if (attempt < max_attempts) {
                double wait = std::min(BACKOFF_BASE_SECONDS * std::pow(2.0, (double)(attempt - 1)),
                                       BACKOFF_CAP_SECONDS);
                if (sleeper)
                    sleeper(wait);
                else
                    std::this_thread::sleep_for(std::chrono::milliseconds((long)(wait * 1000.0)));
            } else {
                throw ModelDownloadError("download of " + filename + " failed after "
                                         + std::to_string(max_attempts)
                                         + " attempts; partial data kept at " + part_path
                                         + " for resume (" + last_error + ")");
            }
        }
    }
    std::error_code ec;
    fs::rename(part_path, fs::path(final_dir) / filename, ec);
    if (ec)
        throw ModelDownloadError("cannot move " + part_path + " to "
                                 + (fs::path(final_dir) / filename).string()
                                 + " (" + ec.message() + ")");
    return (fs::path(final_dir) / filename).string();
}

std::string ensure_default_model(const std::string& dest_dir,
                                 const ProgressFn& progress,
                                 const SleeperFn& sleeper) {
    return ensure_model_downloaded(dest_dir, default_opener(), model_repo(),
                                   model_filename(), model_size_bytes(),
                                   progress, CHUNK_SIZE, MAX_ATTEMPTS, sleeper);
}

// ensure_draft_model_downloaded(dest_dir, *, repo, filename, expected_size,
// progress): wraps ensure_model_downloaded with the default opener.
std::string ensure_draft_model_downloaded(const std::string& dest_dir,
                                          const std::string& repo,
                                          const std::string& filename,
                                          long long expected_size,
                                          const ProgressFn& progress) {
    return ensure_model_downloaded(dest_dir, default_opener(), repo, filename,
                                   expected_size, progress, CHUNK_SIZE, MAX_ATTEMPTS, {});
}

}  // namespace ga::download
