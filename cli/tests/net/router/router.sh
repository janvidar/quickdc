#!/bin/sh
#
# Make this container a router: router.sh WAN_IP LAN_IP NAT READY_FILE [UPNP]
#
# NAT is one of
#   home       masquerade (Linux keeps the source port when it can, so the
#              mapping is endpoint-independent in practice) and drop whatever
#              inbound the lan did not ask for - a typical home router
#   symmetric  a random port per connection, same filtering
#   none       plain routing, no NAT, nothing dropped
#
# UPNP "on" runs miniupnpd (UPnP IGD, NAT-PMP, PCP) on the lan: what it maps is
# forwarded through the filter above. Its log is READY_FILE's directory/NAME.upnp.
#
# Either address may be empty where that side has no IPv4. From the environment:
#   WAN6, LAN6  the IPv6 addresses, where the networks have them. IPv6 is routed,
#               never translated; with NAT other than none, new inbound IPv6 is
#               dropped as a home router does, unless FW6=open
#   ROUTES6     "PREFIX via ADDR;..." - the other lans' prefixes, via their routers
#   LINK        netem options for both directions (e.g. "delay 75ms 5ms loss 2%")
#   CUT         "BYTES:SECONDS[:reboot]": once BYTES have been forwarded, drop
#               everything for SECONDS, then restore the link; reboot also
#               forgets every connection, as a router that restarted would

set -eu

wan_ip="$1"
lan_ip="$2"
nat="$3"
ready="$4"
upnp="${5:-off}"
wan6="${WAN6:-}"
lan6="${LAN6:-}"
fw6="${FW6:-closed}"

iface_of() {
	[ -n "$1" ] || return 0
	ip -o addr show | awk -v ip="$1" '{ split($4, a, "/"); if (a[1] == ip) print $2 }'
}

# A static IPv6 address is unusable until duplicate address detection is done.
while ip -6 addr show | grep -q tentative
do
	sleep 0.05
done

wan=$(iface_of "$wan_ip")
[ -n "$wan" ] || wan=$(iface_of "$wan6")
lan=$(iface_of "$lan_ip")
[ -n "$lan" ] || lan=$(iface_of "$lan6")
[ -n "$wan" ] && [ -n "$lan" ] || { echo "router: no interface for $wan_ip$wan6 or $lan_ip$lan6" >&2; exit 1; }

echo "${ROUTES6:-}" | tr ';' '\n' | while read -r route
do
	[ -z "$route" ] || ip -6 route replace $route
done

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
		ct status dnat accept
	}
}
NFT
fi

if [ -n "$wan6" ] && [ -n "$masq" ]
then
	open=
	[ "$fw6" = open ] && open="iifname \"$wan\" oifname \"$lan\" accept"
	nft -f - <<NFT
table ip6 filter {
	chain forward {
		type filter hook forward priority filter; policy drop;
		ct state established,related accept
		meta l4proto icmpv6 accept
		iifname "$lan" oifname "$wan" accept
		$open
	}
}
NFT
fi

if [ -n "${LINK:-}" ]
then
	tc qdisc add dev "$wan" root netem $LINK
	tc qdisc add dev "$lan" root netem $LINK
fi

if [ -n "${CUT:-}" ]
then
	cut_bytes=${CUT%%:*}
	cut_seconds=${CUT#*:}
	cut_reboot=
	case "$cut_seconds" in
	*:reboot) cut_seconds=${cut_seconds%:reboot}; cut_reboot=1 ;;
	esac
	nft -f - <<NFT
table inet cut {
	chain forward {
		type filter hook forward priority filter - 2; policy accept;
		counter
	}
}
NFT
	(
		until [ "$(nft list chain inet cut forward | awk '/counter/ { print $5 }')" -ge "$cut_bytes" ]
		do
			sleep 0.05
		done
		for dev in "$wan" "$lan"
		do
			tc qdisc replace dev "$dev" root netem loss 100%
		done
		echo "cut $(date +%s.%N)" >>"$(dirname "$ready")/$(hostname).link"
		sleep "$cut_seconds"
		[ -z "$cut_reboot" ] || conntrack -F 2>/dev/null
		for dev in "$wan" "$lan"
		do
			if [ -n "${LINK:-}" ]
			then
				tc qdisc replace dev "$dev" root netem $LINK
			else
				tc qdisc del dev "$dev" root
			fi
		done
		echo "restored $(date +%s.%N)" >>"$(dirname "$ready")/$(hostname).link"
	) &
fi

if [ "$upnp" = on ]
then
	# miniupnpd's own chains, in a table of its own; the filter above lets
	# through what it maps (ct status dnat).
	nft -f - <<NFT
table inet miniupnpd {
	chain forward {
		type filter hook forward priority filter - 1; policy accept;
		jump miniupnpd
	}
	chain miniupnpd { }
	chain prerouting {
		type nat hook prerouting priority dstnat; policy accept;
		jump prerouting_miniupnpd
	}
	chain postrouting {
		type nat hook postrouting priority srcnat - 1; policy accept;
		jump postrouting_miniupnpd
	}
	chain prerouting_miniupnpd { }
	chain postrouting_miniupnpd { }
}
NFT
	cat >/etc/miniupnpd/test.conf <<CONF
ext_ifname=$wan
ext_ip=$wan_ip
ext_allow_private_ipv4=yes
ipv6_disable=yes
listening_ip=$lan
http_port=5000
enable_natpmp=yes
enable_pcp_pmp=yes
secure_mode=yes
system_uptime=yes
notify_interval=30
uuid=3d3cec3a-8cf0-11e0-98ee-001a6bd2d07b
upnp_table_name=miniupnpd
upnp_nat_table_name=miniupnpd
upnp_forward_chain=miniupnpd
upnp_nat_chain=prerouting_miniupnpd
upnp_nat_postrouting_chain=postrouting_miniupnpd
allow 1024-65535 0.0.0.0/0 1024-65535
deny 0-65535 0.0.0.0/0 0-65535
CONF
	miniupnpd -d -f /etc/miniupnpd/test.conf >"$(dirname "$ready")/$(hostname).upnp" 2>&1 &
	tries=0
	until ss -ltn | grep -q ":5000 "
	do
		tries=$((tries + 1))
		[ $tries -lt 200 ] || { echo "router: miniupnpd did not start" >&2; exit 1; }
		sleep 0.05
	done
fi

echo "$wan $lan $nat $upnp" >"$ready"
exec sleep 2147483647
