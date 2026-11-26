#!/bin/sh
#
# Run a script with each core in a qdc agent of its own, driven by a controller,
# and check how the controller ends.
#
# Usage: remote.sh <exit status> <text stderr must hold, or -> <qdc> [--kill CORE TEXT] [--wrong-key] <script> <core...>
#
# The agents and controller share a QDC_AGENT_KEY; --wrong-key gives the
# controller another one.
#
# --kill CORE TEXT kills the agent of CORE once the controller's stderr says TEXT,
# and then the controller must end within 20 seconds.

set -u

expected="$1"
needle="$2"
qdc="$3"
shift 3

kill_core=
kill_text=
if [ "$1" = "--kill" ]
then
	kill_core="$2"
	kill_text="$3"
	shift 3
fi

controller_key=
if [ "$1" = "--wrong-key" ]
then
	controller_key=not-the-key
	shift
fi

script="$1"
shift

QDC_AGENT_KEY="key-$$-$(date +%s%N)"
export QDC_AGENT_KEY

home=$(mktemp -d "${TMPDIR:-/tmp}/qdc-remote-XXXXXX") || exit 1
agents=
trap 'for pid in $agents; do kill -9 $pid 2>/dev/null; done; rm -rf "$home"' EXIT

connect=
for core in "$@"
do
	"$qdc" --serve 127.0.0.1:0 --core "$core" --home "$home" >"$home/agent-$core.out" 2>"$home/agent-$core.err" &
	pid=$!
	agents="$agents $pid"
	eval "pid_$core=$pid"

	tries=0
	until grep -q '^serving ' "$home/agent-$core.out" 2>/dev/null
	do
		tries=$((tries + 1))
		if [ $tries -gt 600 ] || ! kill -0 $pid 2>/dev/null
		then
			echo "the agent for $core did not start" >&2
			cat "$home/agent-$core.err" >&2
			exit 1
		fi
		sleep 0.05
	done
	endpoint=$(sed -n 's/^serving //p' "$home/agent-$core.out")
	connect="$connect --connect $core=$endpoint"
done

started=$(date +%s)
QDC_AGENT_KEY="${controller_key:-$QDC_AGENT_KEY}" "$qdc" --home "$home" $connect --free-port PORT --quiet "$script" >"$home/stdout" 2>"$home/stderr" &
controller=$!

failed=0

if [ -n "$kill_core" ]
then
	tries=0
	until grep -qF -- "$kill_text" "$home/stderr"
	do
		tries=$((tries + 1))
		if [ $tries -gt 1200 ] || ! kill -0 $controller 2>/dev/null
		then
			echo "the controller never said: $kill_text" >&2
			failed=1
			break
		fi
		sleep 0.05
	done
	started=$(date +%s)
	eval "kill -9 \$pid_$kill_core"
fi

wait $controller
status=$?

if [ -n "$kill_core" ] && [ $(( $(date +%s) - started )) -gt 20 ]
then
	echo "the controller took $(( $(date +%s) - started ))s to see its agent go" >&2
	failed=1
fi

if [ "$status" -ne "$expected" ]
then
	echo "the controller exited $status, expected $expected" >&2
	failed=1
fi

if [ "$needle" != "-" ] && ! grep -qF -- "$needle" "$home/stderr"
then
	echo "stderr does not say: $needle" >&2
	failed=1
fi

# Agents end with their controller, however it ends - except one they turned
# away, which they keep waiting past. The controller stops at the first.
if [ -n "$controller_key" ]
then
	grep -q "refused a controller" "$home/agent-$1.err" || { echo "agent $1 did not refuse" >&2; failed=1; }
	agents_to_wait=
else
	agents_to_wait=$agents
fi

for pid in $agents_to_wait
do
	tries=0
	while kill -0 $pid 2>/dev/null && [ "$(cut -d' ' -f3 /proc/$pid/stat 2>/dev/null)" != Z ]
	do
		tries=$((tries + 1))
		if [ $tries -gt 400 ]
		then
			echo "agent $pid outlived its controller" >&2
			failed=1
			break
		fi
		sleep 0.05
	done
done

if [ "$failed" -ne 0 ]
then
	echo "--- stdout" >&2
	cat "$home/stdout" >&2
	echo "--- stderr" >&2
	cat "$home/stderr" >&2
	for core in "$@"
	do
		echo "--- agent $core" >&2
		cat "$home/agent-$core.err" >&2
	done
fi

exit $failed
