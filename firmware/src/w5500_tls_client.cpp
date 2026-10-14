#include "w5500_tls_client.h"
#if ENABLE_ETH && ETH_SERVER_TLS
#include <Dns.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/sha1.h>
#include <algorithm>
#include <cstring>

namespace {
constexpr uint32_t HANDSHAKE_TIMEOUT_MS = 20000;
constexpr uint32_t WRITE_TIMEOUT_MS = 45000;
constexpr size_t TCP_CHUNK_BYTES = 1024;
}

int W5500TlsClient::sendBytes(void *context, const unsigned char *data, size_t size) {
    auto &self = *static_cast<W5500TlsClient *>(context);
    if (!self.tcp_.connected()) return MBEDTLS_ERR_NET_CONN_RESET;
    // Ethernet 2.0.2 may report the requested size after truncating writes
    // to its socket buffer. Never pass it more than one safe-sized chunk.
    const size_t count = self.tcp_.write(data, std::min(size, TCP_CHUNK_BYTES));
    return count ? static_cast<int>(count) : MBEDTLS_ERR_NET_SEND_FAILED;
}

int W5500TlsClient::receiveBytes(void *context, unsigned char *data, size_t size) {
    auto &self = *static_cast<W5500TlsClient *>(context);
    const int count = self.tcp_.available();
    if (count <= 0)
        return self.tcp_.connected() ? MBEDTLS_ERR_SSL_WANT_READ : 0;
    // Arduino-ESP32 2.x SPI bulk IO casts its buffers to uint32_t*. TLS
    // record offsets are not necessarily aligned. Receive into an aligned
    // scratch buffer, then copy exactly the bytes returned by Ethernet.
    alignas(4) uint8_t scratch[TCP_CHUNK_BYTES];
    const int received = self.tcp_.read(scratch,
        std::min(std::min(size, static_cast<size_t>(count)), TCP_CHUNK_BYTES));
    if (received > 0) memcpy(data, scratch, received);
    return received > 0 ? received : MBEDTLS_ERR_NET_RECV_FAILED;
}

int W5500TlsClient::connect(const char *host, uint16_t port) {
    DNSClient dns;
    IPAddress ip;
    dns.begin(Ethernet.dnsServerIP());
    if (dns.getHostByName(host, ip, 3000) != 1) {
        error_ = MBEDTLS_ERR_NET_UNKNOWN_HOST;
        return 0;
    }
    return connect(host, ip, port);
}

int W5500TlsClient::connect(IPAddress ip, uint16_t port) {
    const String host = ip.toString();
    return connect(host.c_str(), ip, port);
}

int W5500TlsClient::connect(const char *host, IPAddress ip, uint16_t port) {
    stop();
    error_ = 0;
    if (!tcp_.connect(ip, port)) {
        error_ = MBEDTLS_ERR_NET_CONNECT_FAILED;
        return 0;
    }
    mbedtls_ssl_init(&ssl_);
    mbedtls_ssl_config_init(&config_);
    mbedtls_entropy_init(&entropy_);
    mbedtls_ctr_drbg_init(&rng_);
    initialized_ = true;
    const char personalization[] = "blackbox-w5500-tls";
    int result = mbedtls_ctr_drbg_seed(&rng_, mbedtls_entropy_func, &entropy_,
        reinterpret_cast<const unsigned char *>(personalization), sizeof(personalization) - 1);
    if (!result) result = mbedtls_ssl_config_defaults(&config_, MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (!result) {
        mbedtls_ssl_conf_rng(&config_, mbedtls_ctr_drbg_random, &rng_);
        mbedtls_ssl_conf_min_version(&config_, MBEDTLS_SSL_MAJOR_VERSION_3,
                                    MBEDTLS_SSL_MINOR_VERSION_3);
        mbedtls_ssl_conf_max_version(&config_, MBEDTLS_SSL_MAJOR_VERSION_3,
                                    MBEDTLS_SSL_MINOR_VERSION_3);
        // Preserve the existing development mode or explicit fingerprint pin.
        // Fingerprint verification happens before ready_ allows application IO.
        mbedtls_ssl_conf_authmode(&config_, MBEDTLS_SSL_VERIFY_NONE);
        result = mbedtls_ssl_setup(&ssl_, &config_);
    }
    if (!result) result = mbedtls_ssl_set_hostname(&ssl_, host); // SNI
    if (!result) {
        mbedtls_ssl_set_bio(&ssl_, this, sendBytes, receiveBytes, nullptr);
        const uint32_t started = millis();
        do {
            result = mbedtls_ssl_handshake(&ssl_);
            if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE)
                break;
            if (millis() - started >= HANDSHAKE_TIMEOUT_MS) {
                result = MBEDTLS_ERR_SSL_TIMEOUT;
                break;
            }
            delay(1);
        } while (true);
    }
    if (!result && !fingerprintMatches()) result = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    if (result) {
        error_ = result;
        stop();
        return 0;
    }
    ready_ = true;
    eof_ = false;
    return 1;
}

