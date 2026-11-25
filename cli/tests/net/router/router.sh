#!/bin/sh
#
# Make this container a router: router.sh WAN_IP LAN_IP NAT READY_FILE
#
# NAT is one of
#   home       masquerade (Linux keeps the source port when it can, so the
#              mapping is endpoint-independent in practice) and drop whatever
#              inbound the lan did not ask for - a typical home router
#   symmetric  a random port per connection, same filtering
#   none       plain routing, no NAT, nothing dropped

set -eu

wan_ip="$1"
lan_ip="$2"
nat="$3"
ready="$4"

iface_of() {
	ip -o -4 addr show | awk -v ip="$1" '{ split($4, a, "/"); if (a[1] == ip) print $2 }'
}

wan=$(iface_of "$wan_ip")
lan=$(iface_of "$lan_ip")
[ -n "$wan" ] && [ -n "$lan" ] || { echo "router: no interface for $wan_ip or $lan_ip" >&2; exit 1; }

case "$nat" in
home)      masq="masquerade" ;;
symmetric) masq="masquerade fully-random" ;;
none)      masq="" ;;
*)         echo "router: unknown nat '$nat'" >&2; exit 1 ;;
esac

if [ -n "$masq" ]
then
	nft -f - <<NFT
table ip nat {
	chain postrouting {
		type nat hook postrouting priority srcnat;
		oifname "$wan" $masq
	}
}
table ip filter {
	# Silence, not a reset, for what nobody inside asked for: a reset would
	# end a traversal attempt that a drop leaves to its retry.
	chain input {
		type filter hook input priority filter; policy accept;
		iifname "$wan" ct state new drop
	}
	chain forward {
		type filter hook forward priority filter; policy drop;
		ct state established,related accept
		iifname "$lan" oifname "$wan" accept
	}
}
NFT
fi

echo "$wan $lan $nat" >"$ready"
exec sleep 2147483647
