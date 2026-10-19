#!/usr/bin/env bash
#
# test_mlw_driver.sh - self-test of mlw_driver.py against fake_streamer.py.
# No Sunshine, no real streamer, no network. Uses dummy PEM strings only.
#
# Usage: tests/e2e/test_mlw_driver.sh

set -uo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
drv=("python3" "-I" "$here/mlw_driver.py")
fake="$here/fake_streamer.py"

dir=$(mktemp -d)
cleanup() {
  "${drv[@]}" --state-dir "$dir/state" cleanup >/dev/null 2>&1 || true
  rm -rf -- "${dir:?}"
}
trap cleanup EXIT

fails=0
ok() { printf 'ok   %s\n' "$1"; }
bad() { printf 'FAIL %s\n' "$1"; fails=$((fails + 1)); }
check() { # check <name> <command...>
  local name=$1
  shift
  if "$@"; then ok "$name"; else bad "$name"; fi
}
eq() { [[ $1 == "$2" ]]; }

cat >"$dir/data.json" <<'JSON'
{"version":"3","users":{"0":{"role_id":0,"name":"u","password":null,"client_unique_id":"fake-client"}},
 "hosts":{"0":{"owner":null,"address":"fake.invalid","http_port":47989,
  "pair_info":{"client_private_key":{"tag":"PRIVATE KEY","contents":[1,2,3]},
               "client_certificate":{"tag":"CERTIFICATE","contents":[4,5,6]},
               "server_certificate":{"tag":"CERTIFICATE","contents":[7,8,9]}},
  "cache":{"name":"x","mac":null}}},
 "roles":{}}
JSON
before=$(cksum <"$dir/data.json")

common=(--state-dir "$dir/state" --streamer "$fake" --data-json "$dir/data.json" --app-id 7
  --ready-timeout 10 --stop-timeout 4)
log="$dir/state/streamer.log"
status_field() { # status_field <jq-like python expression on d>
  "${drv[@]}" --state-dir "$dir/state" status | python3 -I -c "import json,sys; d=json.load(sys.stdin); print($1)"
}

echo "== help / errors"
check "help exits 0" bash -c '"$@" --help >/dev/null' _ "${drv[@]}"
"${drv[@]}" --state-dir "$dir/state" --streamer "$fake" start >/dev/null 2>&1
check "start without --data-json fails" eq $? 1

echo "== clean start / stop"
check "start" "${drv[@]}" "${common[@]}" start
check "streamer alive" eq "$(status_field "d['streamer']['alive']")" True
check "holder alive" eq "$(status_field "d['holder']['alive']")" True
"${drv[@]}" "${common[@]}" start >/dev/null 2>&1
check "second start refused" eq $? 1
sleep 0.5
out=$("${drv[@]}" "${common[@]}" stop)
check "stop exit code" eq $? 0
check "stop prints exit_code=0" eq "$out" "exit_code=0"
check "status exit_code 0" eq "$(status_field "d['exit_code']")" 0
check "log: init" grep -q 'fake: init ok app_id=7 client_id=fake-client' "$log"
check "log: StartStream settings" grep -q 'got StartStream 1280x720@30 5000kbps codecs=1' "$log"
check "log: rtt echoed" grep -q 'fake: got rtt echo' "$log"
check "log: Stop received" grep -q 'fake: got Stop' "$log"
check "log: PEM block filtered" bash -c "! grep -q -e 'BEGIN' -e 'QUJDREVGRw' -e 'END FAKE' '$log'"
check "log: no fault env" grep -q 'fault= no_watchdog=' "$log"
check "stop after exit reports code" eq "$("${drv[@]}" "${common[@]}" stop)" "exit_code=0 (already exited)"

echo "== kill"
check "restart" "${drv[@]}" "${common[@]}" start
check "kill" "${drv[@]}" "${common[@]}" kill
check "streamer dead" eq "$(status_field "d['streamer']['alive']")" False
check "exit_code -9" eq "$(status_field "d['exit_code']")" -9

echo "== freeze / cleanup"
check "restart" "${drv[@]}" "${common[@]}" start
check "freeze" "${drv[@]}" "${common[@]}" freeze
check "streamer stopped" eq "$(status_field "d['streamer']['stopped']")" True
check "cleanup" "${drv[@]}" "${common[@]}" cleanup
check "streamer gone" eq "$(status_field "d['streamer']['alive']")" False
check "no pid files" bash -c "[[ ! -e '$dir/state/streamer.pid' && ! -e '$dir/state/holder.pid' ]]"
check "restart after cleanup" "${drv[@]}" "${common[@]}" start
check "cleanup again" "${drv[@]}" "${common[@]}" cleanup

echo "== wedge (no watchdog): stop returns 3"
export FAKE_WEDGE=1
check "start wedge" "${drv[@]}" "${common[@]}" --fault wedge-audio-stop --no-watchdog start
check "log: fault env" grep -q 'fault=wedge-audio-stop no_watchdog=1' "$log"
"${drv[@]}" "${common[@]}" stop >/dev/null
check "stop returns 3" eq $? 3
check "still alive" eq "$(status_field "d['streamer']['alive']")" True
check "cleanup kills wedged" "${drv[@]}" "${common[@]}" cleanup
check "wedged streamer gone" eq "$(status_field "d['streamer']['alive']")" False

echo "== wedge with watchdog: exit code 2"
export FAKE_WEDGE=2
check "start" "${drv[@]}" "${common[@]}" --fault wedge-audio-stop start
out=$("${drv[@]}" "${common[@]}" stop)
check "stop prints exit_code=2" eq "$out" "exit_code=2"
unset FAKE_WEDGE

echo "== misc"
check "data.json untouched" eq "$(cksum <"$dir/data.json")" "$before"
check "log has no key bytes" bash -c "! grep -q -e '\"contents\"' -e 'PRIVATE KEY' '$log'"
echo '{"version":"3","users":{},"hosts":{"0":{"address":"a","http_port":1,"pair_info":null},"1":{"address":"b","http_port":1,"pair_info":null}},"roles":{}}' >"$dir/two.json"
msg=$("${drv[@]}" "${common[@]}" --data-json "$dir/two.json" start 2>&1)
check "several hosts need --host-id" bash -c "[[ '$msg' == *'--host-id'* ]]"
"${drv[@]}" "${common[@]}" cleanup

if ((fails)); then
  echo "$fails check(s) failed"
  exit 1
fi
echo "all checks passed"
