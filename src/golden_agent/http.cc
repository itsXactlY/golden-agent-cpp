#include "golden_agent/http.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <curl/curl.h>

namespace ga::http {

namespace {

std::once_flag g_curl_init;

void curl_init_once() { std::call_once(g_curl_init, [] { curl_global_init(CURL_GLOBAL_ALL); }); }

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
// libcurl 8.x header-callback ABI: return type is size_t and the function must
// return the number of bytes handled (size * nmemb). Returning 0 aborts the
// transfer with CURLE_WRITE_ERROR — the old "return 0" style from pre-8 libcurl
// is fatal here.
size_t header_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t total = size * nmemb;
    auto* st = static_cast<std::pair<std::map<std::string, std::string>*, std::string*>*>(userdata);
    std::string line(ptr, total);
    if (line.size() >= 2 && line.substr(line.size() - 2) == "\r\n") line.erase(line.size() - 2);
    if (!line.empty() && line.back() == '\n') line.pop_back();
    if (line.empty()) return total;
    if (line.size() >= 5 && line.compare(0, 5, "HTTP/") == 0) {
        // status line: "HTTP/1.1 200 OK"
        auto sp = line.find(' ', 5);
        if (sp != std::string::npos) {
            st->first->emplace("__status__", line.substr(sp + 1));
        }
        return total;
    }
    auto colon = line.find(':');
    if (colon == std::string::npos) return total;
    std::string key = lower(line.substr(0, colon));
    std::string val = line.substr(colon + 1);
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(0, 1);
    if (key == "content-type") *st->second = val;
    (*st->first)[key] = val;
    return total;
}

std::string header_line(const std::pair<std::string, std::string>& h) {
    return h.first + ": " + h.second;
}

// One libcurl transfer unit. The request-header slist must outlive
// curl_easy_perform — libcurl reads it during the transfer — so it is kept
// alongside the handle and both are released together, after the transfer.
// (Freeing the slist immediately after setopt is a use-after-free.)
struct CurlReq {
    CURL* handle = nullptr;
    curl_slist* headers = nullptr;
    CurlReq() = default;
    ~CurlReq() {
        if (headers) curl_slist_free_all(headers);
        if (handle) curl_easy_cleanup(handle);
    }
    CurlReq(const CurlReq&) = delete;
    CurlReq& operator=(const CurlReq&) = delete;
    CurlReq(CurlReq&& o) noexcept : handle(o.handle), headers(o.headers) {
        o.handle = nullptr;
        o.headers = nullptr;
    }
    CurlReq& operator=(CurlReq&& o) noexcept {
        if (this != &o) {
            if (headers) curl_slist_free_all(headers);
            if (handle) curl_easy_cleanup(handle);
            handle = o.handle;
            headers = o.headers;
            o.handle = nullptr;
            o.headers = nullptr;
        }
        return *this;
    }
};

CurlReq make_handle(const std::string& method, const std::string& url, const std::string& body,
                    const HeaderVec& headers, int connect_timeout, int read_timeout,
                    std::size_t max_bytes, size_t(*writefn)(char*, size_t, size_t, void*), void* userdata) {
    CurlReq req;
    req.handle = curl_easy_init();
    if (!req.handle) throw HttpError("curl init failed", 0);
    CURL* c = req.handle;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "golden-agent/0.1");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, (long)connect_timeout);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)(read_timeout > 0 ? read_timeout : 3600));
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 120L);
    if (method != "GET" && !method.empty()) {
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!body.empty()) {
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
        }
    }
    if (!headers.empty()) {
        curl_slist* list = nullptr;
        for (const auto& h : headers) list = curl_slist_append(list, header_line(h).c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
        req.headers = list;  // ownership transfers; ~CurlReq frees it post-transfer
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writefn);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, userdata);
    (void)max_bytes;
    return req;
}

}  // namespace

