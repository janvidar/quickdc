#!/bin/bash
#
# Run a qdc script across a test network of rootless podman containers.
#
# Usage: run.sh <qdc> <topology> <script> [controller options...]
#
# The topology file has one statement per line (# comments):
#
#   net NAME [ipv6=on|only]           a network; "wan" is the internet, any
#                                     other is a lan behind a router; ipv6=on
#                                     is dual stack, only has no IPv4
#   node NAME ROLE ADDR... [nat=MODE] [upnp=on] [fw6=open] [link=NETEM]
#                          [cut=BYTES:SECONDS] [also=CORE,...]
#                                     a container; ADDR is NET.HOST, e.g. wan.20,
#                                     which is an address in each family NET has
#
# ROLE is router (first ADDR on the wan, second on its lan; nat=home, symmetric
# or none, see router/router.sh; upnp=on runs miniupnpd on its lan), client (on
# a lan, default route through that lan's router), public (on the wan) or drop
# (on the wan: a reachability server, drop/dropserver.py, known to every
# container as drop.test, ipv4.drop.test and ipv6.drop.test and trusted by every
# qdc; the script gets its name as ${DROP}). Every client and public node runs a qdc
# agent with one core named after the node; the script addresses it as @NAME.
# also=x,y adds cores x and y to that agent, so they share its host; they start
# before the node's own core, so what they bind it finds taken.
# IPv6 is routed, not translated: a router's fw6=open lets new inbound IPv6
# through to its lan, which is otherwise dropped (unless nat=none). link= impairs
# both directions through a router with tc netem, written as comma-separated
# option=value with / for spaces: link=delay=75ms/5ms,loss=2%,reorder=10%,rate=8mbit.
# cut=BYTES:SECONDS drops everything through the router for SECONDS once BYTES
# have been forwarded; when is logged to NAME.link in the run directory's log/.
# Each node's first address (a router's on the wan) is ${IP_NAME}, its first
# IPv6 one ${IP6_NAME}. Agents serve
# on Unix sockets in a directory mounted into every container, so the
# controller runs here and no control traffic crosses the test networks. Agents
# share one home (/run/qdc/home), so ../b/... works as with remote.sh.
#
# The wan is 198.18.N.0/24 and lan k is 10.N.k.0/24, N picked per run, so runs
# in parallel do not collide; in IPv6 2001:db8:N::/64 and 2001:db8:N:k::/64 -
# the documentation prefix, which nothing routes, and which qdc counts as global
# (it excludes only link-local, unique-local, loopback, mapped and multicast). Everything is torn down however the run ends;
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

defines=()

nets=()
declare -A net_subnet net_subnet6 net_family
nodes=()
declare -A node_role node_addrs node_nat node_upnp node_also node_fw6 node_link node_cut

