#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build only the two project IPKs with the official x86/64 release SDK.
set -euo pipefail
umask 022

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    printf 'Usage: bash scripts/build-sdk.sh\n\n'
    printf 'Requires Linux x86_64, a non-root user, and the OpenWrt SDK host tools.\n'
    printf 'Optional: SDK_CACHE_DIR, SDK_OUTPUT_DIR, SDK_JOBS, SDK_VERBOSE=s, SDK_KEEP_WORK=1.\n'
    exit 0
fi

fail() { printf 'build-sdk: %s\n' "$*" >&2; exit 1; }

[[ $# == 0 ]] || fail 'unexpected arguments; see --help'
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || fail 'the official SDK needs Linux x86_64'
(( EUID != 0 )) || fail 'run as a non-root build user'
for tool in curl git make gcc g++ perl python3 rsync sha256sum tar zstd; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing host tool: $tool"
done

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
[[ -f $repo/package/meshd/Makefile && -f $repo/luci-app-meshd/Makefile ]] || fail 'both package Makefiles are required'

sdk_release=24.10.5
sdk_name="openwrt-sdk-$sdk_release-x86-64_gcc-13.3.0_musl.Linux-x86_64"
sdk_archive="$sdk_name.tar.zst"
sdk_url="https://downloads.openwrt.org/releases/$sdk_release/targets/x86/64/$sdk_archive"
# https://downloads.openwrt.org/releases/24.10.5/targets/x86/64/sha256sums
sdk_sha256=d3e8ea62fc1c12f93a9c808c2ef4c01b6e149ee240bcd5a74d15bebcbc385bdd

cache=${SDK_CACHE_DIR:-"$repo/.build-cache/openwrt"}
output=${SDK_OUTPUT_DIR:-"$repo/.build-cache/ipk"}
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
    # Base is the v24.10.5 release tag's commit. The other two commits come
    # from the release's feeds.buildinfo. Use upstream's GitHub mirrors and
    # fetch just these revisions, not full histories or mutable branches.
    cat > feeds.conf <<'FEEDS'
src-git base https://github.com/openwrt/openwrt.git^4f7e6e554be2aef6a55be36f9f954d56705eb2ee
src-git packages https://github.com/openwrt/packages.git^953b6d47b4e9f0ad3c39547c6d3f9a828f10e206
src-git luci https://github.com/openwrt/luci.git^d88390be4ec9722cb427fee03368fc8c8582627d
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
    for pkg in meshd luci-app-meshd libubox libubus libuci libblobmsg-json libmbedtls luci-base; do
        grep -Eq "^CONFIG_PACKAGE_${pkg}=[my]$" .config || fail "SDK configuration excluded $pkg"
    done

    make -j"$jobs" V="${SDK_VERBOSE:-}" package/meshd/compile
    make -j"$jobs" V="${SDK_VERBOSE:-}" package/luci-app-meshd/compile
)

shopt -s globstar nullglob
meshd=("$sdk"/bin/**/meshd_*_x86_64.ipk)
luci=("$sdk"/bin/**/luci-app-meshd_*_all.ipk)
[[ ${#meshd[@]} == 1 && -s ${meshd[0]} ]] || fail 'expected exactly one nonempty x86_64 meshd IPK'
[[ ${#luci[@]} == 1 && -s ${luci[0]} ]] || fail 'expected exactly one nonempty architecture-independent LuCI IPK'
cp -- "${meshd[0]}" "${luci[0]}" "$output/"
printf 'Built IPKs:\n  %s\n  %s\n' "$output/${meshd[0]##*/}" "$output/${luci[0]##*/}"
