#!/bin/bash
#
# Run a qdc script across a test network of rootless podman containers.
#
# Usage: run.sh <qdc> <topology> <script> [controller options...]
#
# The topology file has one statement per line (# comments):
#
#   net NAME                          a network; "wan" is the internet, any
#                                     other is a lan behind a router
#   node NAME ROLE ADDR... [nat=MODE] a container; ADDR is NET.HOST, e.g. wan.20
#
# ROLE is router (first ADDR on the wan, second on its lan; nat=home, symmetric
# or none, see router/router.sh), client (on a lan, default route through that
# lan's router) or public (on the wan). Every client and public node runs a qdc
# agent with one core named after the node; the script addresses it as @NAME.
# Each node's first address (a router's on the wan) is ${IP_NAME}. Agents serve
# on Unix sockets in a directory mounted into every container, so the
# controller runs here and no control traffic crosses the test networks. Agents
# share one home (/run/qdc/home), so ../b/... works as with remote.sh.
#
# The wan is 198.18.N.0/24 and lan k is 10.N.k.0/24, N picked per run, so runs
# in parallel do not collide. Everything is torn down however the run ends;
# NET_KEEP=1 keeps the run directory (logs, homes) and prints where it is.
#
# Exit: 0 the script passed, 1 it failed, 77 podman or rootless networking is
# not available here (ctest SKIP_RETURN_CODE).

set -u

SKIP=77
here=$(cd "$(dirname "$0")" && pwd)

qdc=$(readlink -f "$1")
topology="$2"
script=$(readlink -f "$3")
shift 3
extra=("$@")

timeout_s=${NET_TIMEOUT:-240}

skip() { echo "net: skipped: $*" >&2; exit $SKIP; }
fail() { echo "net: $*" >&2; exit 1; }

command -v podman >/dev/null 2>&1 || skip "no podman"
podman info >/dev/null 2>&1 || skip "podman does not work here"

run=qdcnet-$$-$RANDOM
# Short and outside the build tree: a Unix socket path must fit in 108 bytes.
rundir=$(mktemp -d /tmp/qdcnet.XXXXXX) || exit 1
mkdir -p "$rundir/sock" "$rundir/log" "$rundir/home"

teardown() {
	local ids
	ids=$(podman ps -aq --filter "label=quickdc-net.run=$run" 2>/dev/null)
	[ -n "$ids" ] && podman rm -f -t 0 $ids >/dev/null 2>&1
	ids=$(podman network ls -q --filter "label=quickdc-net.run=$run" 2>/dev/null)
	[ -n "$ids" ] && podman network rm -f $ids >/dev/null 2>&1
	if [ "${NET_KEEP:-0}" = 1 ]
	then
		echo "net: kept $rundir" >&2
	else
		rm -rf "$rundir"
	fi
}
trap teardown EXIT
trap 'exit 1' INT TERM HUP

# --- images: tagged with a hash of what builds them, so a change rebuilds

image() {
	local dir="$here/$1" tag
	tag=quickdc-net-$1:$(sha256sum <"$dir/Containerfile" | cut -c1-12)
	if ! podman image exists "$tag"
	then
		local base
		base=$(sed -n 's/^FROM //p' "$dir/Containerfile")
		podman image exists "$base" || podman pull -q "$base" >/dev/null 2>&1 \
			|| skip "cannot pull $base"
		# Parallel runs build one at a time; the second finds the image there.
		exec 9>"${XDG_RUNTIME_DIR:-/tmp}/quickdc-net-$1.lock"
		flock 9
		podman image exists "$tag" \
			|| podman build -q -t "$tag" "$dir" >"$rundir/log/build-$1" 2>&1 \
			|| { cat "$rundir/log/build-$1" >&2; fail "building $tag failed"; }
		# Images an older Containerfile built.
		podman images --format '{{.Repository}}:{{.Tag}}' "localhost/quickdc-net-$1" \
			| grep -vx "localhost/$tag" | xargs -r podman rmi >/dev/null 2>&1
		exec 9>&-
	fi
	echo "$tag"
}

host_image=$(image host) || exit $?
router_image=$(image router) || exit $?

# --- topology

nets=()
declare -A net_subnet
nodes=()
declare -A node_role node_addrs node_nat

while read -r word rest
do
	case "$word" in
	''|'#'*) ;;
	net) nets+=("$rest") ;;
	node)
		set -- $rest
		name="$1" role="$2"
		shift 2
		node_nat[$name]=home
		node_addrs[$name]=
		for arg
		do
			case "$arg" in
			nat=*) node_nat[$name]=${arg#nat=} ;;
			*) node_addrs[$name]="${node_addrs[$name]} $arg" ;;
			esac
		done
		node_role[$name]=$role
		nodes+=("$name")
		;;
	*) fail "$topology: unknown statement '$word'" ;;
	esac
done <"$topology"

