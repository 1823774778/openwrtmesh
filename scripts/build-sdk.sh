#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the two plugin packages with a release-pinned OpenWrt SDK.
set -euo pipefail
export LC_ALL=C
umask 022

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    printf 'Usage: bash scripts/build-sdk.sh [x86-64-ipk|aarch64-cortex-a53-ipk|aarch64-cortex-a53-apk]\n\n'
    printf 'Build the wifiroam daemon and its LuCI plugin with a release-pinned OpenWrt SDK.\n\n'
    printf 'Default: x86-64-ipk (OpenWrt 24.10.5 x86/64, IPK).\n'
    printf 'aarch64-cortex-a53-ipk: OpenWrt 24.10.5 mediatek/filogic (IPK).\n'
    printf 'aarch64-cortex-a53-apk: OpenWrt 25.12.6 mediatek/filogic (APK).\n'
    printf 'Both SDKs require a Linux x86_64 host, a non-root user, and the OpenWrt SDK host tools.\n'
    printf 'Optional: SDK_CACHE_DIR, SDK_OUTPUT_DIR, SDK_JOBS, SDK_VERBOSE=s, SDK_KEEP_WORK=1.\n'
    exit 0
fi

fail() { printf 'build-sdk: %s\n' "$*" >&2; exit 1; }

[[ $# -le 1 ]] || fail 'unexpected arguments; see --help'
profile=${1-x86-64-ipk}
case "$profile" in
    x86-64-ipk)
        sdk_release=24.10.5
        sdk_target=x86/64
        sdk_gcc=13.3.0
        pkg_arch=x86_64
        pkg_ext=ipk
        # https://downloads.openwrt.org/releases/24.10.5/targets/x86/64/sha256sums
        sdk_sha256=d3e8ea62fc1c12f93a9c808c2ef4c01b6e149ee240bcd5a74d15bebcbc385bdd
        base_rev=4f7e6e554be2aef6a55be36f9f954d56705eb2ee
        packages_rev=953b6d47b4e9f0ad3c39547c6d3f9a828f10e206
        luci_rev=d88390be4ec9722cb427fee03368fc8c8582627d
        ;;
    aarch64-cortex-a53-ipk)
        sdk_release=24.10.5
        sdk_target=mediatek/filogic
        sdk_gcc=13.3.0
        pkg_arch=aarch64_cortex-a53
        pkg_ext=ipk
        # https://downloads.openwrt.org/releases/24.10.5/targets/mediatek/filogic/sha256sums
        sdk_sha256=0fa98c542ee07f1423ebcdce19aed464bc7f8008ef2cbe83cb723ec694ed8c56
        base_rev=4f7e6e554be2aef6a55be36f9f954d56705eb2ee
        packages_rev=953b6d47b4e9f0ad3c39547c6d3f9a828f10e206
        luci_rev=d88390be4ec9722cb427fee03368fc8c8582627d
        ;;
    aarch64-cortex-a53-apk)
        sdk_release=25.12.6
        sdk_target=mediatek/filogic
        sdk_gcc=14.3.0
        pkg_arch=aarch64_cortex-a53
        pkg_ext=apk
        # https://downloads.openwrt.org/releases/25.12.6/targets/mediatek/filogic/sha256sums
        sdk_sha256=1c04ac47bdfc361d6aab299eb520d570e368013dedcb8d29cf39dc7168ae642e
        # Match the official SDK's version.buildinfo and feeds.buildinfo.
        base_rev=981c978eff636fc7d81f0ed17b00e2a6317c949a
        packages_rev=093d551675b1119a06ef224d419905e8d6b2af1a
        luci_rev=75a306df84a020c57a631ef5b4c02ace56d93387
        ;;
    *) fail "unknown profile '$profile'; see --help" ;;
esac

