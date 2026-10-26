#pragma once

// Compare the registers actually read from the controller, not a cached IP.
namespace network_recovery {
template <typename Address>
bool sameAddress(const Address &actual, const Address &expected) {
    for (unsigned i = 0; i < 4; ++i)
        if (actual[i] != expected[i]) return false;
    return true;
}

// A controller can lose configuration without its PHY link going down.
// Re-read after restoring: failed SPI writes must not be reported as success.
template <typename Port>
bool ensureConfigured(Port &port) {
    if (!port.present()) return false;
    if (port.configured()) return true;
    port.restore();
    return port.present() && port.configured();
}
}  // namespace network_recovery
