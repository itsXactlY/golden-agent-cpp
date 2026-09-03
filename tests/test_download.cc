// test_download.cc — faithful port of Golden-Agent/tests/test_download.py
#include "download.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_checks = 0;

[[noreturn]] void fail(const std::string& msg) {
    std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
    std::exit(1);
}

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) fail(std::string(__FILE__) + ":" +                     \
                           std::to_string(__LINE__) + "  " #cond);           \
        ++g_checks;                                                        \
    } while (0)

void check_eq(const std::string& a, const std::string& b, const char* what) {
    if (a != b) {
        std::string msg = std::string(what) + ": got [" + a + "] want [" + b + "]";
        fail(msg);
    }
    ++g_checks;
}

// make_sparse_file: seek to size-1, write one zero byte.
void make_sparse_file(const fs::path& path, long long size) {
    fs::create_directories(path.parent_path());
    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) fail("open " + path.string());
    if (size > 0) {
        if (::lseek(fd, (off_t)(size - 1), SEEK_SET) == (off_t)-1 ||
            ::write(fd, "\0", 1) != 1)
            fail("sparse write " + path.string());
    }
    ::close(fd);
}

std::string file_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ---- fakes --------------------------------------------------------------
// Mirrors FakeResponse: read(n) returns ONE stored chunk per call, ignoring n.
class FakeModelReader : public ga::download::ModelReader {
public:
    FakeModelReader(std::vector<std::string> chunks, int status, bool mid_stream_fail)
        : chunks_(std::move(chunks)), status_(status), mid_stream_fail_(mid_stream_fail) {}

    int status() const override { return status_; }

    std::string read(std::size_t /*chunk_size*/) override {
        if (chunks_.empty()) {
            if (mid_stream_fail_)
                throw ga::download::TransportError("connection reset by peer");
            return std::string();
        }
        std::string c = std::move(chunks_.front());
        chunks_.erase(chunks_.begin());
        return c;
    }

    bool failed() const override { return false; }
    const std::string& error() const override { static std::string e; return e; }

private:
    std::vector<std::string> chunks_;
    int status_;
    bool mid_stream_fail_;

public:
    // Test helper: append a chunk (mirrors Python FakeResponse chunk list).
    void chunks_pub(const std::string& c) { chunks_.push_back(c); }
};

// Mirrors FakeOpener: records (url, headers), pops the next reader,
// raises AssertionError if none remain.
struct FakeOpener {
    std::vector<std::unique_ptr<ga::download::ModelReader>> responses;
    std::vector<std::pair<std::string, ga::download::HeaderVec>> calls;

    std::unique_ptr<ga::download::ModelReader> operator()(
            const std::string& url, const ga::download::HeaderVec& headers) {
        calls.emplace_back(url, headers);
        if (responses.empty())
            throw std::runtime_error("unexpected extra network call");
        auto r = std::move(responses.front());
        responses.erase(responses.begin());
        return r;
    }
};

ga::download::OpenFn opener_fn(FakeOpener* fo) {
    return [fo](const std::string& url, const ga::download::HeaderVec& h)
            -> std::unique_ptr<ga::download::ModelReader> {
        return (*fo)(url, h);
    };
}

std::pair<ga::download::OpenFn, std::unique_ptr<FakeOpener>> chunked_response(const std::string& payload) {
    auto fo = std::make_unique<FakeOpener>();
    auto reader = std::make_unique<FakeModelReader>(std::vector<std::string>(), 206, false);
    for (size_t i = 0; i < payload.size(); i += 4096)
        reader->chunks_pub(payload.substr(i, std::min<size_t>(4096, payload.size() - i)));
    fo->responses.push_back(std::move(reader));
    return std::make_pair(opener_fn(fo.get()), std::move(fo));
}

// ResumableServerFake: serves 16-byte chunks of payload[start:];
// if fail_budget set and not yet failed, serves fail_budget bytes then fails.
struct ResumableServer {
    std::string payload;
    int fail_budget = -1;
    bool failed_once = false;
    std::vector<std::pair<std::string, ga::download::HeaderVec>> calls;

