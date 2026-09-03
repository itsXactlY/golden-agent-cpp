// test_http.cc — http-layer regression tests against an in-process TCP mini-server.
//
// Covers the two defect classes found in the 2026-09-03 audit of http.cc:
//
//   D1 (slist UAF): the header curl_slist used to be freed *before*
//      curl_easy_perform ran, leaving libcurl holding a dangling slist.
//      Fixed with the RAII CurlReq (slist freed only after the transfer).
//      Any request that completes with headers validates the lifetime.
//
//   D2/D3 (silent cap truncation): the body write-callback used to report
//      all bytes as consumed even when the response body exceeded max_body,
//      so request() returned a silently truncated body and CurlReader
//      stopped early without an error. Fixed: the callback returns 0 once the
//      cap is hit; request() throws BodyLimitExceeded; the streaming reader
//      fails with a "max_bytes" error.
//
// The mini server is a raw loopback TCP listener (no networking beyond
// 127.0.0.1, ephemeral port) so the tests run offline.

#include "http.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

namespace {

int g_checks = 0;

[[noreturn]] void fail(const std::string& msg) {
    std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
    std::exit(1);
}

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) fail(std::string(__FILE__) + ":" +                   \
                           std::to_string(__LINE__) + "  " #cond);         \
        ++g_checks;                                                       \
    } while (0)

// ---------------- minimal in-process HTTP/1.1 server ----------------

std::string big_body(std::size_t n) {
    std::string s;
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i) s.push_back(static_cast<char>('A' + (i % 26)));
    return s;
}

class MiniServer {
public:
    MiniServer() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) fail("socket() failed");
        int one = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;  // ephemeral port
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            fail("bind() failed");
        socklen_t len = sizeof(addr);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
            fail("getsockname() failed");
        port_ = ntohs(addr.sin_port);
        if (::listen(fd_, 8) != 0) fail("listen() failed");
        thread_ = std::thread([this] { serve(); });
    }

    ~MiniServer() {
        stop_ = true;
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }  // unblocks accept() -> serve() exits
        if (thread_.joinable()) thread_.join();
    }

    MiniServer(const MiniServer&) = delete;
    MiniServer& operator=(const MiniServer&) = delete;

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

private:
    int fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;

    void serve() {
        while (!stop_) {
            sockaddr_in cli{};
            socklen_t len = sizeof(cli);
            int c = ::accept(fd_, reinterpret_cast<sockaddr*>(&cli), &len);
            if (c < 0) continue;  // EBADF after close(fd_) or transient
            std::string req;
            char buf[8192];
            for (;;) {
                ssize_t n = ::recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                req.append(buf, static_cast<std::size_t>(n));
                if (req.find("\r\n\r\n") != std::string::npos) break;
            }
            std::string path;
            {
                size_t sp = req.find(' ');
                size_t e = req.find(' ', sp + 1);
                path = (sp == std::string::npos || e == std::string::npos) ? ""
                        : req.substr(sp + 1, e - sp - 1);
            }
            std::string status = "200 OK";
            std::string ct = "text/plain; charset=utf-8";
            std::string body;
            if (path == "/ok") {
                body = "hello from ga mini server";
            } else if (path == "/big") {
                ct = "application/octet-stream";
                body = big_body(1u << 20);  // 1 MiB
            } else if (path == "/404") {
                status = "404 Not Found";
                body = "not found";
            } else {
                status = "500 Internal Server Error";
                body = "unknown path";
            }
            std::string resp = "HTTP/1.1 " + status + "\r\n";
            resp += "Content-Type: " + ct + "\r\n";
            resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            resp += "Connection: close\r\n\r\n";
            resp += body;
            std::size_t off = 0;
            while (off < resp.size()) {
                ssize_t n = ::send(c, resp.data() + off, resp.size() - off, 0);
                if (n <= 0) break;
                off += static_cast<std::size_t>(n);
            }
            ::close(c);
        }
    }
};

}  // namespace

int main() {
    MiniServer srv;

    // 1. happy path: GET /ok — exercises the header-slist path end to end
    //    (regression D1: the slist must stay alive through curl_easy_perform).
    {
        ga::http::HttpResponse r = ga::http::get(srv.url() + "/ok");
        CHECK(r.status == 200);
        CHECK(r.body == "hello from ga mini server");
        CHECK(r.content_type.find("text/plain") != std::string::npos);
        CHECK(r.is_ok());
    }

    // 2. HTTP error propagation: request() throws HttpError with status 404;
    //    get() swallows it into a response (documented parity with Python).
    {
        bool threw = false;
        int code = -1;
        try {
            ga::http::request("GET", srv.url() + "/404");
        } catch (const ga::http::HttpError& e) {
            threw = true;
            code = e.status;
        }
        CHECK(threw);
        CHECK(code == 404);
        ga::http::HttpResponse r = ga::http::get(srv.url() + "/404");
        CHECK(r.status == 404);
        CHECK(!r.is_ok());
    }

    // 3. cap enforcement in request() (regression D2): a 1 MiB body under a
    //    1 KiB cap must throw BodyLimitExceeded — never a silent truncation.
    {
        bool threw = false;
        int code = -1;
        std::string msg;
        try {
            ga::http::request("GET", srv.url() + "/big", "", {}, 30, 1024);
        } catch (const ga::http::BodyLimitExceeded& e) {
            threw = true;
            code = e.status;
            msg = e.what();
        }
        CHECK(threw);
        CHECK(code == 0);  // policy violation, not a transport error
        CHECK(msg.find("max_body") != std::string::npos);
    }

    // 4. request() with a cap comfortably above the body returns the full body.
    {
        ga::http::HttpResponse r =
            ga::http::request("GET", srv.url() + "/big", "", {}, 30, (1u << 20) + 16);
        CHECK(r.status == 200);
        CHECK(r.body.size() == (1u << 20));
        CHECK(r.body == big_body(1u << 20));
    }

    // 5. streaming: full 1 MiB body through CurlReader, cap not hit.
    {
        auto rd = ga::http::curl_reader("GET", srv.url() + "/big", "", {}, 10, 30, (1u << 20) + 16);
        CHECK(rd != nullptr);
        std::string got;
        std::string chunk;
        int chunks = 0;
        while (rd->read_some(chunk)) {
            got += chunk;
            ++chunks;
        }
        CHECK(!rd->failed());
        // http_status() is populated by run() after the transfer completes,
        // so it is only reliable after the read loop has drained EOF.
        CHECK(rd->http_status() == 200);
        CHECK(got.size() == (1u << 20));
        CHECK(got == big_body(1u << 20));
        CHECK(chunks > 1);  // genuinely streamed in pieces
        rd->close();
    }

    // 6. streaming cap enforcement (regression D3): 1 KiB cap < 1 MiB body —
    //    the reader must fail with a max_bytes error, not stop silently.
    {
        auto rd = ga::http::curl_reader("GET", srv.url() + "/big", "", {}, 10, 30, 1024);
        CHECK(rd != nullptr);
        std::string got;
        std::string chunk;
        while (rd->read_some(chunk)) got += chunk;
        CHECK(rd->failed());
        CHECK(std::string(rd->error()).find("max_bytes") != std::string::npos);
        CHECK(got.size() <= 1024);  // nothing beyond the cap may be observed
        rd->close();
    }

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
