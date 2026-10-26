#include "../src/network_recovery.h"
#include <array>
#include <cassert>
#include <iostream>

using Address = std::array<unsigned char, 4>;
struct Registers {
    const Address expectedIP{172, 16, 5, 193};
    const Address expectedGateway{172, 16, 0, 1};
    const Address expectedSubnet{255, 255, 0, 0};
    Address ip = expectedIP, gateway = expectedGateway, subnet = expectedSubnet;
    bool responds = true, writesWork = true, linkUp = true;
    unsigned restores = 0;
    bool present() { return responds; }
    bool configured() {
        return network_recovery::sameAddress(ip, expectedIP) &&
               network_recovery::sameAddress(gateway, expectedGateway) &&
               network_recovery::sameAddress(subnet, expectedSubnet);
    }
    void restore() {
        ++restores;
        if (writesWork) { ip = expectedIP; gateway = expectedGateway; subnet = expectedSubnet; }
    }
};

int main() {
    Registers port;
    assert(network_recovery::ensureConfigured(port) && port.restores == 0);
    // Observed failure: SPI and PHY link are good while IP is zero.
    port.ip = {0, 0, 0, 0};
    assert(port.linkUp && network_recovery::ensureConfigured(port));
    assert(port.ip == port.expectedIP && port.restores == 1);
    assert(network_recovery::ensureConfigured(port) && port.restores == 1);
    port.gateway = {0, 0, 0, 0};
    assert(network_recovery::ensureConfigured(port) && port.restores == 2);
    port.subnet = {255, 255, 255, 0};
    assert(network_recovery::ensureConfigured(port) && port.restores == 3);
    port.ip = {172, 20, 204, 193};
    assert(network_recovery::ensureConfigured(port) && port.restores == 4);
    port.ip = {0, 0, 0, 0};
    port.writesWork = false;
    assert(!network_recovery::ensureConfigured(port) && port.restores == 5);
    port.responds = false;
    assert(!network_recovery::ensureConfigured(port) && port.restores == 5);
    std::cout << "Network recovery: IP lost with link UP, wrong address/gateway/subnet, readback and SPI failure passed\n";
}
