#!/bin/sh
#
# qdc reaches the core through api/protocol.h - commands in, events out - and
# nothing else, so whatever it can do the GUI can do through the same Api. One
# file owns the cores and their threads and may include api/core.h for that; no
# other may include any header of the core's, samurai's or the libraries below.
#
# Usage: only-the-protocol.sh <cli source directory> <core source directory>

set -eu

cli="$1"
core="$2"
status=0

for path in "$cli"/*.cpp "$cli"/*.h
do
	file=$(basename "$path")

	allowed="api/protocol.h"
	[ "$file" = "runner.cpp" ] && allowed="api/protocol.h api/core.h"

	sed -n 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*\([<"][^>"]*[>"]\).*/\1/p' "$path" |
	while read -r include
	do
		name=$(printf '%s' "$include" | sed 's/^.\(.*\).$/\1/')
		ok=0

		case "$include" in
			\"*)
				for header in $allowed
				do
					[ "$name" = "$header" ] && ok=1
				done
				[ "$ok" -eq 0 ] && [ -f "$cli/$name" ] && ok=1
				;;
			\<*)
				ok=1
				case "$name" in
					samurai/*|quickdc/*|drop/*|openssl/*) ok=0 ;;
				esac
				[ -f "$core/$name" ] && ok=0
				;;
		esac

		if [ "$ok" -eq 0 ]
		then
			echo "$file: includes $include; only $allowed may be included from the core." >&2
			echo "  Submit a command and take the answer from the core's events." >&2
			exit 1
		fi
	done || status=1
done

exit $status
