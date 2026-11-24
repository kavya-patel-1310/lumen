#pragma once
// Dependency-free HTTP/1.1 server (POSIX sockets / Winsock) exposing a JSON
// search API and a single-page web UI. Binds to localhost by default so the
// index never leaves the machine.

#include <cstdint>
#include <map>
#include <string>

#include "lumen/index.hpp"
#include "lumen/searcher.hpp"

namespace lumen {

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> query;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
};

class HttpServer {
public:
    HttpServer(const InvertedIndex& index, const Searcher& searcher, std::string host, uint16_t port,
               size_t threads = 8);
    int run();  // blocks; returns non-zero on startup failure

    // Routing is separated from socket I/O so it can be unit tested.
    HttpResponse handle(const HttpRequest& req) const;
    static bool parseRequest(const std::string& raw, HttpRequest& out);
    static std::string urlDecode(const std::string& s);

private:
    const InvertedIndex& index_;
    const Searcher& searcher_;
    std::string host_;
    uint16_t port_;
    size_t threads_;
};

}  // namespace lumen