    std::unique_ptr<ga::download::ModelReader> operator()(
            const std::string& url, const ga::download::HeaderVec& headers) {
        calls.emplace_back(url, headers);
        size_t start = 0;
        for (const auto& [k, v] : headers)
            if (k == "Range") {
                std::string val = v.substr(v.find('=') + 1);
                if (!val.empty() && val.back() == '-') val.pop_back();
                start = (size_t)std::stoul(val);
            }
        std::string remaining = payload.substr(start);
        std::vector<std::string> chunks;
        for (size_t i = 0; i < remaining.size(); i += 16)
            chunks.push_back(remaining.substr(i, std::min<size_t>(16, remaining.size() - i)));
        std::vector<std::string> served;
        int status = start ? 206 : 200;
        if (fail_budget >= 0 && !failed_once) {
            failed_once = true;
            size_t taken = 0;
            for (const auto& c : chunks) {
                if (taken >= (size_t)fail_budget) break;
                size_t piece = std::min(c.size(), (size_t)fail_budget - taken);
                served.push_back(c.substr(0, piece));
                taken += piece;
            }
            return std::make_unique<FakeModelReader>(std::move(served), status, true);
        }
        return std::make_unique<FakeModelReader>(std::move(chunks), status, false);
    }
};

}  // namespace