// ---------------------------------------------------------------------------
// one-shot request
// ---------------------------------------------------------------------------
namespace {

struct CapState {
    std::string body;
    std::size_t cap;
    bool over = false;
    std::size_t received = 0;
};

size_t write_cap(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* st = static_cast<CapState*>(userdata);
    size_t total = size * nmemb;
    size_t room = st->cap - st->received;
    if (total > room) {
        st->body.append(ptr, room);
        st->received += room;
        st->over = true;
        // Returning 0 (not `total`) tells libcurl the write failed and
        // aborts the transfer with CURLE_WRITE_ERROR — the only correct way
        // to enforce a hard cap. Returning bytes not actually consumed
        // violates the write-callback contract.
        return 0;
    }
    st->body.append(ptr, total);
    st->received += total;
    return total;
}

}  // namespace

HttpResponse request(const std::string& method, const std::string& url, const std::string& body,
                     const HeaderVec& extra_headers, int timeout_sec, std::size_t max_body) {
    curl_init_once();
    CapState st{std::string{}, max_body, false, 0};
    std::map<std::string, std::string> headers;
    std::string content_type;
    auto* hud = new std::pair<std::map<std::string, std::string>*, std::string*>(&headers, &content_type);

    CurlReq req = make_handle(method, url, body, extra_headers, 10, timeout_sec, max_body, write_cap, &st);
    curl_easy_setopt(req.handle, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(req.handle, CURLOPT_HEADERDATA, hud);

    CURLcode rc = curl_easy_perform(req.handle);
    const bool over = st.over;
    // req is destroyed at scope exit: slist + handle freed after the transfer.
    delete hud;

    if (over) throw BodyLimitExceeded("response body exceeds max_body limit (" +
                                      std::to_string(max_body) + " bytes)");
    if (rc != CURLE_OK) throw HttpError(std::string("transport error: ") + curl_easy_strerror(rc), 0);
    long status = 0;
    auto it = headers.find("__status__");
    if (it != headers.end()) status = std::stol(it->second);
    if (status >= 400) throw HttpError("HTTP " + std::to_string(status), (int)status);
    HttpResponse out;
    out.status = (int)status;
    out.headers = headers;
    out.body = std::move(st.body);
    out.content_type = content_type;
    return out;
}

HttpResponse get(const std::string& url, const HeaderVec& extra_headers, int timeout) {
    try {
        return request("GET", url, "", extra_headers, timeout, SIZE_MAX);
    } catch (const HttpError& e) {
        HttpResponse out;
        out.status = e.status;
        out.body = e.what();
        return out;
    }
}

// ---------------------------------------------------------------------------
// streaming reader: data is pushed into the buffer as it arrives, so
// consuming is pull-based (not consuming genuinely halts compute).
// ---------------------------------------------------------------------------
namespace {

struct CurlReader : Reader {
    std::unique_ptr<std::thread> thread;
    mutable std::mutex mu;
    std::condition_variable cv;
    std::string buf;
    bool done = false;
    std::size_t max_bytes;
    std::size_t received = 0;
    int code = 0;

    explicit CurlReader(std::size_t cap) : max_bytes(cap) {}

    static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* self = static_cast<CurlReader*>(userdata);
        size_t total = size * nmemb;
        std::lock_guard lk(self->mu);
        size_t room = self->max_bytes - self->received;
        if (total > room) {
            if (room < total) {
                self->buf.append(ptr, room);
                self->received += room;
            }
            self->over_cap_flag = true;
            self->cv.notify_all();
            // Returning 0 aborts the transfer with CURLE_WRITE_ERROR:
            // hard cap, not a silent truncation (which would corrupt a
            // partial download while still reporting "complete").
            return 0;
        }
        self->buf.append(ptr, total);
        self->received += total;
        self->cv.notify_all();
        return total;
    }

    void run(const std::string& method, const std::string& url, const std::string& body,
             const HeaderVec& headers, int connect_timeout, int read_timeout) {
        std::map<std::string, std::string> headers_map;
        std::string ct_local;
        auto* hud = new std::pair<std::map<std::string, std::string>*, std::string*>(&headers_map, &ct_local);
        CURLcode rc = CURLE_OK;
        bool init_failed = false;
        {
            CurlReq req;
            try {
                req = make_handle(method, url, body, headers, connect_timeout, read_timeout,
                                 max_bytes, write_cb, this);
            } catch (const HttpError&) {
                init_failed = true;
            }
            if (!init_failed) {
                curl_easy_setopt(req.handle, CURLOPT_HEADERFUNCTION, header_cb);
                curl_easy_setopt(req.handle, CURLOPT_HEADERDATA, hud);
                rc = curl_easy_perform(req.handle);
            }
            // req destroyed here: slist + handle freed after the transfer.
        }
        delete hud;
        std::lock_guard lk(mu);
        auto it = headers_map.find("__status__");
        if (it != headers_map.end()) code = std::atoi(it->second.c_str());
        ct = std::move(ct_local);
        if (init_failed) {
            failed_ = true;
            error_ = "curl init failed";
        } else if (over_cap_flag) {
            failed_ = true;
            error_ = "response body exceeds max_bytes limit (" + std::to_string(max_bytes) + " bytes)";
        } else if (rc != CURLE_OK) {
            failed_ = true;
            error_ = std::string("transport error: ") + curl_easy_strerror(rc);
        }
        done = true;
        cv.notify_all();
    }

public:
    std::string ct;
    bool over_cap_flag = false;

    bool cap_exceeded() const override {
        std::lock_guard lk(mu);
        return over_cap_flag;
    }
    int status() const {
        std::lock_guard lk(mu);
        return code;
    }
    int http_status() const override { return status(); }
    std::string content_type_header() const override { return content_type_now(); }
    std::thread* raw_thread() override { return thread ? thread.get() : nullptr; }
    std::string content_type_now() const {
        std::lock_guard lk(mu);
        return ct;
    }
    // Blocks until data is available or the transfer finishes.
    // Returns false only at EOF (or transport failure).
    bool read_some(std::string& out) override {
        std::unique_lock lk(mu);
        while (buf.empty() && !done) cv.wait_for(lk, std::chrono::milliseconds(50));
        if (buf.empty()) return false;
        out = std::move(buf);
        buf.clear();
        return true;
    }
};

}  // namespace

