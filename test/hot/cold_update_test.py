"""Registered cold-update owners; real dylibs and bounded strict/ASan-UBSan hosts.
Modules use UBSan only: Apple ASan image-global re-registration crashes on repeated
dlclose/dlopen of copied instrumented images. Fully ASan-instrumented modules are
an explicit failed instrumentation scope, not established by these host runs.
No network, host-wide depletion, interactive terminal or appearance acceptance.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
HOT = ROOT / "host-behavior/hotcwap"
VEX = ROOT / "host-behavior/vexspoke/src"
TEST = Path(__file__).resolve().parent


class ColdUpdateTest(unittest.TestCase):
    def test_real_updates_and_growth(self):
        for sanitized in (False, True):
            with self.subTest(sanitized=sanitized), tempfile.TemporaryDirectory(prefix="cold-update-") as directory:
                base = Path(directory)
                flags = ["-std=gnu23", "-Wall", "-Wextra", "-Werror", "-O1", "-mcpu=apple-m1",
                         "-mmacosx-version-min=14.0", f"-I{HOT}", f"-I{VEX}"]
                module_flags = list(flags)
                if sanitized:
                    flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                    module_flags += ["-fsanitize=undefined", "-fno-omit-frame-pointer"]
                for variant, defines in (("v1", ["-DVERSION=1"]), ("v2", ["-DVERSION=2"]),
                                         ("bad-abi", ["-DABI=99"]), ("bad-restore", ["-DRESTORE_OK=0"])):
                    (base / variant).mkdir()
                    for prefix in ("one", "two"):
                        subprocess.run(["clang", *module_flags, *defines, f'-DPREFIX="{prefix}"', "-dynamiclib",
                                        str(TEST / "fixtures/cold_module.c"), "-o", str(base / variant / f"{prefix}.so")],
                                       check=True, capture_output=True, timeout=60)
                sources = [HOT / "hot" / name for name in ("hot.c", "hot_trampoline.c", "hot_retire.c", "manifest.c", "ledger.c")]
                owner = base / "owner"
                subprocess.run(["clang", *flags, str(TEST / "cold_update_test.c"), *map(str, sources),
                                str(VEX / "security/crypto.c"), "-pthread", "-o", str(owner)],
                               check=True, capture_output=True, timeout=60)
                result = subprocess.run([str(owner), str(base)], capture_output=True, timeout=30,
                                        env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertIn(b"recovery passed", result.stdout)
                self.assertIn(b"cold update rejected", result.stderr)
                self.assertIn(b"rollback resolved", result.stderr)
                self.assertNotIn(b"ERROR: AddressSanitizer", result.stderr)
                self.assertNotIn(b"runtime error:", result.stderr)
                trampoline = base / "trampoline"
                subprocess.run(["clang", *flags, str(TEST / "hot_trampoline_test.c"),
                                str(HOT / "hot/hot_trampoline.c"), "-o", str(trampoline)],
                               check=True, capture_output=True, timeout=60)
                result = subprocess.run([str(trampoline)], capture_output=True, timeout=20,
                                        env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stderr.count(b"HotTrampolineTable exhausted:"), 1)


if __name__ == "__main__":
    unittest.main()
