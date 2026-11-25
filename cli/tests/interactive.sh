#!/bin/sh
#
# Drive qdc interactively over a pipe: help, a typo that is reported without
# ending the run, and a command whose effect is seen as an event and on disk.
#
# Usage: interactive.sh <qdc>

set -u

qdc="$1"

home=$(mktemp -d "${TMPDIR:-/tmp}/qdc-interactive-XXXXXX") || exit 1
trap 'rm -rf "$home"' EXIT

failed=0
fail()
{
	echo "$1" >&2
	failed=1
}

"$qdc" --home "$home" --free-port PORT -i >"$home/stdout" 2>"$home/stderr" <<'SCRIPT'
help hub-account
preset application Network Mode advanced
preset application Network Bind 127.0.0.1
preset application Network Bind6 ""
preset application Network Port ${PORT}
hub-acount oper secret operator
hub-account oper secret operator
wait LocalHubState accounts.0.nick=oper
help
quit
SCRIPT
status=$?

[ "$status" -eq 1 ] || fail "qdc exited $status, expected 1 for the typo"
grep -qF -- "  hub-account <nick> <password> <user|operator|admin>" "$home/stderr" \
	|| fail "help did not give the usage of hub-account"
grep -qF -- "stdin:6: unknown command 'hub-acount'" "$home/stderr" || fail "the typo was not reported"
[ "$(grep -cF -- "  wait <Event>" "$home/stderr")" -eq 1 ] || fail "help did not list every command"
grep -qF -- "[a] LocalHubState request=1 enabled=false" "$home/stdout" || fail "no readable LocalHubState"
grep -qF -- "    accounts.0: nick=oper credential=Operator" "$home/stdout" || fail "the account is not shown"
grep -qF -- 'name="oper"' "$home/a/hub/acl.xml" || fail "the account was not written to the access list"

rm -rf "$home/a"
"$qdc" --home "$home" --free-port PORT --json -i >"$home/json" 2>>"$home/stderr" <<'SCRIPT'
preset application Network Mode advanced
preset application Network Bind 127.0.0.1
preset application Network Bind6 ""
preset application Network Port ${PORT}
status
wait CoreStatus request=1
SCRIPT
status=$?
[ "$status" -eq 0 ] || fail "qdc --json -i exited $status"
grep -qF -- '"event":"CoreStatus","request":1' "$home/json" || fail "--json did not print JSON"

if [ "$failed" -ne 0 ]
then
	for f in stdout stderr json
	do
		echo "--- $f" >&2
		cat "$home/$f" >&2
	done
fi

exit $failed
