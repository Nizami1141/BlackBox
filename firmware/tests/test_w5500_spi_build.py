"""The SPI patch is project-local, repeatable, and fails on an unknown header."""
from pathlib import Path
import runpy
import tempfile
import unittest

SCRIPT = Path(__file__).parents[1] / "tools" / "w5500_spi_build.py"
ORIGINAL = "#define SPI_ETHERNET_SETTINGS SPISettings(14000000, MSBFIRST, SPI_MODE0)\n"


class SpiPatchTest(unittest.TestCase):
    def run_patch(self, directory):
        class Env:
            def subst(self, value):
                return {"$PROJECT_LIBDEPS_DIR": directory, "$PIOENV": "board"}[value]
        runpy.run_path(str(SCRIPT), init_globals={"env": Env(), "Import": lambda _: None})

    def make_header(self, directory, content):
        path = Path(directory) / "board" / "Ethernet" / "src" / "utility" / "w5100.h"
        path.parent.mkdir(parents=True)
        path.write_text(content, encoding="utf-8")
        return path

    def test_repeated_build_keeps_the_same_patch(self):
        with tempfile.TemporaryDirectory() as directory:
            header = self.make_header(directory, "// upstream header\n" + ORIGINAL)
            self.run_patch(directory)
            first = header.read_text(encoding="utf-8")
            self.assertIn("SPISettings(ETH_SPI_CLOCK_HZ,", first)
            self.assertIn("#define ETH_SPI_CLOCK_HZ 1000000", first)
            self.run_patch(directory)
            self.assertEqual(first, header.read_text(encoding="utf-8"))

    def test_unknown_header_is_not_modified(self):
        with tempfile.TemporaryDirectory() as directory:
            header = self.make_header(directory, "// changed upstream header\n")
            with self.assertRaisesRegex(RuntimeError, "header changed"):
                self.run_patch(directory)
            self.assertEqual(header.read_text(), "// changed upstream header\n")

    def test_missing_dependency_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "was not installed"):
                self.run_patch(directory)


if __name__ == "__main__":
    unittest.main()
