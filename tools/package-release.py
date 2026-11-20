#!/usr/bin/env python3
"""Package the qualified runtime without changing its long-tested ELF bytes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = "0.1.12"
DIAGNOSTICS = Path("/usr/local/lib/lightview-diagnostics")
ENGINE = "webkit-preload-timeout-v6-candidate-2.52.6-20261117"
BASE = "webkit-source-loop-2.52.6"
SOUP = "libsoup-redirect-budget-3.8.1-20261118"
AV = "libav-width-guard-v3-1.24.1"
PINNED = {
    f"{ENGINE}/lib/libwebkit2gtk-4.1.so.0.21.10": "b44a25c0e0c07f3bdef491bd98737d2ff892fd3c21c7d8ab16b54e537598efe9",
    f"{SOUP}/lib/libsoup-3.0.so.0.8.1": "37902860b806188489065d3a2576362fe3e0927fb29ea5c11cf6a2d92f68cfc5",
    f"{AV}/lib/gstreamer-1.0/libgstlibav.so": "3cce94f75f71552a976a68cce8c29e6eff233ff1d9ffb2d235e3f03e6ff8392d",
}

INSTALLER = '''#!/bin/sh
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
dest=${DESTDIR:-}
sandbox=false
case ${1:-} in
  '') ;;
  --sandbox-launcher) sandbox=true ;;
  *) echo 'Usage: [DESTDIR=staging-root] ./install.sh [--sandbox-launcher]' >&2; exit 2 ;;
esac
if [ -z "$dest" ] && [ "$(id -u)" != 0 ]; then
  echo 'Use sudo ./install.sh, or set DESTDIR for an unprivileged staging install.' >&2; exit 1
fi
if "$sandbox" && { [ -n "$dest" ] || [ "$(id -u)" != 0 ]; }; then
  echo 'The optional setuid launcher requires a real root install without DESTDIR.' >&2; exit 1
fi
sha256sum --check --quiet SHA256SUMS
# Never overwrite a different runtime already installed at a qualified path.
find runtime-root -type f | while IFS= read -r source; do
  target="$dest/${source#runtime-root/}"
  if [ -e "$target" ] && ! cmp -s "$source" "$target"; then
    echo "Conflicting private runtime: $target" >&2; exit 1
  fi
done
mkdir -p "$dest/usr/local"
# Existing matching files can be memory-mapped by running browsers. Never
# truncate or rewrite them, even during a reinstall of identical bytes.
find runtime-root -mindepth 1 -type d | while IFS= read -r source; do
  mkdir -p "$dest/${source#runtime-root/}"
done
find runtime-root -type f | while IFS= read -r source; do
  target="$dest/${source#runtime-root/}"
  if [ ! -e "$target" ]; then install -m644 "$source" "$target"; fi
  case "$target" in */libexec/*) chmod 755 "$target" ;; esac
done
find runtime-root -type l | while IFS= read -r source; do
  target="$dest/${source#runtime-root/}"
  wanted=$(readlink "$source")
  if [ -L "$target" ]; then
    [ "$(readlink "$target")" = "$wanted" ] || { echo "Conflicting runtime link: $target" >&2; exit 1; }
  elif [ -e "$target" ]; then
    echo "Conflicting runtime link: $target" >&2; exit 1
  else ln -s "$wanted" "$target"; fi
done
install -Dm755 bin/lightview "$dest/usr/local/bin/lightview"
install -Dm755 bin/lightviewctl "$dest/usr/local/bin/lightviewctl"
install -Dm755 bin/lightview-memory "$dest/usr/local/bin/lightview-memory"
install -Dm755 libexec/lightview-bin "$dest/usr/local/libexec/lightview-bin"
install -Dm755 libexec/lightview-sandbox-launcher "$dest/usr/local/libexec/lightview-sandbox-launcher"
cp -a share/. "$dest/usr/local/share/"
if [ "$(id -u)" = 0 ]; then
  for directory in runtime-root/usr/local/lib/lightview-diagnostics/*; do
    chown -R root:root "$dest/usr/local/lib/lightview-diagnostics/${directory##*/}"
  done
  chown root:root "$dest/usr/local/bin/lightview" "$dest/usr/local/libexec/lightview-bin" "$dest/usr/local/libexec/lightview-sandbox-launcher"
fi
if "$sandbox"; then chmod 4755 /usr/local/libexec/lightview-sandbox-launcher; fi
echo 'Installed Lightview 0.1.12 and its qualified private runtime.'
'''


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source-cache", type=Path, required=True,
                        help="directory containing the qualified upstream source trees")
    parser.add_argument("--stage-only", action="store_true",
                        help="prepare and checksum a package for installation QA without compressing")
    args = parser.parse_args()
    dirty = bool(subprocess.check_output(
        ["git", "status", "--porcelain"], cwd=ROOT, text=True).strip())
    if dirty and not args.stage_only:
        raise SystemExit("Commit the reviewed release first; use --stage-only for worktree QA.")
    built_version = subprocess.check_output(
        [str(ROOT / "build/lightview"), "--version"], text=True,
        stderr=subprocess.PIPE).strip()
    if not built_version.startswith(f"Lightview {VERSION} ("):
        raise SystemExit(f"Build version does not match the release: {built_version}")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    package = output / f"lightview-{VERSION}"
    package.mkdir()  # Preserve earlier release artifacts; never reuse a package directory.
    for name, expected in PINNED.items():
        if digest(DIAGNOSTICS / name) != expected:
            raise SystemExit(f"Runtime hash mismatch: {name}")
    runtime = package / "runtime-root/usr/local/lib/lightview-diagnostics"
    for name in (ENGINE, SOUP, AV):
        shutil.copytree(DIAGNOSTICS / name, runtime / name, symlinks=True)
    # Only the matching JSC and injected bundle are needed from the base build.
    base = DIAGNOSTICS / BASE
    for name in ("lib/libjavascriptcoregtk-4.1.so.0.10.14",
                 "lib/webkit2gtk-4.1/injected-bundle/libwebkit2gtkinjectedbundle.so"):
        copy(base / name, runtime / BASE / name)
    jsc = runtime / BASE / "lib"
    (jsc / "libjavascriptcoregtk-4.1.so.0").symlink_to("libjavascriptcoregtk-4.1.so.0.10.14")
    (jsc / "libjavascriptcoregtk-4.1.so").symlink_to("libjavascriptcoregtk-4.1.so.0")
    for source, target in (("build/lightview-runtime-launcher", "bin/lightview"),
                           ("build/lightview", "libexec/lightview-bin"),
                           ("build/lightview-sandbox-launcher", "libexec/lightview-sandbox-launcher"),
                           ("tools/lightviewctl", "bin/lightviewctl"),
                           ("tools/lightview-memory", "bin/lightview-memory"),
                           ("data/lightview.desktop", "share/applications/lightview.desktop")):
        copy(ROOT / source, package / target)
    for size in (16, 24, 32, 48, 64, 128, 256, 512):
        source = ROOT / ("data/lightview.png" if size == 512 else f"data/icons/lightview-{size}.png")
        copy(source, package / f"share/icons/hicolor/{size}x{size}/apps/lightview.png")
    for name in ("README.md", "CHANGELOG.md", "LICENSE", "SECURITY.md"):
        copy(ROOT / name, package / name)
    shutil.copytree(ROOT / "runtime", package / "runtime")
    shutil.copytree(ROOT / "docs", package / "docs")
    source_trees = {
        "webkit": args.source_cache / "webkit-source-build-20261025/webkit2gtk-2.52.6",
        "libsoup": args.source_cache / "libsoup-comparison-20261026/libsoup-3.8.1",
        "gst-libav": args.source_cache / "libav-iteration-20261107/gst-libav-1.24.1",
    }
    for component, source in source_trees.items():
        if not source.is_dir():
            raise SystemExit(f"Missing corresponding source tree: {source}")
        licenses = [p for p in source.rglob("*") if p.is_file()
                    and (p.name.startswith("COPYING") or p.name.startswith("LICENSE"))]
        if not licenses:
            raise SystemExit(f"Missing upstream licenses: {component}")
        for path in licenses:
            copy(path, package / "LICENSES" / component / path.relative_to(source))
    (package / "THIRD-PARTY-NOTICES.txt").write_text(
        "Lightview is MIT licensed. WebKitGTK/JavaScriptCore, libsoup and gst-libav "
        "retain their upstream LGPL and other applicable licenses; see LICENSES/.\n"
        "Corresponding upstream source archives, distribution patches, Lightview "
        "patches and build instructions are provided alongside this binary as "
        f"lightview-{VERSION}-runtime-source.tar.gz in the same GitHub release.\n"
        "These are dynamically linked private libraries. Rebuild/relink them using "
        "the supplied sources and instructions; no signature or activation key "
        "is required to run modified versions. System libraries retain their "
        "distribution's licenses.\n")
    for name, content in (("install.sh", INSTALLER),):
        target = package / name
        target.write_text(content)
        target.chmod(0o755)
    for path in package.rglob("*"):
        if path.is_file() and not path.is_symlink():
            path.chmod(0o755 if os.access(path, os.X_OK) else 0o644)
    manifest = {
        "version": VERSION, "platform": "Ubuntu 24.04 x86_64",
        "source_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "source_worktree_dirty": dirty,
        "application_license": "MIT; bundled components retain their upstream licenses",
        "runtime_files": {str(p.relative_to(package)): digest(p)
                          for p in sorted(runtime.rglob("*")) if p.is_file() and not p.is_symlink()},
        "qualification": {"run_id": "20261118-063038", "duration_seconds": 86402.473,
                          "healthy_navigations": 11957, "sites": 17, "mode": "Normal"},
        "installation_prefix": "/usr/local",
        "runtime_paths": "Compiled private prefixes are preserved; system libraries are not replaced.",
    }
    (package / "runtime-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    checksums = [f"{digest(p)}  {p.relative_to(package)}\n" for p in sorted(package.rglob("*"))
                 if p.is_file() and not p.is_symlink()]
    (package / "SHA256SUMS").write_text("".join(checksums))
    if args.stage_only:
        print(json.dumps({"package": str(package), "staged": True}))
        return
    archive = output / f"lightview-{VERSION}-ubuntu24.04-x86_64.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        tar.add(package, arcname=package.name, filter=lambda info: normalized(info))
    archive.with_name(archive.name + ".sha256").write_text(f"{digest(archive)}  {archive.name}\n")
    print(json.dumps({"package": str(package), "archive": str(archive), "bytes": archive.stat().st_size}))


def normalized(info):
    info.uid = info.gid = 0
    info.uname = info.gname = "root"
    return info


if __name__ == "__main__":
    main()
