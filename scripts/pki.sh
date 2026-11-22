#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# 在受信任的离线 Linux 机器上运行。不要把 ca.key 发到路由器。
set -eu
umask 077

usage() {
	printf '%s\n' \
		'用法: sh scripts/pki.sh init CA目录' \
		'      sh scripts/pki.sh issue CA目录 gateway|node 设备名 新输出目录' \
		'      sh scripts/pki.sh fingerprint 叶证书.crt' >&2
	exit 2
}

fingerprint() {
	# 不用管道掩盖证书解析错误。OpenSSL 输出 hex 和分隔符。
	v=$(openssl x509 -in "$1" -noout -fingerprint -sha256)
	v=${v#*=}
	printf '%s\n' "$v" | tr -d ':' | tr 'A-F' 'a-f'
}

command -v openssl >/dev/null 2>&1 || { echo '需要 openssl' >&2; exit 1; }
[ "$#" -ge 1 ] || usage
case "$1" in
init)
	[ "$#" -eq 2 ] || usage
	case "$2" in ''|-*) usage;; esac
	mkdir -- "$2"
	openssl ecparam -name prime256v1 -genkey -noout -out "$2/ca.key"
	openssl req -new -x509 -sha256 -days 1825 -key "$2/ca.key" -out "$2/ca.crt" \
		-subj '/CN=meshd private root' \
		-addext 'basicConstraints=critical,CA:TRUE,pathlen:0' \
		-addext 'keyUsage=critical,keyCertSign,cRLSign'
	printf '%s\n' '已生成独立 CA；只分发 ca.crt。'
	;;
issue)
	[ "$#" -eq 5 ] || usage
	case "$4" in ''|*[!a-zA-Z0-9_-]*) usage;; esac
	[ "${#4}" -le 48 ] || usage
	case "$5" in ''|-*) usage;; esac
	case "$3" in
	gateway) eku=serverAuth ;;
	node) eku=clientAuth ;;
	*) usage ;;
	esac
	[ -f "$2/ca.crt" ] && [ -f "$2/ca.key" ] || { echo '未找到 CA 证书或私钥' >&2; exit 1; }
	openssl x509 -in "$2/ca.crt" -checkend 34560000 -noout >/dev/null || {
		echo 'CA 有效期不足 400 天；请先规划轮换。' >&2; exit 1;
	}
	mkdir -- "$5"
	work=$(mktemp -d)
	trap 'rm -f -- "$work/request.csr" "$work/extensions"; rmdir -- "$work"' EXIT
	trap 'exit 1' HUP INT TERM
	printf '%s\n' 'basicConstraints=critical,CA:FALSE' 'keyUsage=critical,digitalSignature' \
		"extendedKeyUsage=$eku" 'subjectKeyIdentifier=hash' 'authorityKeyIdentifier=keyid' >"$work/extensions"
	openssl ecparam -name prime256v1 -genkey -noout -out "$5/node.key"
	openssl req -new -sha256 -key "$5/node.key" -out "$work/request.csr" -subj "/CN=meshd-$4"
	openssl x509 -req -sha256 -days 395 -CA "$2/ca.crt" -CAkey "$2/ca.key" \
		-set_serial "0x01$(openssl rand -hex 15)" -extfile "$work/extensions" \
		-in "$work/request.csr" -out "$5/node.crt"
	cp -- "$2/ca.crt" "$5/ca.crt"
	chmod 0600 -- "$5/node.key" "$5/node.crt" "$5/ca.crt"
	openssl verify -CAfile "$5/ca.crt" "$5/node.crt" >&2
	fingerprint "$5/node.crt"
	;;
fingerprint)
	[ "$#" -eq 2 ] || usage
	fingerprint "$2"
	;;
*) usage ;;
esac
