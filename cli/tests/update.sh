#!/bin/sh
#
# The updater against feeds served on the loopback: builds one feed directory per
# case, signed with the test key or deliberately wrong, then runs update.qdc
# over them.
#
# Usage: update.sh <qdc> <update-feed> <tests directory>

set -eu

qdc="$1"
feeder="$2"
tests="$3"

work=$(mktemp -d "${TMPDIR:-/tmp}/qdc-update-XXXXXX")
trap 'rm -rf "$work"' EXIT

case "$(uname -s)" in
	Linux)  kind=.AppImage ;;
	Darwin) kind=.zip ;;
	*)      kind=.exe ;;
esac

package="$work/package$kind"
head -c 200000 /dev/urandom > "$package"

# One case: <name> <version> [update-feed options...]
feed() {
	name=$1 version=$2
	shift 2
	file="QuickDC-$version$kind"
	mkdir -p "$work/feeds/$name/test/$version"
	cp "$package" "$work/feeds/$name/test/$version/$file"
	"$feeder" --seed "$tests/updates/test.seed" --channel test --version "$version" \
		--url "$version/$file" --package "$package" "$@" > "$work/feed.json"
	platform=$(sed -n 's/.*"platform": "\([^"]*\)".*/\1/p' "$work/feed.json")
	mv "$work/feed.json" "$work/feeds/$name/test/$platform.json"
}

feed good 99.0.0
feed old 0.0.1
feed forged 99.0.0 --seed "$tests/updates/other.seed"
feed lying 99.0.0 --sha256 0000000000000000000000000000000000000000000000000000000000000000
feed elsewhere 99.0.0 --channel nightly
feed outside 99.0.0 --url ../../elsewhere/QuickDC-99.0.0$kind

exec "$tests/scenario.sh" 0 - "$qdc" --free-port HTTP -D FEEDS="$work/feeds" --quiet "$tests/update.qdc"