# Pick N whose wan and lans are all free; podman refuses a subnet in use.
slot_ok=
for try in $(seq 0 255)
do
	n=$(( (RANDOM + try) % 256 ))
	created=()
	k=0
	ok=1
	for net in "${nets[@]}"
	do
		if [ "$net" = wan ]
		then
			prefix=198.18.$n
		else
			k=$((k + 1))
			prefix=10.$n.$k
		fi
		if ! podman network create --internal --disable-dns --subnet "$prefix.0/24" \
			--label "quickdc-net.run=$run" "$run-$net" >/dev/null 2>"$rundir/log/net"
		then
			ok=0
			break
		fi
		created+=("$run-$net")
		net_subnet[$net]=$prefix
	done
	if [ $ok = 1 ]
	then
		slot_ok=1
		break
	fi
	[ ${#created[@]} -gt 0 ] && podman network rm -f "${created[@]}" >/dev/null 2>&1
	grep -q "already used" "$rundir/log/net" || skip "cannot create a network: $(cat "$rundir/log/net")"
done
[ -n "$slot_ok" ] || skip "no free subnets"

ip_of() {
	local net="${1%%.*}" host="${1#*.}"
	[ -n "${net_subnet[$net]:-}" ] || fail "$topology: no net '$net'"
	echo "${net_subnet[$net]}.$host"
}

network_args() {
	local addr
	for addr in ${node_addrs[$1]}
	do
		printf -- '--network %s:ip=%s ' "$run-${addr%%.*}" "$(ip_of "$addr")"
	done
}

common=(--label "quickdc-net.run=$run" --hostname)

# --- routers first, so a client's route is there before its qdc dials out

declare -A gateway
defines=()
for name in "${nodes[@]}"
do
	[ "${node_role[$name]}" = router ] || continue
	set -- ${node_addrs[$name]}
	wan_ip=$(ip_of "$1")
	lan_ip=$(ip_of "$2")
	gateway[${2%%.*}]=$lan_ip
	defines+=(-D "IP_$name=$wan_ip")
	podman run -d --name "$run-$name" "${common[@]}" "$name" $(network_args "$name") \
		--cap-add NET_ADMIN --sysctl net.ipv4.ip_forward=1 \
		-v "$here/router:/opt/router:ro" -v "$rundir:/run/qdc" \
		"$router_image" /opt/router/router.sh "$wan_ip" "$lan_ip" "${node_nat[$name]}" \
		"/run/qdc/log/$name.ready" >/dev/null 2>"$rundir/log/$name.run" \
		|| skip "cannot start router $name: $(cat "$rundir/log/$name.run")"
done

wait_for() {
	local test="$1" path="$2" name="$3" tries=0
	until [ $test "$path" ]
	do
		tries=$((tries + 1))
		if [ $tries -gt 400 ] || [ "$(podman inspect -f '{{.State.Running}}' "$run-$name" 2>/dev/null)" != true ]
		then
			podman logs "$run-$name" >&2 2>&1
			cat "$rundir/log/$name".* >&2 2>/dev/null
			fail "$name did not come up"
		fi
		sleep 0.05
	done
}

for name in "${nodes[@]}"
do
	[ "${node_role[$name]}" = router ] && wait_for -f "$rundir/log/$name.ready" "$name"
done

# --- agents

connect=()
qdc_dir=$(dirname "$qdc")
for name in "${nodes[@]}"
do
	role=${node_role[$name]}
	[ "$role" = router ] && continue
	set -- ${node_addrs[$name]}
	defines+=(-D "IP_$name=$(ip_of "$1")")
	route=
	caps=()
	if [ "$role" = client ]
	then
		gw=${gateway[${1%%.*}]:-}
		[ -n "$gw" ] || fail "$topology: no router on ${1%%.*} for $name"
		route="ip route replace default via $gw &&"
		caps=(--cap-add NET_ADMIN)
	fi
	podman run -d --name "$run-$name" "${common[@]}" "$name" $(network_args "$name") "${caps[@]}" \
		-v "$qdc_dir:/opt/qdc:ro" -v "$rundir:/run/qdc" \
		"$host_image" sh -c "$route exec /opt/qdc/$(basename "$qdc") --serve /run/qdc/sock/$name \
			--core $name --home /run/qdc/home >/run/qdc/log/$name.out 2>/run/qdc/log/$name.err" \
		>/dev/null 2>"$rundir/log/$name.run" \
		|| skip "cannot start $name: $(cat "$rundir/log/$name.run")"
	connect+=(--connect "$name=$rundir/sock/$name")
done

for name in "${nodes[@]}"
do
	[ "${node_role[$name]}" = router ] || wait_for -S "$rundir/sock/$name" "$name"
done

# --- the script

timeout "$timeout_s" "$qdc" --home "$rundir/home" "${connect[@]}" "${defines[@]}" "${extra[@]}" "$script" \
	>"$rundir/log/controller.out" 2>"$rundir/log/controller.err"
status=$?

if [ $status -ne 0 ]
then
	[ $status -eq 124 ] && echo "net: the script ran past ${timeout_s}s" >&2
	echo "--- controller stderr" >&2
	cat "$rundir/log/controller.err" >&2
	for name in "${nodes[@]}"
	do
		[ "${node_role[$name]}" = router ] && continue
		echo "--- agent $name" >&2
		tail -n 50 "$rundir/log/$name.err" >&2
	done
	exit 1
fi
exit 0
