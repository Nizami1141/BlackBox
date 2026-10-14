"""Make the pinned Arduino Ethernet SPI clock configurable before compilation.

Ethernet 2.0.2 hard-codes 14 MHz in w5100.h, overriding command-line defines.
Patch only the project-local dependency header; no global packages change.
"""
from pathlib import Path

Import("env")

header = (Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") /
          "Ethernet" / "src" / "utility" / "w5100.h")
original = "#define SPI_ETHERNET_SETTINGS SPISettings(14000000, MSBFIRST, SPI_MODE0)"
replacement = """// BLACKBOX_CONFIGURABLE_ETHERNET_SPI
#ifndef ETH_SPI_CLOCK_HZ
#define ETH_SPI_CLOCK_HZ 1000000
#endif
#define SPI_ETHERNET_SETTINGS SPISettings(ETH_SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0)"""

if not header.is_file():
    raise RuntimeError(f"Pinned Ethernet dependency was not installed: {header}")
source = header.read_text(encoding="utf-8")
if original in source:
    header.write_text(source.replace(original, replacement, 1), encoding="utf-8")
elif "BLACKBOX_CONFIGURABLE_ETHERNET_SPI" not in source:
    raise RuntimeError("Ethernet SPI header changed; review the pinned library before building")
