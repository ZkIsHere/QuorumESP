#!/bin/bash
# Mint a client cert (CN=$1) from the local dev CA + enroll into WSL NSS DB.
# Usage: mint-client.sh <CN>   (e.g. interop-test)
set -e
CN="$1"
PKI=/mnt/d/PersonalProject/QuorumESP/host/pki
NSS_DB=/etc/corosync/qdevice/net/nssdb
mkdir -p "$PKI"
cd "$PKI"
openssl req -newkey rsa:2048 -nodes -keyout "client-$CN.key" \
  -out "client-$CN.csr" -subj "/CN=$CN"
openssl x509 -req -in "client-$CN.csr" -CA ca.crt -CAkey ca.key \
  -CAcreateserial -days 90 -out "client-$CN.crt"
rm -f "client-$CN.csr"
chmod 600 "client-$CN.key"
openssl pkcs12 -export -out "client-$CN.p12" -inkey "client-$CN.key" \
  -in "client-$CN.crt" -password pass:
certutil -D -d "$NSS_DB" -n "Cluster Cert" 2>/dev/null || true
certutil -D -d "$NSS_DB" -n "QNet CA" 2>/dev/null || true
certutil -A -d "$NSS_DB" -n "QNet CA" -t "CT,c,c" -i ca.crt
pk12util -i "client-$CN.p12" -d "$NSS_DB" -W ""
rm -f "client-$CN.p12"
certutil -L -d "$NSS_DB"