bool W5500TlsClient::fingerprintMatches() {
#if ETH_TLS_INSECURE
    return true;
#else
    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&ssl_);
    if (!peer) return false;
    unsigned char actual[20];
    if (mbedtls_sha1_ret(peer->raw.p, peer->raw.len, actual)) return false;
    const char *text = ETH_TLS_FINGERPRINT;
    unsigned char expected[20] = {};
    size_t digits = 0;
    for (; *text; ++text) {
        const char ch = *text;
        if (ch == ' ' || ch == ':' || ch == '\t') continue;
        int value = ch >= '0' && ch <= '9' ? ch - '0' :
                    ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 :
                    ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
        if (value < 0 || digits >= 40) return false;
        expected[digits / 2] = static_cast<unsigned char>((expected[digits / 2] << 4) | value);
        ++digits;
    }
    return digits == 40 && memcmp(actual, expected, sizeof(actual)) == 0;
#endif
}

size_t W5500TlsClient::write(const uint8_t *data, size_t size) {
    if (!ready_) return 0;
    size_t sent = 0;
    const uint32_t started = millis();
    while (sent < size) {
        const int result = mbedtls_ssl_write(&ssl_, data + sent,
                                           std::min(size - sent, TCP_CHUNK_BYTES));
        if (result > 0) { sent += result; continue; }
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (millis() - started < WRITE_TIMEOUT_MS) { delay(1); continue; }
            error_ = MBEDTLS_ERR_SSL_TIMEOUT;
        } else error_ = result;
        return sent;
    }
    return sent;
}

int W5500TlsClient::available() {
    if (peeked_ >= 0) return 1;
    if (!ready_ || eof_) return 0;
    if (!mbedtls_ssl_get_bytes_avail(&ssl_) && !tcp_.available()) return 0;
    unsigned char value;
    const int result = mbedtls_ssl_read(&ssl_, &value, 1);
    if (result == 1) { peeked_ = value; return 1; }
    if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    if (result < 0 && result != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) error_ = result;
    eof_ = true;
    return 0;
}

int W5500TlsClient::read() {
    if (!available()) return -1;
    const int value = peeked_;
    peeked_ = -1;
    return value;
}

int W5500TlsClient::read(uint8_t *data, size_t size) {
    size_t copied = 0;
    while (copied < size && available()) data[copied++] = static_cast<uint8_t>(read());
    return static_cast<int>(copied);
}

int W5500TlsClient::peek() { return available() ? peeked_ : -1; }
void W5500TlsClient::flush() { tcp_.flush(); }
uint8_t W5500TlsClient::connected() {
    return ready_ && (peeked_ >= 0 || (!eof_ &&
        (mbedtls_ssl_get_bytes_avail(&ssl_) || tcp_.connected())));
}

void W5500TlsClient::stop() {
    if (ready_ && initialized_) mbedtls_ssl_close_notify(&ssl_);
    ready_ = false;
    eof_ = false;
    peeked_ = -1;
    tcp_.stop();
    if (initialized_) {
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&config_);
        mbedtls_ctr_drbg_free(&rng_);
        mbedtls_entropy_free(&entropy_);
        initialized_ = false;
    }
}

String W5500TlsClient::errorText() const {
    char message[160];
    mbedtls_strerror(error_, message, sizeof(message));
    return String(message);
}
#endif
