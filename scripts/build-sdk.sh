#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the two plugin packages with a release-pinned OpenWrt SDK.
set -euo pipefail
export LC_ALL=C
umask 022

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    printf 'Usage: bash scripts/build-sdk.sh [x86-64-ipk|aarch64-cortex-a53-apk]\n\n'
    printf 'Default: x86-64-ipk (OpenWrt 24.10.5 x86/64).\n'
    printf 'aarch64-cortex-a53-apk: OpenWrt 25.12.6 mediatek/filogic.\n'
    printf 'Both SDKs require a Linux x86_64 host, a non-root user, and the OpenWrt SDK host tools.\n'
    printf 'Optional: SDK_CACHE_DIR, SDK_OUTPUT_DIR, SDK_JOBS, SDK_VERBOSE=s, SDK_KEEP_WORK=1.\n'
    printf 'SDK_WIFI_TEST=1 runs isolated Wi-Fi fixtures with the x86/64 SDK (requires sudo).\n'
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
if [[ ${SDK_WIFI_TEST:-0} == 1 && $profile != x86-64-ipk ]]; then
    fail 'SDK_WIFI_TEST uses the x86/64 SDK to execute on the Linux x86_64 host'
fi

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
[[ -f $repo/package/meshd/Makefile && -f $repo/luci-app-meshd/Makefile ]] || fail 'both package Makefiles are required'

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
cp -a -- "$repo/package/meshd" "$sdk/package/meshd"
cp -a -- "$repo/luci-app-meshd" "$sdk/package/luci-app-meshd"

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
    ./scripts/feeds install libubox libubus libuci libblobmsg-json libmbedtls luci-base

    cat > .config <<'CONFIG'
# CONFIG_ALL is not set
# CONFIG_ALL_NONSHARED is not set
# CONFIG_ALL_KMODS is not set
CONFIG_PACKAGE_meshd=m
CONFIG_PACKAGE_luci-app-meshd=m
CONFIG
    make defconfig
    grep -Fxq "CONFIG_TARGET_ARCH_PACKAGES=\"$pkg_arch\"" .config || fail "SDK target is not $pkg_arch"
    if [[ $pkg_ext == apk ]]; then
        grep -Fxq 'CONFIG_USE_APK=y' .config || fail 'SDK is not configured for APK packages'
    else
        if grep -Fxq 'CONFIG_USE_APK=y' .config; then fail 'SDK is not configured for IPK packages'; fi
    fi
    for pkg in meshd luci-app-meshd libubox libubus libuci libblobmsg-json libmbedtls luci-base; do
        grep -Eq "^CONFIG_PACKAGE_${pkg}=[my]$" .config || fail "SDK configuration excluded $pkg"
    done

    make -j"$jobs" V="${SDK_VERBOSE:-}" package/meshd/compile
    make -j"$jobs" V="${SDK_VERBOSE:-}" package/luci-app-meshd/compile
)

shopt -s globstar nullglob
if [[ $pkg_ext == apk ]]; then
    meshd=("$sdk"/bin/**/meshd-[0-9]*.apk)
    luci=("$sdk"/bin/**/luci-app-meshd-[0-9]*.apk)
else
    meshd=("$sdk"/bin/**/meshd_*_"$pkg_arch".ipk)
    luci=("$sdk"/bin/**/luci-app-meshd_*_all.ipk)
fi
[[ ${#meshd[@]} == 1 && -s ${meshd[0]} ]] || fail "expected exactly one nonempty $pkg_arch meshd $pkg_ext"
[[ ${#luci[@]} == 1 && -s ${luci[0]} ]] || fail "expected exactly one nonempty architecture-independent LuCI $pkg_ext"

if [[ $pkg_ext == apk ]]; then
    apk="$sdk/staging_dir/host/bin/apk"
    [[ -x $apk ]] || fail 'SDK APK tool is missing'
    "$apk" adbdump --format json "${meshd[0]}" >"$work/meshd.json"
    "$apk" adbdump --format json "${luci[0]}" >"$work/luci.json"
    python3 - "$work/meshd.json" "$work/luci.json" "$pkg_arch" <<'PY'
import json
import re
import sys
from pathlib import Path

for filename, name, arch, required in (
    (sys.argv[1], 'meshd', sys.argv[3], ('libubus', 'libubox', 'libblobmsg-json', 'libuci', 'libmbedtls')),
    (sys.argv[2], 'luci-app-meshd', 'noarch', ('meshd', 'luci-base')),
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
    "$apk" --allow-untrusted extract --destination "$work/apk-contents" "${meshd[0]}"
    readelf -h "$work/apk-contents/usr/sbin/meshd" >"$work/meshd.elf"
    grep -Eq 'Machine:[[:space:]]+AArch64$' "$work/meshd.elf" || fail 'meshd is not an AArch64 ELF'
    grep -Fxq '/etc/config/meshd' "$work/apk-contents/lib/apk/packages/meshd.conffiles" || fail 'APK did not preserve meshd configuration'
fi

if [[ ${SDK_WIFI_TEST:-0} == 1 ]]; then
    SDK="$sdk" bash "$repo/package/meshd/tests/run-wifi.sh"
fi

cp -- "${meshd[0]}" "${luci[0]}" "$output/"
printf 'Built packages (%s):\n  %s\n  %s\n' "$profile" "$output/${meshd[0]##*/}" "$output/${luci[0]##*/}"
(cd -- "$output" && sha256sum -- "${meshd[0]##*/}" "${luci[0]##*/}")
