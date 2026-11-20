#!/usr/bin/env python3
"""Publish corresponding sources alongside the qualified runtime binaries."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = "0.1.12"
INPUTS = {
    "webkit-source-build-20261025/webkit2gtk_2.52.6.orig.tar.xz": "179a2ea3f8f6edd4be7f31fdc55afc57bd0729f1fba648c61d4181539ac116fc",
    "webkit-source-build-20261025/webkit2gtk_2.52.6-0ubuntu0.24.04.1.dsc": None,
    "webkit-source-build-20261025/webkit2gtk_2.52.6-0ubuntu0.24.04.1.debian.tar.xz": None,
    "libsoup-comparison-20261026/libsoup-3.8.1.tar.xz": "5013dcef9136f6f861e6529fca578af50c1f52a39219027fb5aff652d1371a04",
    "libav-iteration-20261107/gst-libav-1.24.1.tar.xz": "549b7821fb5a4c866d6197888496015106cc6ec96ffb12a64e6efa91aa6562fd",
}


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True).strip():
        raise SystemExit("Commit the reviewed release before packaging corresponding sources.")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = output / f"lightview-{VERSION}-runtime-source"
    source.mkdir()
    upstream = source / "upstream"
    upstream.mkdir()
    manifest = {}
    for relative, expected in INPUTS.items():
        path = args.source_cache / relative
        actual = digest(path)
        if expected and actual != expected:
            raise SystemExit(f"Source hash mismatch: {path.name}")
        shutil.copy2(path, upstream / path.name)
        manifest[path.name] = actual
    # The signed source description records the distribution archive checksums.
    dsc = (upstream / "webkit2gtk_2.52.6-0ubuntu0.24.04.1.dsc").read_text()
    for name in ("webkit2gtk_2.52.6.orig.tar.xz", "webkit2gtk_2.52.6-0ubuntu0.24.04.1.debian.tar.xz"):
        if manifest[name] not in dsc:
            raise SystemExit(f"Distribution source checksum mismatch: {name}")
    shutil.copytree(ROOT / "runtime", source / "patches-and-build-notes")
    shutil.copytree(ROOT / "docs", source / "docs")
    for name in ("package-release.py", "package-runtime-source.py"):
        shutil.copy2(ROOT / "tools" / name, source / name)
    cache = args.source_cache / "webkit-source-build-20261025/build/CMakeCache.txt"
    options = []
    for line in cache.read_text().splitlines():
        if line.startswith(("ENABLE_", "USE_", "PORT:", "CMAKE_BUILD_TYPE:",
                            "CMAKE_C_FLAGS_RELEASE:", "CMAKE_CXX_FLAGS_RELEASE:",
                            "CMAKE_EXE_LINKER_FLAGS:", "CMAKE_SHARED_LINKER_FLAGS:",
                            "CMAKE_INSTALL_", "CMAKE_BUILD_WITH_INSTALL_RPATH:")):
            options.append(line.replace(str(args.source_cache.resolve()), "<SOURCE_CACHE>"))
    (source / "webkit-build-options.txt").write_text("\n".join(options) + "\n")
    (source / "README.txt").write_text(
        "Corresponding source for the Lightview 0.1.12 private runtime\n\n"
        "The original upstream archives and the Ubuntu WebKit source description/patches "
        "are included in upstream/. No upstream archive is relicensed under MIT.\n"
        "Extract WebKit using dpkg-source -x on the .dsc, then apply the six Lightview "
        "patches in the order recorded in patches-and-build-notes/webkitgtk-2.52.6/README.md. "
        "That source also supplies JavaScriptCore and the injected bundle.\n"
        "The qualified engine was incrementally linked from that patched build. "
        "Its WebKit process-helper path was compiled for "
        "/usr/local/lib/lightview-diagnostics/webkit-preload-timeout-v6-candidate-2.52.6-20261117. "
        "The unchanged JSC/injected-bundle build used the webkit-source-loop-2.52.6 "
        "prefix under the same parent. A full build may use its own independent "
        "prefix; configure install paths consistently with your launcher. "
        "Exact debug-byte reproducibility is not claimed.\n"
        "Extract libsoup and gst-libav and apply their respective supplied patches. "
        "Their README files contain Meson options, dependencies and build commands. "
        "WebKit's relevant CMake options are also recorded in webkit-build-options.txt. "
        "Build dependencies are supplied by Ubuntu 24.04; compilers, Python, CMake, "
        "Meson, Ninja and their normal system dependencies are not bundled.\n"
        "The Lightview application, automation tools, native regressions and release "
        "packagers are available in the matching v0.1.12 GitHub source archive. "
        "Private runtime libraries are dynamically linked and may be rebuilt/replaced "
        "without an activation key. See the license files in these source archives "
        "and LICENSES/ in the binary package.\n")
    (source / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    files = sorted(p for p in source.rglob("*") if p.is_file())
    (source / "SHA256SUMS").write_text("".join(f"{digest(p)}  {p.relative_to(source)}\n" for p in files))
    archive = output / f"{source.name}.tar.gz"
    def normalize(info):
        info.uid = info.gid = 0
        info.uname = info.gname = "root"
        return info
    with tarfile.open(archive, "w:gz") as tar:
        tar.add(source, arcname=source.name, filter=normalize)
    archive.with_name(archive.name + ".sha256").write_text(f"{digest(archive)}  {archive.name}\n")
    print(json.dumps({"archive": str(archive), "bytes": archive.stat().st_size}))


if __name__ == "__main__":
    main()
