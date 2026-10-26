#include "../src/http_response.h"
#include <cstdlib>
#include <iostream>
#include <string>

struct Reader {
    std::string input;
    size_t position = 0;
    int terminal = -1;
    Reader(const std::string &wire, int end) : input(wire), terminal(end) {}
    int read() {
        return position < input.size() ? static_cast<unsigned char>(input[position++]) : terminal;
    }
};
struct Sink {
    std::string body;
    bool append(char c) { body += c; return true; }
};
static unsigned checks = 0;
static void expect(bool condition, const char *name) {
    if (!condition) { std::cerr << "FAILED: " << name << '\n'; std::exit(1); }
    ++checks;
}
static http_response::Info parse(const std::string &wire, bool success,
                                const std::string &body, const char *name, int terminal = -1) {
    Reader reader(wire, terminal);
    Sink sink;
    http_response::Info info;
    const bool result = http_response::read(reader, sink, info);
    expect(result == success, name);
    if (success) expect(sink.body == body, name);
    else expect(info.error != nullptr, name);
    return info;
}

int main() {
    const std::string json = R"({"detail":"Device rejected by AKS"})";
    auto info = parse("HTTP/1.1 403 Forbidden\r\nServer: nginx\r\nContent-Type: application/json\r\n"
                      "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json,
                      true, json, "403 detail preserved");
    expect(info.code == 403, "actual status preserved");
    expect(std::string(info.server) == "nginx", "server captured");
    expect(std::string(info.contentType) == "application/json", "content type captured");
    const std::string matched = R"({"status":"success","faces":[{"match":true,"log_id":145}]})";
    char size[32];
    snprintf(size, sizeof(size), "%zX", matched.size());
    parse(std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n") +
          size + ";extension=yes\r\n" + matched + "\r\n0\r\nX-Trace: test\r\n\r\n",
          true, matched, "chunked recognition with trailers");
    parse("HTTP/1.1 403 Forbidden\r\nTransfer-Encoding: chunked\r\n\r\n"
          "7\r\nBlocked\r\n1\r\n!\r\n0\r\n\r\n", true, "Blocked!", "multiple chunks");
    parse("HTTP/1.0 403 Forbidden\r\nContent-Type: text/html\r\n\r\n<h1>Denied</h1>",
          true, "<h1>Denied</h1>", "close-delimited HTML error");
    parse("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}",
          true, "{}", "interim response");
    parse("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n", true, "", "empty 403");
    parse("HTTP/1.1 204 No Content\r\n\r\n", true, "", "no content", -2);
    parse("HTTP/1.1 200 OK\r\ncOnTeNt-LeNgTh: \t2 \t\r\n\r\n{}",
          true, "{}", "case insensitive headers");
    parse("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n{}", false, "", "truncated body");
    parse("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n{}", false, "", "body timeout", -2);
    parse("HTTP/1.1 200 OK\r\n\r\n{}", false, "", "timeout is not EOF", -2);
    parse("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n", false, "", "incomplete headers");
    parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n",
          false, "", "missing zero chunk");
    parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}XX0\r\n\r\n",
          false, "", "invalid chunk delimiter");
    parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\n",
          false, "", "invalid hex chunk size");
    parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
          false, "", "unsupported transfer coding");
    parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 2\r\n\r\n",
          false, "", "ambiguous framing");
    parse("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", false, "", "negative length");
    parse("HTTP/1.1 200 OK\r\nContent-Length: garbage\r\n\r\n", false, "", "non-numeric length");
    parse("HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551616\r\n\r\n",
          false, "", "overflow length");
    parse("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\n",
          false, "", "conflicting lengths");
    parse("HTTP/1.1 200 OK\r\nContent-Length: 8193\r\n\r\n", false, "", "body limit");
    parse("HTTP/1.1 200 OK\r\n\r\n" + std::string(8193, 'a'), false, "", "close body limit");
    parse("HTTP/1.1 200 OK\r\nContent-Length: 8192\r\n\r\n" + std::string(8192, 'a'),
          true, std::string(8192, 'a'), "exact body limit");
    parse("HTTP/1.1 BAD\r\n\r\n", false, "", "bad status");
    parse("HTTP/1.1 200 OK\n\n", false, "", "invalid line endings");
    parse("HTTP/1.1 200 OK\r\nX-Long: " + std::string(512, 'x') + "\r\n\r\n",
          false, "", "line limit");
    std::string tooManyHeaders = "HTTP/1.1 200 OK\r\n";
    for (unsigned i = 0; i < 50; ++i) tooManyHeaders += "X-Extra: " + std::string(200, 'x') + "\r\n";
    parse(tooManyHeaders + "\r\n", false, "", "total headers limit");
    std::cout << checks << " checks passed\n";
}
