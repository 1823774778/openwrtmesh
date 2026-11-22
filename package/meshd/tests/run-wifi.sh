#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Compile and run only against temporary fixture paths on a Linux test runner.
set -euo pipefail
export LC_ALL=C
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.."
src=package/meshd/src
test_src=package/meshd/tests/test_wifi.c

[[ $(uname -s) == Linux ]] || { echo 'Use a Linux test runner.' >&2; exit 1; }
[[ $# == 0 ]] || { echo 'Usage: SDK=/path/to/x86-64-sdk bash package/meshd/tests/run-wifi.sh' >&2; exit 1; }
[[ -r $src/wifi.c && -r $test_src ]] || exit 1
d=$(mktemp -d /tmp/meshd-wifi-test.XXXXXXXX)
cleanup() {
    case $d in /tmp/meshd-wifi-test.????????) sudo rm -rf -- "$d" ;; esac
}
trap cleanup EXIT
sudo install -d -o root -g root -m 0700 \
    "$d/config" "$d/state" "$d/delta" "$d/rpcd/uci-example" \
    "$d/rpcd/snapshot-files" "$d/override" "$d/sbin"
sudo install -o root -g root -m 0755 /bin/true "$d/sbin/hostapd"

# Rewrite all real paths in the temporary copy. APK_PAYLOAD_DIR is only used
# to check package list data; the separate HOSTAPD_DIR opens the fixture files.
sed -e "s|/etc/config|$d/config|g" \
    -e "s|/etc/meshd|$d/state|g" \
    -e "s|/usr/lib/opkg/status|$d/status|g" \
    -e "s|/lib/apk|$d/apk|g" \
    -e "s|/usr/bin/apk|$d/apk-bin|g" \
    -e "s|^#define HOSTAPD_DIR APK_PAYLOAD_DIR\$|#define HOSTAPD_DIR \"$d/sbin\"|" \
    -e "s|/tmp/\.uci|$d/delta|g" \
    -e "s|scan_deltas(\"/tmp\",|scan_deltas(\"$d\",|g" \
    -e "s|/var/run/rpcd|$d/rpcd|g" \
    -e "s|UCI_CONF2DIR|\"$d/override\"|g" \
    "$src/wifi.c" >"$d/wifi-test-impl.c"
grep -Fxq "#define HOSTAPD_DIR \"$d/sbin\"" "$d/wifi-test-impl.c"
grep -vFx '#define APK_PAYLOAD_DIR "/usr/sbin"' "$d/wifi-test-impl.c" >"$d/path-check.c"
if grep -Eq '/etc/(config|meshd)|/usr/lib/opkg/status|/lib/apk|/usr/bin/apk|/usr/sbin|/tmp/\.uci|/var/run/rpcd|UCI_CONF2DIR|scan_deltas\("/tmp"|^#define HOSTAPD_DIR APK_PAYLOAD_DIR' "$d/path-check.c"; then
    echo 'Fixture source still references production paths; refusing to test.' >&2
    exit 1
fi
printf '\nconst char mesh_wifi_test_link_root[] = "%s";\n' "$d" >>"$d/wifi-test-impl.c"

cc_args=(-D_GNU_SOURCE -std=gnu11 -Wall -Wextra -Werror "-DMESH_WIFI_TEST_ROOT=\"$d\"" "-I$src")
cc_src=("$d/wifi-test-impl.c" "$src/protocol.c" "$test_src")
if [[ -n ${SDK:-} ]]; then
    target="$SDK/staging_dir/target-x86_64_musl"
    shopt -s nullglob
    toolchains=("$SDK"/staging_dir/toolchain-x86_64_gcc-*_musl)
    if [[ ${#toolchains[@]} != 1 || ! -x ${toolchains[0]}/bin/x86_64-openwrt-linux-musl-gcc ]]; then
        echo 'Expected one x86/64 musl toolchain in the SDK.' >&2
        exit 1
    fi
    toolchain=${toolchains[0]}
    export PATH="$toolchain/bin:$PATH" STAGING_DIR="$target"
    x86_64-openwrt-linux-musl-gcc "${cc_args[@]}" -I"$target/usr/include" "${cc_src[@]}" \
        -L"$target/usr/lib" -Wl,-rpath-link,"$target/usr/lib" -luci -lubox -lmbedcrypto -o "$d/test_wifi"
    sudo "$toolchain/lib/ld-musl-x86_64.so.1" --library-path "$target/usr/lib:$toolchain/lib" "$d/test_wifi"
else
    "${CC:-cc}" "${cc_args[@]}" "${cc_src[@]}" -luci -lubox -lmbedcrypto -o "$d/test_wifi"
    sudo "$d/test_wifi"
fi
