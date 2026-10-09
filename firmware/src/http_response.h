#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Framework-independent HTTP framing. Reader::read() returns a byte, -1 on
// connection close, or -2 on timeout; Sink::append() reports allocation failure.
namespace http_response {

constexpr size_t MAX_BODY = 8192;
constexpr size_t MAX_HEADERS = 8192;
constexpr size_t MAX_LINE = 512;

struct Info {
    int code = 0;
    char server[96] = {};
    char contentType[96] = {};
    const char *error = nullptr;
};

inline bool fail(Info &info, const char *message) {
    info.error = message;
    return false;
}

inline char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

inline bool equalIgnoreCase(const char *a, const char *b) {
    while (*a && *b && lower(*a) == lower(*b)) { ++a; ++b; }
    return *a == *b;
}

inline char *trim(char *text) {
    while (*text == ' ' || *text == '\t') ++text;
    size_t len = strlen(text);
    while (len && (text[len - 1] == ' ' || text[len - 1] == '\t')) text[--len] = 0;
    return text;
}

inline void copyHeader(char *target, size_t size, const char *value) {
    const size_t len = strlen(value) < size - 1 ? strlen(value) : size - 1;
    memcpy(target, value, len);
    target[len] = 0;
}

template <typename Reader>
bool line(Reader &reader, char *target, size_t &headerBytes, Info &info) {
    size_t used = 0;
    while (true) {
        const int c = reader.read();
        if (c < 0) return fail(info, c == -2 ? "response timeout" : "incomplete response line");
        if (++headerBytes > MAX_HEADERS) return fail(info, "headers too large");
        if (c == '\r') {
            const int next = reader.read();
            if (next < 0) return fail(info, next == -2 ? "response timeout" : "incomplete CRLF");
            if (++headerBytes > MAX_HEADERS) return fail(info, "headers too large");
            if (next != '\n') return fail(info, "invalid CRLF");
            target[used] = 0;
            return true;
        }
        if (c == '\n' || c == 0) return fail(info, "invalid response line");
        if (used >= MAX_LINE - 1) return fail(info, "response line too long");
        target[used++] = static_cast<char>(c);
    }
}

inline bool decimal(const char *value, size_t &number) {
    if (!*value) return false;
    number = 0;
    while (*value) {
        if (*value < '0' || *value > '9') return false;
        const size_t digit = static_cast<size_t>(*value++ - '0');
        if (number > (SIZE_MAX - digit) / 10) return false;
        number = number * 10 + digit;
    }
    return true;
}

inline bool chunkSize(const char *value, size_t &number) {
    number = 0;
    bool any = false;
    while (*value && *value != ';') {
        const char c = lower(*value++);
        const int digit = c >= '0' && c <= '9' ? c - '0' :
                          c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (digit < 0 || number > (SIZE_MAX - static_cast<size_t>(digit)) / 16) return false;
        any = true;
        number = number * 16 + static_cast<size_t>(digit);
    }
    return any;
}

template <typename Reader, typename Sink>
bool bytes(Reader &reader, Sink &body, size_t count, size_t &total, Info &info) {
    if (count > MAX_BODY - total) return fail(info, "response body too large");
    for (size_t i = 0; i < count; ++i) {
        const int c = reader.read();
        if (c < 0) return fail(info, c == -2 ? "response timeout" : "incomplete response body");
        if (!body.append(static_cast<char>(c))) return fail(info, "response allocation failed");
        ++total;
    }
    return true;
}

template <typename Reader, typename Sink>
bool read(Reader &reader, Sink &body, Info &info) {
    char buffer[MAX_LINE];
    size_t headerBytes = 0;
    size_t length = 0;
    bool hasLength = false;
    bool chunked = false;
    for (unsigned interim = 0; ; ++interim) {
        if (interim > 4) return fail(info, "too many interim responses");
        if (!line(reader, buffer, headerBytes, info)) return false;
        if ((strncmp(buffer, "HTTP/1.1 ", 9) && strncmp(buffer, "HTTP/1.0 ", 9)) ||
            strlen(buffer) < 12 || buffer[9] < '1' || buffer[9] > '5' ||
            buffer[10] < '0' || buffer[10] > '9' || buffer[11] < '0' || buffer[11] > '9' ||
            (buffer[12] != 0 && buffer[12] != ' ')) return fail(info, "invalid HTTP status");
        info.code = (buffer[9] - '0') * 100 + (buffer[10] - '0') * 10 + buffer[11] - '0';
        info.server[0] = info.contentType[0] = 0;
        hasLength = chunked = false;
        while (true) {
            if (!line(reader, buffer, headerBytes, info)) return false;
            if (!buffer[0]) break;
            char *colon = strchr(buffer, ':');
            if (!colon) return fail(info, "invalid response header");
            *colon = 0;
            char *value = trim(colon + 1);
            if (equalIgnoreCase(buffer, "Content-Length")) {
                size_t parsed = 0;
                if (!decimal(value, parsed) || (hasLength && parsed != length))
                    return fail(info, "invalid Content-Length");
                length = parsed;
                hasLength = true;
            } else if (equalIgnoreCase(buffer, "Transfer-Encoding")) {
                if (chunked || !equalIgnoreCase(value, "chunked"))
                    return fail(info, "unsupported Transfer-Encoding");
                chunked = true;
            } else if (equalIgnoreCase(buffer, "Server")) {
                copyHeader(info.server, sizeof(info.server), value);
            } else if (equalIgnoreCase(buffer, "Content-Type")) {
                copyHeader(info.contentType, sizeof(info.contentType), value);
            }
        }
        if (info.code >= 200) break;
        if (info.code == 101) return fail(info, "protocol upgrade unsupported");
    }
    if (info.code == 204 || info.code == 304) return true;
    if (chunked && hasLength) return fail(info, "conflicting response framing");
    size_t total = 0;
    if (chunked) {
        while (true) {
            if (!line(reader, buffer, headerBytes, info)) return false;
            size_t count = 0;
            if (!chunkSize(buffer, count)) return fail(info, "invalid chunk size");
            if (!count) {
                do {
                    if (!line(reader, buffer, headerBytes, info)) return false;
                } while (buffer[0]);
                return true;
            }
            if (!bytes(reader, body, count, total, info)) return false;
            const int cr = reader.read();
            const int lf = reader.read();
            if (cr != '\r' || lf != '\n') return fail(info, "incomplete chunk delimiter");
        }
    }
    if (hasLength) return bytes(reader, body, length, total, info);
    while (true) {
        const int c = reader.read();
        if (c == -1) return true;
        if (c < 0) return fail(info, "response timeout");
        if (total == MAX_BODY) return fail(info, "response body too large");
        if (!body.append(static_cast<char>(c))) return fail(info, "response allocation failed");
        ++total;
    }
}

}  // namespace http_response
