#!/bin/sh
#
# Run qdc over a home of its own and check how it ends.
#
# Usage: scenario.sh <exit status> <text stderr must hold, !text it must not, or -> <qdc> [arguments...]

set -u

expected="$1"
needle="$2"
qdc="$3"
shift 3

home=$(mktemp -d "${TMPDIR:-/tmp}/qdc-scenario-XXXXXX") || exit 1
trap 'rm -rf "$home"' EXIT

"$qdc" --home "$home" "$@" >"$home/stdout" 2>"$home/stderr"
status=$?

failed=0

if [ "$status" -ne "$expected" ]
then
	echo "qdc exited $status, expected $expected" >&2
	failed=1
fi

case "$needle" in
-)
	;;
!*)
	if grep -qF -- "${needle#!}" "$home/stderr"
	then
		echo "stderr says what it must not: ${needle#!}" >&2
		failed=1
	fi
	;;
*)
	if ! grep -qF -- "$needle" "$home/stderr"
	then
		echo "stderr does not say: $needle" >&2
		failed=1
	fi
	;;
esac

if [ "$failed" -ne 0 ]
then
	echo "--- stdout" >&2
	cat "$home/stdout" >&2
	echo "--- stderr" >&2
	cat "$home/stderr" >&2
fi

exit $failed