while read -r word rest
do
	case "$word" in
	''|'#'*) ;;
	net)
		set -- $rest
		nets+=("$1")
		net_family[$1]=ipv4
		case "${2:-}" in
		'') ;;
		ipv6=on) net_family[$1]=dual ;;
		ipv6=only) net_family[$1]=ipv6 ;;
		*) fail "$topology: unknown net option '$2'" ;;
		esac
		;;
	node)
		set -- $rest
		name="$1" role="$2"
		shift 2
		node_nat[$name]=home
		node_upnp[$name]=off
		node_also[$name]=
		node_addrs[$name]=
		node_fw6[$name]=closed
		node_link[$name]=
		node_cut[$name]=
		for arg
		do
			case "$arg" in
			nat=*) node_nat[$name]=${arg#nat=} ;;
			upnp=*) node_upnp[$name]=${arg#upnp=} ;;
			also=*) node_also[$name]=${arg#also=} ;;
			fw6=*) node_fw6[$name]=${arg#fw6=} ;;
			link=*)
				netem=${arg#link=}
				netem=${netem//,/ }
				netem=${netem//=/ }
				node_link[$name]=${netem//\// }
				;;
			cut=*) node_cut[$name]=${arg#cut=} ;;
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
	n=$(( (RANDOM + try) % 255 + 1 ))
	created=()
	k=0
	ok=1
	for net in "${nets[@]}"
	do
		if [ "$net" = wan ]
		then
			prefix=198.18.$n
			prefix6=2001:db8:$n
		else
			k=$((k + 1))
			prefix=10.$n.$k
			prefix6=2001:db8:$n:$k
		fi
		subnets=()
		[ "${net_family[$net]}" = ipv6 ] || subnets+=(--subnet "$prefix.0/24")
		[ "${net_family[$net]}" = ipv4 ] || subnets+=(--ipv6 --subnet "$prefix6::/64")
		if ! podman network create --internal --disable-dns "${subnets[@]}" \
			--label "quickdc-net.run=$run" "$run-$net" >/dev/null 2>"$rundir/log/net"
		then
			ok=0
			break
		fi
		created+=("$run-$net")
		net_subnet[$net]=
		net_subnet6[$net]=
		[ "${net_family[$net]}" = ipv6 ] || net_subnet[$net]=$prefix
		[ "${net_family[$net]}" = ipv4 ] || net_subnet6[$net]=$prefix6
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

# An address in each family, empty where the net has none in that one.
ip_of() {
	local net="${1%%.*}" host="${1#*.}"
	[ -n "${net_family[$net]:-}" ] || fail "$topology: no net '$net'"
	[ -z "${net_subnet[$net]}" ] || echo "${net_subnet[$net]}.$host"
}

ip6_of() {
	local net="${1%%.*}" host="${1#*.}"
	[ -n "${net_family[$net]:-}" ] || fail "$topology: no net '$net'"
	[ -z "${net_subnet6[$net]}" ] || echo "${net_subnet6[$net]}::$host"
}

network_args() {
	local addr v4 v6 opts
	for addr in ${node_addrs[$1]}
	do
		v4=$(ip_of "$addr")
		v6=$(ip6_of "$addr")
		opts=
		[ -z "$v4" ] || opts="ip=$v4"
		[ -z "$v6" ] || opts="${opts:+$opts,}ip6=$v6"
		printf -- '--network %s:%s ' "$run-${addr%%.*}" "$opts"
	done
}

define_ips() {
	local v4 v6
	v4=$(ip_of "$2")
	v6=$(ip6_of "$2")
	[ -z "$v4" ] || defines+=(-D "IP_$1=$v4")
	[ -z "$v6" ] || defines+=(-D "IP6_$1=$v6")
}

# What a host on the wan needs to reach every lan's IPv6 prefix: the route through
# that lan's router, other than to a lan of its own.
routes6=()
for name in "${nodes[@]}"
do
	[ "${node_role[$name]}" = router ] || continue
	set -- ${node_addrs[$name]}
	gw6=$(ip6_of "$1")
	[ -n "$gw6" ] && [ -n "${net_subnet6[${2%%.*}]}" ] || continue
	routes6+=("${2%%.*}|${net_subnet6[${2%%.*}]}::/64 via $gw6")
done

routes6_for() {
	local route out=
	for route in "${routes6[@]}"
	do
		[ "${route%%|*}" = "$2" ] && continue
		out="$out${route#*|};"
	done
	echo "$out"
}

# A shell prefix that waits out duplicate address detection and adds those routes.
wan_setup() {
	local route setup='while ip -6 addr show | grep -q tentative; do sleep 0.05; done;'
	local IFS=';'
	for route in $(routes6_for "$1" "")
	do
		[ -n "$route" ] && setup="$setup ip -6 route replace $route &&"
	done
	echo "$setup"
}

hosts=()
agent_env=()
for name in "${nodes[@]}"
do
	[ "${node_role[$name]}" = drop ] || continue
	set -- ${node_addrs[$name]}
	hosts+=(--add-host "drop.test:$(ip_of "$1")" --add-host "ipv4.drop.test:$(ip_of "$1")")
	v6=$(ip6_of "$1")
	[ -z "$v6" ] || hosts+=(--add-host "ipv6.drop.test:$v6")
	agent_env=(-e SSL_CERT_FILE=/run/qdc/drop/ca.pem)
	defines+=(-D DROP=drop.test)
done

common=(--label "quickdc-net.run=$run" "${hosts[@]}" --hostname)

# --- routers first, so a client's route is there before its qdc dials out

declare -A gateway gateway6
for name in "${nodes[@]}"
do
	[ "${node_role[$name]}" = router ] || continue
	set -- ${node_addrs[$name]}
	wan_ip=$(ip_of "$1")
	lan_ip=$(ip_of "$2")
	gateway[${2%%.*}]=$lan_ip
	gateway6[${2%%.*}]=$(ip6_of "$2")
	define_ips "$name" "$1"
	podman run -d --name "$run-$name" "${common[@]}" "$name" $(network_args "$name") \
		--cap-add NET_ADMIN --sysctl net.ipv4.ip_forward=1 --sysctl net.ipv6.conf.all.forwarding=1 \
		-e WAN6="$(ip6_of "$1")" -e LAN6="$(ip6_of "$2")" -e FW6="${node_fw6[$name]}" \
		-e ROUTES6="$(routes6_for "$name" "${2%%.*}")" -e LINK="${node_link[$name]}" -e CUT="${node_cut[$name]}" \
		-v "$here/router:/opt/router:ro" -v "$rundir:/run/qdc" \
		"$router_image" /opt/router/router.sh "$wan_ip" "$lan_ip" "${node_nat[$name]}" \
		"/run/qdc/log/$name.ready" "${node_upnp[$name]}" >/dev/null 2>"$rundir/log/$name.run" \
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
	[ "${node_role[$name]}" = drop ] || continue
	set -- ${node_addrs[$name]}
	define_ips "$name" "$1"
	[ -x "${DROPD:-}" ] || skip "no dropd (set DROPD to DROP's server binary)"
	mkdir -p "$rundir/drop"
	podman run -d --name "$run-$name" "${common[@]}" "$name" $(network_args "$name") --cap-add NET_ADMIN \
		-v "$here/drop:/opt/drop:ro" -v "$(readlink -f "$DROPD"):/opt/dropd:ro" -v "$rundir:/run/qdc" \
		"$router_image" sh -c "$(wan_setup "$name") exec /opt/drop/drop.sh /opt/dropd /run/qdc/drop $(ip_of "$1") $(ip6_of "$1") \
			>/run/qdc/log/$name.err 2>&1" >/dev/null 2>"$rundir/log/$name.run" \
		|| skip "cannot start $name: $(cat "$rundir/log/$name.run")"
done

for name in "${nodes[@]}"
do
	case "${node_role[$name]}" in
	router) wait_for -f "$rundir/log/$name.ready" "$name" ;;
	drop) wait_for -f "$rundir/drop/ready" "$name" ;;
	esac
done

# --- agents

connect=()
qdc_dir=$(dirname "$qdc")
for name in "${nodes[@]}"
do
	role=${node_role[$name]}
	[ "$role" = router ] || [ "$role" = drop ] && continue
	set -- ${node_addrs[$name]}
	define_ips "$name" "$1"
	caps=(--cap-add NET_ADMIN)
	if [ "$role" = client ]
	then
		lan=${1%%.*}
		[ -n "${gateway[$lan]+x}" ] || fail "$topology: no router on $lan for $name"
		route='while ip -6 addr show | grep -q tentative; do sleep 0.05; done;'
		[ -z "${gateway[$lan]}" ] || route="$route ip route replace default via ${gateway[$lan]} &&"
		[ -z "${gateway6[$lan]}" ] || route="$route ip -6 route replace default via ${gateway6[$lan]} &&"
	else
		route=$(wan_setup "$name")
	fi
	cores=
	for also in ${node_also[$name]//,/ }
	do
		cores="$cores --core $also"
		connect+=(--connect "$also=$rundir/sock/$name")
	done
	cores="$cores --core $name"
	podman run -d --name "$run-$name" "${common[@]}" "$name" $(network_args "$name") "${caps[@]}" "${agent_env[@]}" \
		-v "$qdc_dir:/opt/qdc:ro" -v "$rundir:/run/qdc" \
		"$host_image" sh -c "$route exec /opt/qdc/$(basename "$qdc") --serve /run/qdc/sock/$name \
			$cores --home /run/qdc/home >/run/qdc/log/$name.out 2>/run/qdc/log/$name.err" \
		>/dev/null 2>"$rundir/log/$name.run" \
		|| skip "cannot start $name: $(cat "$rundir/log/$name.run")"
	connect+=(--connect "$name=$rundir/sock/$name")
done

for name in "${nodes[@]}"
do
	case "${node_role[$name]}" in
	client|public) wait_for -S "$rundir/sock/$name" "$name" ;;
	esac
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
		case "${node_role[$name]}" in
		router)
			[ -f "$rundir/log/$name.upnp" ] || continue
			echo "--- miniupnpd on $name" >&2
			tail -n 30 "$rundir/log/$name.upnp" >&2
			;;
		drop)
			echo "--- reachability server $name" >&2
			tail -n 30 "$rundir/log/$name.err" >&2
			;;
		*)
			echo "--- agent $name" >&2
			tail -n 50 "$rundir/log/$name.err" >&2
			;;
		esac
	done
	exit 1
fi
exit 0