int main() {
    using ga::download::ensure_model_downloaded;
    using ga::download::model_url;
    using ga::download::model_filename;
    using ga::download::model_repo;
    using ga::download::model_size_bytes;

    const std::string FILENAME = "Ornith-1.5-9B-AD-Q4_K-IQ4_XS.gguf";
    fs::path tmp = fs::path("/tmp/test_download_dir");
    fs::remove_all(tmp);

    // ---- test_model_url_targets_huggingface_resolve ----
    {
        check_eq(model_url(), "https://huggingface.co/" + model_repo() + "/resolve/main/" +
                                 model_filename(), "model_url()");
        check_eq(model_filename(), FILENAME, "MODEL_FILENAME");
    }

    // ---- test_model_cached_true / false / mismatched redownload ----
    {
        fs::path dir = tmp / "cached";
        make_sparse_file(dir / FILENAME, model_size_bytes());
        CHECK(ga::download::model_cached(dir.string(), FILENAME, model_size_bytes()));

        fs::path dir2 = tmp / "miss";
        CHECK(!ga::download::model_cached(dir2.string(), FILENAME, model_size_bytes()));
        make_sparse_file(dir2 / FILENAME, model_size_bytes() - 1);
        CHECK(!ga::download::model_cached(dir2.string(), FILENAME, model_size_bytes()));
    }
    {
        fs::path dir = tmp / "redownload";
        make_sparse_file(dir / FILENAME, 1024);  // stale cached file, too small
        auto [opener, fo] = chunked_response(std::string(2048, 'z'));
        std::string result = ensure_model_downloaded(dir.string(), opener, model_repo(),
                                                     FILENAME, 2048);
        long long sz = (long long)fs::file_size(result);
        if (sz != 2048) fail("redownload size");
        CHECK(fo->calls.size() == 1);
        bool has_range = false;
        for (const auto& [k, v] : fo->calls[0].second)
            if (k == "Range") has_range = true;
        CHECK(!has_range);
    }

    // ---- test_resumes_partial_download_with_range_request ----
    {
        fs::path dir = tmp / "resume";
        fs::create_directories(dir);
        {
            std::ofstream o(dir / (FILENAME + ".part"), std::ios::binary);
            o.write("abcde", 5);
        }
        auto fo = std::make_unique<FakeOpener>();
        fo->responses.push_back(std::make_unique<FakeModelReader>(
            std::vector<std::string>{"fghij"}, 206, false));
        std::string result = ensure_model_downloaded(dir.string(), opener_fn(fo.get()), model_repo(),
                                                     FILENAME, 10);
        check_eq(file_bytes(result), "abcdefghij", "resumed bytes");
        bool range_ok = false;
        for (const auto& [k, v] : fo->calls[0].second)
            if (k == "Range" && v == "bytes=5-") range_ok = true;
        CHECK(range_ok);
        CHECK(!fs::exists(dir / (FILENAME + ".part")));
    }

    // ---- test_restarts_from_scratch_when_server_ignores_range ----
    {
        fs::path dir = tmp / "ignore_range";
        fs::create_directories(dir);
        {
            std::ofstream o(dir / "fake.gguf.part", std::ios::binary);
            o.write("stale", 5);
        }
        auto fo = std::make_unique<FakeOpener>();
        fo->responses.push_back(std::make_unique<FakeModelReader>(
            std::vector<std::string>{"fresh-body"}, 200, false));
        std::string result = ensure_model_downloaded(dir.string(), opener_fn(fo.get()), "r",
                                                     "fake.gguf", 10);
        check_eq(file_bytes(result), "fresh-body", "ignored-range bytes");
        bool range_ok = false;
        for (const auto& [k, v] : fo->calls[0].second)
            if (k == "Range" && v == "bytes=5-") range_ok = true;
        CHECK(range_ok);
    }

    // ---- test_raises_and_cleans_part_when_final_size_is_wrong ----
    {
        fs::path dir = tmp / "wrong_size";
        auto fo = std::make_unique<FakeOpener>();
        fo->responses.push_back(std::make_unique<FakeModelReader>(
            std::vector<std::string>{"too-short"}, 200, false));
        bool threw = false;
        try {
            ensure_model_downloaded(dir.string(), opener_fn(fo.get()), "r", "fake.gguf", 999);
        } catch (const ga::download::ModelDownloadError& e) {
            threw = true;
            if (std::string(e.what()).find("size") == std::string::npos)
                fail(std::string("no 'size' in: ") + e.what());
        }
        CHECK(threw);
        CHECK(!fs::exists(dir / "fake.gguf.part"));
    }

    // ---- test_progress_callback_reports_cumulative_bytes ----
    {
        fs::path dir = tmp / "progress";
        auto fo = std::make_unique<FakeOpener>();
        std::vector<std::string> chunks;
        chunks.push_back(std::string(10, 'a'));
        chunks.push_back(std::string(5, 'b'));
        fo->responses.push_back(std::make_unique<FakeModelReader>(chunks, 206, false));
        std::vector<std::pair<long long, long long>> observed;
        ensure_model_downloaded(dir.string(), opener_fn(fo.get()), "r", "fake.gguf", 15,
                               [&observed](long long done, long long total) {
                                   observed.push_back({done, total});
                               });
        CHECK(!observed.empty());
        CHECK(observed.back().first == 15 && observed.back().second == 15);
    }

    // ---- test_none_dest_defaults_to_cache_dir ----
    // Python monkeypatches dl.cache_models_dir to return `cache`; in C++ we
    // mirror that by pointing XDG_CACHE_HOME at `cache` and placing the cached
    // file where the real cache_models_dir() resolves (base/golden-agent/models).
    {
        fs::path cache = tmp / "cache-models";
        fs::path models = cache / "golden-agent" / "models";
        make_sparse_file(models / FILENAME, model_size_bytes());
        setenv("XDG_CACHE_HOME", cache.c_str(), 1);
        auto fo = std::make_unique<FakeOpener>();  // empty -> any call raises
        std::string result = ensure_model_downloaded("", opener_fn(fo.get()), model_repo(),
                                                     FILENAME, model_size_bytes());
        check_eq(result, (models / FILENAME).string(), "cache-dir default");
    }

    // ---- test_retries_mid_stream_failure_and_resumes_from_offset ----
    {
        fs::path dir = tmp / "mid_stream";
        std::string payload;
        for (int i = 0; i < 10; ++i) payload += "0123456789";
        ResumableServer server{payload, 40, false, {}};
        ga::download::OpenFn opener = [&server](const std::string& url,
                                                 const ga::download::HeaderVec& h) {
            return server(url, h);
        };
        std::string result = ensure_model_downloaded(dir.string(), opener, model_repo(),
                                                     FILENAME, (long long)payload.size(), {},
                                                     1024, 5, [](double) {});
        check_eq(file_bytes(result), payload, "mid-stream resume bytes");
        CHECK(server.calls.size() == 2);
        bool range_ok = false;
        for (const auto& [k, v] : server.calls[1].second)
            if (k == "Range" && v == "bytes=40-") range_ok = true;
        CHECK(range_ok);
    }

    // ---- test_backs_off_exponentially_between_retries ----
    {
        fs::path dir = tmp / "backoff";
        ga::download::OpenFn opener = [](const std::string&, const ga::download::HeaderVec&)
                -> std::unique_ptr<ga::download::ModelReader> {
            throw ga::download::TransportError("down");
        };
        std::vector<double> sleeps;
        bool threw = false;
        try {
            ensure_model_downloaded(dir.string(), opener, "r", "fake.gguf", 100, {},
                                   1024, 4, [&sleeps](double s) { sleeps.push_back(s); });
        } catch (const ga::download::ModelDownloadError& e) {
            threw = true;
            if (std::string(e.what()).find("attempts") == std::string::npos)
                fail(std::string("no 'attempts' in: ") + e.what());
        }
        CHECK(threw);
        CHECK(sleeps.size() == 3);
        if (sleeps.size() == 3) {
            CHECK(sleeps[0] == 1.0 && sleeps[1] == 2.0 && sleeps[2] == 4.0);
        }
    }

    // ---- test_keeps_partial_file_when_retries_are_exhausted ----
    {
        fs::path dir = tmp / "keep_partial";
        fs::create_directories(dir);
        fs::path partial = dir / "fake.gguf.part";
        {
            std::ofstream o(partial, std::ios::binary);
            o.write("keep-me", 7);
        }
        ga::download::OpenFn opener = [](const std::string&, const ga::download::HeaderVec&)
                -> std::unique_ptr<ga::download::ModelReader> {
            throw ga::download::TransportError("down");
        };
        bool threw = false;
        try {
            ensure_model_downloaded(dir.string(), opener, "r", "fake.gguf", 100, {},
                                   1024, 2, [](double) {});
        } catch (const ga::download::ModelDownloadError& e) {
            threw = true;
        }
        CHECK(threw);
        check_eq(file_bytes(partial), "keep-me", "partial kept");
    }

    // ---- test_client_errors_fail_fast_without_retry ----
    {
        fs::path dir = tmp / "client_err";
        std::vector<int> callcount;
        std::vector<double> sleeps;
        ga::download::OpenFn opener = [&callcount](const std::string&,
                                                   const ga::download::HeaderVec&)
                -> std::unique_ptr<ga::download::ModelReader> {
            callcount.push_back(1);
            throw ga::download::HttpStatusError("HTTP 403", 403);
        };
        bool threw = false;
        try {
            ensure_model_downloaded(dir.string(), opener, "r", "fake.gguf", 100, {},
                                   1024, 5, [&sleeps](double s) { sleeps.push_back(s); });
        } catch (const ga::download::HttpStatusError&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(callcount.size() == 1);
        CHECK(sleeps.empty());
    }

    // ---- test_stale_part_larger_than_expected_restarts_from_scratch ----
    {
        fs::path dir = tmp / "stale_part";
        fs::create_directories(dir);
        {
            std::ofstream o(dir / (FILENAME + ".part"), std::ios::binary);
            o.write(std::string(999, 'x').data(), 999);
        }
        std::string payload = "fresh-data";
        ResumableServer server{payload, -1, false, {}};
        ga::download::OpenFn opener = [&server](const std::string& url,
                                                 const ga::download::HeaderVec& h) {
            return server(url, h);
        };
        std::string result = ensure_model_downloaded(dir.string(), opener, model_repo(),
                                                     FILENAME, (long long)payload.size(), {},
                                                     1024, 5, [](double) {});
        bool has_range = false;
        for (const auto& [k, v] : server.calls[0].second)
            if (k == "Range") has_range = true;
        CHECK(!has_range);
        check_eq(file_bytes(result), payload, "stale-part restart bytes");
    }

    fs::remove_all(tmp);
    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
