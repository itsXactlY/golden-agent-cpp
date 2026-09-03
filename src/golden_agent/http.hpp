#pragma once
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
namespace ga::http {

using HeaderVec = std::vector<std::pair<std::string, std::string>>;

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;  // lower-cased keys
    std::string body;
    std::string content_type;  // e.g. "text/html"

    bool is_ok() const { return status >= 200 && status < 300; }
};

class HttpError : public std::runtime_error {
public:
    int status;
    HttpError(const std::string& msg, int code) : std::runtime_error(msg), status(code) {}
};

// One-shot request. Throws HttpError on HTTP >= 400 (like urllib raising
// HTTPError) or transport failure. Redirects are followed.
HttpResponse request(const std::string& method, const std::string& url,
                     const std::string& body = "", const HeaderVec& extra_headers = {},
                     int timeout_sec = 30, std::size_t max_body = SIZE_MAX);

// Blocking streaming response. Data is pulled in a background thread;
// read_some() returns chunks until EOF.
class Reader {
public:
    int status = 0;
    std::string content_type;
    explicit Reader(bool owned = true);
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    virtual ~Reader();
    // Returns false at EOF. `out` receives raw bytes (possibly empty at EOF).
    virtual bool read_some(std::string& out) = 0;
    virtual int http_status() const { return 0; }
    virtual std::string content_type_header() const { return ""; }
    // Access to the underlying transfer thread (for joining on destruction).
    virtual std::thread* raw_thread() { return nullptr; }
    void close();
    bool failed() const { return failed_; }
    const std::string& error() const { return error_; }

protected:
    void fail(const std::string& msg, int status = 0);
    bool failed_ = false;
    std::string error_;
};

// libcurl-backed reader
std::unique_ptr<Reader> curl_reader(const std::string& method, const std::string& url,
                                    const std::string& body = "", const HeaderVec& extra_headers = {},
                                    int connect_timeout = 10, int read_timeout = 300,
                                    std::size_t max_bytes = SIZE_MAX);

// Convenience: blocking GET with bounded body (returns body even on 404)
HttpResponse get(const std::string& url, const HeaderVec& extra_headers = {}, int timeout = 20);

// Line iteration helper over a Reader (SSE / generic)
class LineReader {
public:
    explicit LineReader(Reader& r) : r_(r) {}
    bool next(std::string& line);  // false at EOF; line without trailing \n
private:
    Reader& r_;
    std::string buf_;
};

}  // namespace ga::http