std::unique_ptr<Reader> curl_reader(const std::string& method, const std::string& url,
                                    const std::string& body, const HeaderVec& extra_headers,
                                    int connect_timeout, int read_timeout, std::size_t max_bytes) {
    curl_init_once();
    auto r = std::make_unique<CurlReader>(max_bytes);
    CurlReader* raw = r.get();
    r->thread = std::make_unique<std::thread>([raw, method, url, body, extra_headers,
                                             connect_timeout, read_timeout] {
        raw->run(method, url, body, extra_headers, connect_timeout, read_timeout);
    });
    return r;
}

Reader::Reader(bool) {}
Reader::~Reader() {
    if (std::thread* t = raw_thread(); t && t->joinable()) t->join();
}
void Reader::close() {
    if (std::thread* t = raw_thread(); t && t->joinable()) t->join();
}
void Reader::fail(const std::string& msg, int status) {
    failed_ = true;
    error_ = msg;
    (void)status;
}

bool LineReader::next(std::string& line) {
    for (;;) {
        auto nl = buf_.find('\n');
        if (nl != std::string::npos) {
            line = buf_.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            buf_.erase(0, nl + 1);
            return true;
        }
        std::string chunk;
        if (!r_.read_some(chunk)) {
            if (buf_.empty()) return false;
            line = std::move(buf_);
            buf_.clear();
            return true;
        }
        buf_ += std::move(chunk);
    }
}

}  // namespace ga::http