[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || fail 'the official SDK needs a Linux x86_64 host'
(( EUID != 0 )) || fail 'run as a non-root build user'
for tool in curl git make gcc g++ perl python3 readelf rsync sha256sum tar zstd; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing host tool: $tool"
done

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
[[ -f $repo/package/wifiroam/Makefile && -f $repo/luci-app-wifiroam/Makefile ]] || fail 'both wifiroam package Makefiles are required'

sdk_name="openwrt-sdk-$sdk_release-${sdk_target/\//-}_gcc-${sdk_gcc}_musl.Linux-x86_64"
sdk_archive="$sdk_name.tar.zst"
sdk_url="https://downloads.openwrt.org/releases/$sdk_release/targets/$sdk_target/$sdk_archive"

cache=${SDK_CACHE_DIR:-"$repo/.build-cache/openwrt"}
output=${SDK_OUTPUT_DIR:-"$repo/.build-cache/$pkg_ext"}
jobs=${SDK_JOBS:-$(getconf _NPROCESSORS_ONLN)}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || fail 'SDK_JOBS must be a positive integer'
mkdir -p -- "$cache" "$output"
cache=$(cd -- "$cache" && pwd -P)
output=$(cd -- "$output" && pwd -P)
[[ ! $cache =~ [[:space:]] ]] || fail 'the SDK cache path must not contain whitespace'

work=$(mktemp -d "$cache/sdk.XXXXXXXX")
cleanup() {
    local rc=$?
    if [[ ${SDK_KEEP_WORK:-0} == 1 ]]; then
        printf 'SDK work directory: %s\n' "$work"
    else
        rm -rf -- "$work"
    fi
    exit "$rc"
}
trap cleanup EXIT

archive="$cache/$sdk_archive"
verify_sdk() { printf '%s  %s\n' "$sdk_sha256" "$1" | sha256sum -c -; }
if [[ ! -f $archive ]]; then
    printf 'Downloading %s\n' "$sdk_url"
    curl --fail --location --silent --show-error --retry 3 --connect-timeout 20 \
        --proto '=https' --proto-redir '=https' --output "$work/$sdk_archive" "$sdk_url"
    verify_sdk "$work/$sdk_archive"
    mv -f -- "$work/$sdk_archive" "$archive"
fi
verify_sdk "$archive" || fail "invalid cached SDK; remove $archive and retry"
tar --zstd -xf "$archive" -C "$work"
sdk="$work/$sdk_name"
cp -a -- "$repo/package/wifiroam" "$sdk/package/wifiroam"
cp -a -- "$repo/luci-app-wifiroam" "$sdk/package/luci-app-wifiroam"

(
    cd -- "$sdk"
    # Use the official release sources via their GitHub mirrors. Fetch only
    # these immutable revisions, not entire histories or mutable branches.
    cat > feeds.conf <<FEEDS
src-git base https://github.com/openwrt/openwrt.git^$base_rev
src-git packages https://github.com/openwrt/packages.git^$packages_rev
src-git luci https://github.com/openwrt/luci.git^$luci_rev
FEEDS
    mkdir -p feeds
    while read -r _ name source; do
        revision=${source##*^}
        git init --quiet "feeds/$name"
        git -C "feeds/$name" remote add origin "${source%^*}"
        git -C "feeds/$name" fetch --no-tags --depth=1 origin "$revision"
        git -C "feeds/$name" -c core.autocrlf=false checkout --quiet --detach "$revision"
        [[ $(git -C "feeds/$name" rev-parse HEAD) == "$revision" ]] || fail "wrong $name feed revision"
    done < feeds.conf
    ./scripts/feeds update -i base packages luci
    # Install selected recipes and their dependencies, not all release feeds.
    # wifiroam needs the ucode runtime modules plus the netfilter queue libs.
    ./scripts/feeds install \
        ucode ucode-mod-ubus ucode-mod-uloop ucode-mod-socket \
        ucode-mod-fs ucode-mod-uci ucode-mod-digest \
        libnetfilter-queue libmnl libnfnetlink luci-base

    cat > .config <<'CONFIG'
# CONFIG_ALL is not set
# CONFIG_ALL_NONSHARED is not set
# CONFIG_ALL_KMODS is not set
CONFIG_PACKAGE_wifiroam=m
CONFIG_PACKAGE_luci-app-wifiroam=m
CONFIG
    make defconfig
    grep -Fxq "CONFIG_TARGET_ARCH_PACKAGES=\"$pkg_arch\"" .config || fail "SDK target is not $pkg_arch"
    if [[ $pkg_ext == apk ]]; then
        grep -Fxq 'CONFIG_USE_APK=y' .config || fail 'SDK is not configured for APK packages'
    else
        if grep -Fxq 'CONFIG_USE_APK=y' .config; then fail 'SDK is not configured for IPK packages'; fi
    fi
    for pkg in wifiroam luci-app-wifiroam ucode ucode-mod-ubus ucode-mod-uloop \
               ucode-mod-socket ucode-mod-fs ucode-mod-uci ucode-mod-digest luci-base; do
        grep -Eq "^CONFIG_PACKAGE_${pkg}=[my]$" .config || fail "SDK configuration excluded $pkg"
    done

    # Build the DPI library dependencies into staging_dir first, then the packages.
    for dep in libmnl libnfnetlink libnetfilter-queue; do
        make -j"$jobs" V="${SDK_VERBOSE:-}" "package/$dep/compile"
    done
    make -j"$jobs" V="${SDK_VERBOSE:-}" package/wifiroam/compile
    make -j"$jobs" V="${SDK_VERBOSE:-}" package/luci-app-wifiroam/compile
)

shopt -s globstar nullglob
if [[ $pkg_ext == apk ]]; then
    daemon_pkg=("$sdk"/bin/**/wifiroam-[0-9]*.apk)
    luci=("$sdk"/bin/**/luci-app-wifiroam-[0-9]*.apk)
else
    daemon_pkg=("$sdk"/bin/**/wifiroam_*_"$pkg_arch".ipk)
    luci=("$sdk"/bin/**/luci-app-wifiroam_*_all.ipk)
fi
[[ ${#daemon_pkg[@]} == 1 && -s ${daemon_pkg[0]} ]] || fail "expected exactly one nonempty $pkg_arch wifiroam $pkg_ext"
[[ ${#luci[@]} == 1 && -s ${luci[0]} ]] || fail "expected exactly one nonempty architecture-independent LuCI $pkg_ext"

if [[ $pkg_ext == apk ]]; then
    apk="$sdk/staging_dir/host/bin/apk"
    [[ -x $apk ]] || fail 'SDK APK tool is missing'
    "$apk" adbdump --format json "${daemon_pkg[0]}" >"$work/wifiroam.json"
    "$apk" adbdump --format json "${luci[0]}" >"$work/luci.json"
    python3 - "$work/wifiroam.json" "$work/luci.json" "$pkg_arch" <<'PY'
import json
import re
import sys
from pathlib import Path

for filename, name, arch, required in (
    (sys.argv[1], 'wifiroam', sys.argv[3], ('ucode', 'libnetfilter-queue')),
    # APK LuCI recipes depend on wifiroam (+libc); luci-base is provided by the image,
    # not an explicit package dependency, so do not require it here.
    (sys.argv[2], 'luci-app-wifiroam', 'noarch', ('wifiroam',)),
):
    pkg = json.loads(Path(filename).read_text(encoding='utf-8'))['info']
    if (pkg['name'], pkg['arch']) != (name, arch):
        raise SystemExit(f"Incorrect APK metadata: {pkg['name']}, {pkg['arch']}")
    deps = [re.split(r'[<>=~]', dep, maxsplit=1)[0] for dep in pkg.get('depends', [])]
    if any(not any(re.fullmatch(re.escape(lib) + r'\d*', dep) for dep in deps) for lib in required):
        raise SystemExit(f'{name} has incorrect dependencies: {deps}')
    print(f"Verified APK metadata: {name} {pkg['version']} ({arch})")
PY
    mkdir -- "$work/apk-contents"
    # Loose SDK packages are unsigned. Do not install them in the runner OS.
    "$apk" --allow-untrusted extract --destination "$work/apk-contents" "${daemon_pkg[0]}"
    readelf -h "$work/apk-contents/usr/sbin/wifiroam-dpi" >"$work/wifiroam-dpi.elf"
    grep -Eq 'Machine:[[:space:]]+AArch64$' "$work/wifiroam-dpi.elf" || fail 'wifiroam-dpi is not an AArch64 ELF'
    [[ -f $work/apk-contents/etc/config/wifiroam ]] || fail 'APK did not ship wifiroam configuration'
    [[ -f $work/apk-contents/usr/share/wifiroam/wifiroamd.uc ]] || fail 'APK did not ship the wifiroamd ucode script'
    python3 - "$work/wifiroam.json" <<'PY'
import json
import sys
from pathlib import Path

deps = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))['info'].get('depends', [])
print('Runtime dependencies (must already exist on the device): ' + ', '.join(deps))
PY
else
    python3 - "${daemon_pkg[0]}" "${luci[0]}" <<'PY'
import gzip
import io
import sys
import tarfile
from pathlib import Path


def control(path):
    raw = Path(path).read_bytes()
    # OpenWrt ships IPKs as an ar archive or, on some releases, a gzipped tar.
    if raw[:2] == b'\x1f\x8b':
        outer = gzip.decompress(raw)
    elif raw[:8] == b'!<arch>\n':
        raise SystemExit(f'{path}: unsupported ar IPK layout')
    else:
        raise SystemExit(f'{path}: unknown container')
    with tarfile.open(fileobj=io.BytesIO(outer)) as archive:
        member = next(n for n in archive.getnames() if 'control.tar' in n)
        inner = archive.extractfile(member).read()
    with tarfile.open(fileobj=io.BytesIO(inner), mode='r:*') as archive:
        name = next(n for n in archive.getnames() if n.endswith('/control'))
        return archive.extractfile(name).read().decode('utf-8', 'replace')


for path in sys.argv[1:]:
    for line in control(path).splitlines():
        if line.startswith('Depends:'):
            print(f'{Path(path).name} -> {line[len("Depends:"):].strip()}')
PY
fi

cp -- "${daemon_pkg[0]}" "${luci[0]}" "$output/"
printf 'Built packages (%s):\n  %s\n  %s\n' "$profile" "$output/${daemon_pkg[0]##*/}" "$output/${luci[0]##*/}"
(cd -- "$output" && sha256sum -- "${daemon_pkg[0]##*/}" "${luci[0]##*/}")
