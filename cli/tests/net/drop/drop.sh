#!/bin/sh
#
# Run DROP's own server, dropd, on a test wan: drop.sh DROPD DIR IP [IP6]
#
# Makes a CA and a certificate for drop.test, ipv4.drop.test and ipv6.drop.test
# in DIR (DIR/ca.pem is what every qdc trusts), configures dropd for IP (and
# IP6) with STUN on 3478 and short probe timeouts, and writes DIR/ready once it
# listens.

set -eu

dropd="$1"
dir="$2"
ip="$3"
ip6="${4:-}"
cd "$dir"

openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout ca.key -out ca.pem \
	-days 2 -subj "/CN=DROP test CA" -addext "basicConstraints=critical,CA:TRUE" \
	-addext "keyUsage=critical,keyCertSign" 2>/dev/null
openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout server.key -out server.csr \
	-subj "/CN=drop.test" 2>/dev/null
echo "subjectAltName=DNS:drop.test,DNS:ipv4.drop.test,DNS:ipv6.drop.test" >server.ext
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out server.pem \
	-days 2 -extfile server.ext 2>/dev/null

mkdir -p state
cat >dropd.toml <<TOML
[server]
name = "drop.test"
operator = "test network"
abuse_contact = "nobody@drop.test"

[control]
hostnames = ["drop.test", "ipv4.drop.test", "ipv6.drop.test"]
https_addr = ":443"
tls_cert = "$dir/server.pem"
tls_key = "$dir/server.key"

[limits]
max_ports_per_test = 4
min_port = 1024
probe_timeout = "3s"
inbound_deadline = "10s"
session_ttl = "60s"
sessions_per_hour = 1000
max_sessions = 4096

[state]
dir = "$dir/state"

[stun.ipv4]
primary_bind = "$ip"
primary_advertise = "$ip"
primary_port = 3478

$([ -z "$ip6" ] || printf '[stun.ipv6]\nprimary_bind = "%s"\nprimary_advertise = "%s"\nprimary_port = 3478\n' "$ip6" "$ip6")

[development]
allow_unroutable_advertise = true
TOML

"$dropd" -config dropd.toml &
tries=0
until ss -ltn | grep -q ":443 "
do
	tries=$((tries + 1))
	[ $tries -lt 200 ] || { echo "drop: dropd did not start" >&2; exit 1; }
	sleep 0.05
done
touch ready
wait
