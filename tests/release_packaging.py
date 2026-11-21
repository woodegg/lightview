"""Exercise installer preflight with real ELF files and no system installation."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("package_release", ROOT / "tools/package-release.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class InstallerPreflight(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.geteuid() == 0:
            raise RuntimeError("Run packaging checks as an unprivileged user.")
        if package.host_platform() not in ("ubuntu24.04", "ubuntu26.04"):
            raise RuntimeError("These native installer checks require supported Ubuntu.")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="lightview-package-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in ("bin", "libexec", "runtime-root"):
            (self.root / name).mkdir()
        for name in ("bin/lightview", "libexec/lightview-bin"):
            shutil.copy2("/usr/bin/true", self.root / name)
        self.script = self.root / "install.sh"
        self.script.write_text(package.installer(package.host_platform()))
        self.script.chmod(0o755)

    def checksum(self):
        paths = sorted(p for p in self.root.rglob("*") if p.is_file() and p.name != "SHA256SUMS")
        (self.root / "SHA256SUMS").write_text("".join(
            f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(self.root)}\n"
            for p in paths))

    def run_check(self):
        env = {k: v for k, v in os.environ.items() if k != "DESTDIR"}
        env["LC_ALL"] = "C"
        return subprocess.run([str(self.script), "--check"], env=env,
                              capture_output=True, text=True)

    def test_other_distribution_asset_is_rejected_before_checksum_or_install(self):
        other = "ubuntu26.04" if package.host_platform() == "ubuntu24.04" else "ubuntu24.04"
        self.script.write_text(package.installer(other))
        result = self.run_check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Download the matching Ubuntu archive", result.stderr)
        self.assertFalse((self.root / "SHA256SUMS").exists())

    def test_check_only_validates_without_creating_installation_files(self):
        self.checksum()
        before = sorted(str(p.relative_to(self.root)) for p in self.root.rglob("*"))
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("package checks passed", result.stdout)
        self.assertEqual(before, sorted(str(p.relative_to(self.root)) for p in self.root.rglob("*")))

    def test_corrupt_binary_is_rejected(self):
        self.checksum()
        with (self.root / "libexec/lightview-bin").open("ab") as stream:
            stream.write(b"corrupt-package")
        result = self.run_check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("FAILED", result.stdout + result.stderr)

    def test_missing_transitive_elf_dependency_is_rejected(self):
        build = self.root / "compiler"
        build.mkdir()
        source = build / "dependency.c"
        source.write_text("int lightview_dependency(void) { return 7; }\n")
        missing = f"liblightview-test-missing-{os.getpid()}.so.1"
        library = build / "libdependency.so"
        subprocess.run(["cc", "-shared", "-fPIC", str(source), "-o", str(library),
                        f"-Wl,-soname,{missing}"], check=True)
        source.write_text("extern int lightview_dependency(void);\n"
                          "int consumer(void) { return lightview_dependency(); }\n")
        consumer = self.root / "runtime-root/consumer.so"
        subprocess.run(["cc", "-shared", "-fPIC", str(source), str(library),
                        "-o", str(consumer)], check=True)
        self.checksum()
        result = self.run_check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(missing, result.stderr)
        self.assertIn("Missing runtime dependencies", result.stderr)


if __name__ == "__main__":
    unittest.main()
