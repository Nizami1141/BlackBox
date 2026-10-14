#pragma once
#include "config.h"

#if ENABLE_ETH && ETH_SERVER_TLS
#include <Arduino.h>
#include <Client.h>
#include <Ethernet.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

// ESP32's native TLS engine over the existing W5500 TCP socket.
// The TCP client must outlive this wrapper.
class W5500TlsClient : public Client {
public:
    explicit W5500TlsClient(EthernetClient &tcp) : tcp_(tcp) {}
    ~W5500TlsClient() override { stop(); }
    int connect(const char *host, uint16_t port) override;
    int connect(IPAddress ip, uint16_t port) override;
    int connect(const char *host, IPAddress ip, uint16_t port);
    size_t write(uint8_t value) override { return write(&value, 1); }
    size_t write(const uint8_t *data, size_t size) override;
    int available() override;
    int read() override;
    int read(uint8_t *data, size_t size) override;
    int peek() override;
    void flush() override;
    void stop() override;
    uint8_t connected() override;
    operator bool() override { return connected(); }
    bool isSecure() const { return ready_; }
    int lastError() const { return error_; }
    String errorText() const;

private:
    W5500TlsClient(const W5500TlsClient &) = delete;
    W5500TlsClient &operator=(const W5500TlsClient &) = delete;
    static int sendBytes(void *context, const unsigned char *data, size_t size);
    static int receiveBytes(void *context, unsigned char *data, size_t size);
    bool fingerprintMatches();
    EthernetClient &tcp_;
    mbedtls_ssl_context ssl_{};
    mbedtls_ssl_config config_{};
    mbedtls_entropy_context entropy_{};
    mbedtls_ctr_drbg_context rng_{};
    bool initialized_ = false;
    bool ready_ = false;
    bool eof_ = false;
    int peeked_ = -1;
    int error_ = 0;
};
#endif
