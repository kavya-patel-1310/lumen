#include "lumen/server.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "lumen/document_loader.hpp"
#include "lumen/thread_pool.hpp"
#include "lumen/util.hpp"
#include "web_ui.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define LUMEN_CLOSE closesocket
static const socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using socket_t = int;
#define LUMEN_CLOSE ::close
static const socket_t kInvalidSocket = -1;
#endif

namespace lumen {
namespace {

const char* statusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        default: return "Internal Server Error";
    }
}

HttpResponse jsonError(int status, const std::string& msg) {
    return {status, "application/json; charset=utf-8", "{\"error\":\"" + jsonEscape(msg) + "\"}"};
}

size_t parseSize(const std::map<std::string, std::string>& q, const std::string& key, size_t def, size_t max) {
    auto it = q.find(key);
    if (it == q.end()) return def;
    try {
        const long long v = std::stoll(it->second);
        if (v < 0) return def;
        return std::min(static_cast<size_t>(v), max);
    } catch (...) {
        return def;
    }
}

void sendAll(socket_t s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const auto n = ::send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

}  // namespace

HttpServer::HttpServer(const InvertedIndex& index, const Searcher& searcher, std::string host, uint16_t port,
                       size_t threads)
    : index_(index), searcher_(searcher), host_(std::move(host)), port_(port), threads_(threads) {}

std::string HttpServer::urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

bool HttpServer::parseRequest(const std::string& raw, HttpRequest& out) {
    const size_t eol = raw.find("\r\n");
    if (eol == std::string::npos) return false;
    std::istringstream line(raw.substr(0, eol));
    std::string target, version;
    if (!(line >> out.method >> target >> version)) return false;
    if (version.rfind("HTTP/", 0) != 0) return false;

    const size_t qpos = target.find('?');
    out.path = urlDecode(target.substr(0, qpos));
    if (qpos != std::string::npos) {
        std::string qs = target.substr(qpos + 1);
        size_t start = 0;
        while (start <= qs.size()) {
            size_t amp = qs.find('&', start);
            if (amp == std::string::npos) amp = qs.size();
            const std::string pair = qs.substr(start, amp - start);
            const size_t eq = pair.find('=');
            if (!pair.empty()) {
                if (eq == std::string::npos) out.query[urlDecode(pair)] = "";
                else out.query[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
            }
            start = amp + 1;
        }
    }
    return true;
}

HttpResponse HttpServer::handle(const HttpRequest& req) const {
    if (req.method != "GET") return jsonError(405, "only GET is supported");

    if (req.path == "/" || req.path == "/index.html") {
        return {200, "text/html; charset=utf-8", std::string(kWebUi)};
    }

    if (req.path == "/api/health") return {200, "application/json; charset=utf-8", "{\"status\":\"ok\"}"};

    if (req.path == "/api/stats") {
        const auto s = index_.stats();
        std::ostringstream o;
        o << "{\"documents\":" << s.documents << ",\"terms\":" << s.terms << ",\"postings\":" << s.postings
          << ",\"tokens\":" << s.totalTokens << ",\"corpusBytes\":" << s.corpusBytes
          << ",\"avgDocLength\":" << s.avgDocLength << "}";
        return {200, "application/json; charset=utf-8", o.str()};
    }

    if (req.path == "/api/search") {
        auto it = req.query.find("q");
        if (it == req.query.end()) return jsonError(400, "missing q parameter");
        SearchOptions opts;
        opts.limit = parseSize(req.query, "limit", 10, 100);
        opts.offset = parseSize(req.query, "offset", 0, 10000);
        opts.snippets = SnippetStyle::Html;
        const auto r = searcher_.search(it->second, opts);

        std::ostringstream o;
        o << "{\"query\":\"" << jsonEscape(it->second) << "\",\"total\":" << r.totalMatches
          << ",\"tookMs\":" << r.elapsedMicros / 1000.0 << ",\"relaxed\":" << (r.relaxed ? "true" : "false")
          << ",\"cached\":" << (r.cached ? "true" : "false") << ",\"suggestion\":\"" << jsonEscape(r.suggestion)
          << "\",\"hits\":[";
        for (size_t i = 0; i < r.hits.size(); ++i) {
            const auto& h = r.hits[i];
            if (i) o << ',';
            o << "{\"id\":" << h.doc << ",\"score\":" << h.score << ",\"title\":\"" << jsonEscape(h.title)
              << "\",\"path\":\"" << jsonEscape(h.path) << "\",\"snippet\":\"" << jsonEscape(h.snippet) << "\"}";
        }
        o << "]}";
        return {200, "application/json; charset=utf-8", o.str()};
    }

    if (req.path == "/api/doc") {
        const size_t id = parseSize(req.query, "id", SIZE_MAX, SIZE_MAX);
        if (id >= index_.docSlots() || index_.doc(static_cast<DocId>(id)).deleted)
            return jsonError(404, "no such document");
        // Only serves files that are already in the index, never arbitrary paths.
        const auto doc = loadDocument(index_.doc(static_cast<DocId>(id)).path);
        if (!doc) return jsonError(404, "document is no longer readable");
        return {200, "text/plain; charset=utf-8", doc->text};
    }

    return jsonError(404, "not found");
}

int HttpServer::run() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif
    socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == kInvalidSocket) {
        std::perror("socket");
        return 1;
    }
    int yes = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "invalid host address: %s\n", host_.c_str());
        return 1;
    }
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::perror("bind");
        LUMEN_CLOSE(listener);
        return 1;
    }
    if (::listen(listener, 128) != 0) {
        std::perror("listen");
        LUMEN_CLOSE(listener);
        return 1;
    }
    std::printf("Lumen is serving %zu documents at http://%s:%u  (Ctrl+C to stop)\n", index_.liveDocuments(),
                host_.c_str(), static_cast<unsigned>(port_));
    std::fflush(stdout);

    ThreadPool pool(threads_);
    for (;;) {
        socket_t client = ::accept(listener, nullptr, nullptr);
        if (client == kInvalidSocket) continue;
        pool.submit([this, client] {
#ifdef _WIN32
            DWORD timeout = 5000;
#else
            timeval timeout{5, 0};
#endif
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

            std::string raw;
            char buf[4096];
            while (raw.find("\r\n\r\n") == std::string::npos && raw.size() < 16384) {
                const auto n = ::recv(client, buf, sizeof(buf), 0);
                if (n <= 0) break;
                raw.append(buf, static_cast<size_t>(n));
            }

            HttpRequest req;
            HttpResponse resp = parseRequest(raw, req) ? handle(req) : jsonError(400, "malformed request");
            std::ostringstream head;
            head << "HTTP/1.1 " << resp.status << ' ' << statusText(resp.status) << "\r\n"
                 << "Content-Type: " << resp.contentType << "\r\n"
                 << "Content-Length: " << resp.body.size() << "\r\n"
                 << "X-Content-Type-Options: nosniff\r\n"
                 << "Content-Security-Policy: default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'\r\n"
                 << "Cache-Control: no-store\r\n"
                 << "Connection: close\r\n\r\n";
            sendAll(client, head.str() + resp.body);
            LUMEN_CLOSE(client);
        });
    }
}

}  // namespace lumen
