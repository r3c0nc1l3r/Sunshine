#!/usr/bin/env bash
#
# leak_cycles.sh - repeated stream start/stop cycles that check a running
# Sunshine for session and capture-buffer leaks (threads, fds, DRM memory,
# ~15 MiB dmabuf buffers, active-session count).
#
# Runs on the test host. See tests/e2e/README.md for inputs and criteria.
#
# Usage:
#   leak_cycles.sh                    run every scenario in SCENARIOS
#   leak_cycles.sh --dry-run [PID]    one measurement pass, prints metrics
#   leak_cycles.sh --help             show this text
#
# Exit status: 0 when every scenario passes, 1 when any scenario fails,
# 2 on setup or configuration errors.

set -euo pipefail
shopt -s nullglob

# ---------------------------------------------------------------------------
# Configuration (environment variables, see README.md)
# ---------------------------------------------------------------------------
SUNSHINE_UNIT=${SUNSHINE_UNIT:-sunshine-lanternbox.service}
SUNSHINE_PID=${SUNSHINE_PID:-}
SUNSHINE_LOG_CMD=${SUNSHINE_LOG_CMD:-journalctl --user -u $SUNSHINE_UNIT -o cat --since=@SINCE@}
SUNSHINE_API=${SUNSHINE_API:-https://localhost:47990}
SUNSHINE_API_USER=${SUNSHINE_API_USER:-}
SUNSHINE_API_PASS=${SUNSHINE_API_PASS:-}
CLIENT_START=${CLIENT_START:-}
CLIENT_STOP=${CLIENT_STOP:-}
CLIENT_KILL=${CLIENT_KILL:-}
CLIENT_WEDGE=${CLIENT_WEDGE:-}
CLIENT_DROP=${CLIENT_DROP:-}
CLIENT_RECONNECT=${CLIENT_RECONNECT:-}
CLIENT_CLEANUP=${CLIENT_CLEANUP:-}
N=${N:-20}
SCENARIOS=${SCENARIOS:-clean kill wedge drop}
STREAM_SECONDS=${STREAM_SECONDS:-10}
SETTLE_TIMEOUT=${SETTLE_TIMEOUT:-30}
PING_TIMEOUT=${PING_TIMEOUT:-10}
WATCHDOG_DEADLINE=${WATCHDOG_DEADLINE:-0}
MODE=${MODE:-sunshine-only}
TOLERANCE_THREADS=${TOLERANCE_THREADS:-2}
TOLERANCE_FDS=${TOLERANCE_FDS:-2}
TOLERANCE_MIB=${TOLERANCE_MIB:-64}
HOOK_TIMEOUT=${HOOK_TIMEOUT:-120}
OUT=${OUT:-./leak-cycles-$(date +%F)}

readonly MIB=1048576
readonly BUF_LO=$((14 * MIB))
readonly BUF_HI=$((16 * MIB))
readonly METRICS=(threads render dmabuf buf15 drm_mem drm_total drm_res)

# awk program: input lines are "<fd> <fdinfo line>". Produces one line:
# "<drm fd count> <memory MiB> <total MiB> <resident MiB>", where each MiB sum
# is deduplicated per drm-client-id (keeping the largest value per client and
# key) so several fds of one client are not double counted.
readonly DRM_AWK='
{
  id = $1
  line = substr($0, length(id) + 2)
  if (line ~ /^drm-driver:/) {
    drm[id] = 1
  } else if (line ~ /^drm-client-id:/) {
    v = line
    sub(/^drm-client-id:[ \t]*/, "", v)
    cid[id] = v
  } else if (line ~ /^drm-(memory|total|resident)-[^:]+:/) {
    key = line
    sub(/:.*/, "", key)
    v = line
    sub(/^[^:]+:[ \t]*/, "", v)
    n = split(v, p, /[ \t]+/)
    num = p[1] + 0
    unit = (n >= 2) ? p[2] : "B"
    if (unit == "KiB") mib = num / 1024
    else if (unit == "MiB") mib = num
    else if (unit == "GiB") mib = num * 1024
    else mib = num / 1048576
    if (!((id, key) in val) || mib > val[id, key]) val[id, key] = mib
  }
}
END {
  for (id in drm) fds++
  for (k in val) {
    split(k, parts, SUBSEP)
    client = (parts[1] in cid) ? "c:" cid[parts[1]] : "f:" parts[1]
    ck = client SUBSEP parts[2]
    if (!(ck in cm) || val[k] > cm[ck]) cm[ck] = val[k]
  }
  for (ck in cm) {
    split(ck, parts, SUBSEP)
    fam = parts[2]
    sub(/^drm-/, "", fam)
    sub(/-.*/, "", fam)
    sum[fam] += cm[ck]
  }
  printf "%d %.3f %.3f %.3f\n", fds + 0, sum["memory"] + 0, sum["total"] + 0, sum["resident"] + 0
}'

# ---------------------------------------------------------------------------
# Global state
# ---------------------------------------------------------------------------
PID=""
RUN_START=0
API_OK=0
API_CFG=""
HTTP_CODE=000
dir=""
CSV=""
SETTLED=n/a
BASE_NOTE=""
DMABUF_FDS=()

M_threads=0
M_sessions=n/a
M_render=0
M_dmabuf=0
M_buf15=0
M_drm_fds=0
M_drm_mem=0
M_drm_total=0
M_drm_res=0

CYC_CLOSED=n/a
CYC_GONE=n/a
CYC_RECON=n/a
CYC_REASONS=""
CYC_FAIL=""
CYC_INFO=""
CYC_RESULT=""

declare -a SC_LIST=()
declare -A BASEV=()
declare -A SERIES=()
declare -A SC_RESULT=()
declare -A SC_CYCLES=()
declare -A SC_FAILED=()
declare -A TREND=()

# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------
log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" >&2; }

die() {
  log "ERROR: $*"
  exit 2
}

usage() {
  sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
}

# within A B TOL: exit 0 when |A - B| <= TOL (numbers, may be fractional).
within() {
  awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN { d = a - b; if (d < 0) d = -d; exit (d <= t) ? 0 : 1 }'
}

fail() { CYC_FAIL+="$1;"; }
info() { CYC_INFO+="$1;"; }

sessions_idle() { [[ $1 == 0 || $1 == n/a ]]; }

# Escapes a value for a double-quoted curl config string.
curl_escape() {
  local s=$1
  s=${s//\\/\\\\}
  s=${s//\"/\\\"}
  printf '%s' "$s"
}

need_hook() {
  local name=$1
  [[ -n ${!name:-} ]] || die "hook $name must be set for the selected scenarios"
}

validate_config() {
  local name sc
  local -a sc_list
  for name in N STREAM_SECONDS SETTLE_TIMEOUT PING_TIMEOUT WATCHDOG_DEADLINE \
    TOLERANCE_THREADS TOLERANCE_FDS TOLERANCE_MIB HOOK_TIMEOUT; do
    [[ ${!name} =~ ^[0-9]+$ ]] || die "$name must be a non-negative integer"
  done
  (( N >= 1 )) || die "N must be at least 1"
  case $MODE in
    sunshine-only | full-stack) ;;
    *) die "MODE must be sunshine-only or full-stack" ;;
  esac
  read -ra sc_list <<<"$SCENARIOS"
  (( ${#sc_list[@]} > 0 )) || die "SCENARIOS is empty"
  need_hook CLIENT_START
  for sc in "${sc_list[@]}"; do
    case $sc in
      clean) need_hook CLIENT_STOP ;;
      kill) need_hook CLIENT_KILL ;;
      wedge) need_hook CLIENT_WEDGE ;;
      drop)
        need_hook CLIENT_DROP
        need_hook CLIENT_RECONNECT
        need_hook CLIENT_STOP
        ;;
      *) die "unknown scenario: $sc" ;;
    esac
  done
  if [[ -z $CLIENT_CLEANUP ]]; then
    log "CLIENT_CLEANUP not set: leftover clients will not be killed between cycles"
  fi
}

resolve_pid() {
  if [[ -n $SUNSHINE_PID ]]; then
    PID=$SUNSHINE_PID
  else
    PID=$(systemctl --user show -p MainPID --value "$SUNSHINE_UNIT" 2>/dev/null || true)
  fi
  [[ $PID =~ ^[1-9][0-9]*$ ]] || die "could not resolve a PID (set SUNSHINE_PID or check SUNSHINE_UNIT)"
  [[ -d /proc/$PID/task ]] || die "PID $PID is not a running process"
}

# ---------------------------------------------------------------------------
# /proc measurements (read only: /proc/PID/{task,fd,fdinfo})
# ---------------------------------------------------------------------------
measure_threads() {
  local t=(/proc/"$PID"/task/*)
  M_threads=${#t[@]}
}

# Counts render-node fds and dmabuf fds and remembers the dmabuf fd numbers.
measure_fds() {
  local link target
  M_render=0
  M_dmabuf=0
  DMABUF_FDS=()
  for link in /proc/"$PID"/fd/*; do
    target=$(readlink "$link" 2>/dev/null) || continue
    case $target in
      /dev/dri/renderD*)
        M_render=$((M_render + 1))
        ;;
      /dmabuf:* | anon_inode:dmabuf*)
        M_dmabuf=$((M_dmabuf + 1))
        DMABUF_FDS+=("${link##*/}")
        ;;
    esac
  done
}

# Counts dmabuf fds whose fdinfo size is between 14 and 16 MiB.
measure_buffers() {
  local fd size
  M_buf15=0
  for fd in "${DMABUF_FDS[@]}"; do
    size=$(awk '$1 == "size:" { print $2; exit }' "/proc/$PID/fdinfo/$fd" 2>/dev/null) || size=""
    if [[ $size =~ ^[0-9]+$ ]] && (( size >= BUF_LO && size <= BUF_HI )); then
      M_buf15=$((M_buf15 + 1))
    fi
  done
}

# Sets M_drm_fds and the deduplicated drm memory sums in MiB.
measure_drm() {
  local f id out
  M_drm_fds=0
  M_drm_mem=0
  M_drm_total=0
  M_drm_res=0
  out=$(
    for f in /proc/"$PID"/fdinfo/*; do
      id=${f##*/}
      awk -v id="$id" '{ print id " " $0 }' "$f" 2>/dev/null || true
    done | awk "$DRM_AWK"
  ) || out=""
  if [[ -n $out ]]; then
    read -r M_drm_fds M_drm_mem M_drm_total M_drm_res <<<"$out" || true
  fi
}

measure_proc() {
  measure_threads
  measure_fds
  measure_buffers
  measure_drm
}

metric_now() {
  case $1 in
    threads) printf '%s' "$M_threads" ;;
    render) printf '%s' "$M_render" ;;
    dmabuf) printf '%s' "$M_dmabuf" ;;
    buf15) printf '%s' "$M_buf15" ;;
    drm_mem) printf '%s' "$M_drm_mem" ;;
    drm_total) printf '%s' "$M_drm_total" ;;
    drm_res) printf '%s' "$M_drm_res" ;;
  esac
}

# ---------------------------------------------------------------------------
# Sunshine log and session API
# ---------------------------------------------------------------------------
# read_log SINCE_EPOCH: prints Sunshine log lines from that time on.
read_log() {
  local cmd=${SUNSHINE_LOG_CMD//@SINCE@/$1}
  timeout 30 bash -c "$cmd" 2>/dev/null || true
}

# Prints N from the newest "Session ended" line since RUN_START, or nothing.
newest_session_count() {
  local txt
  txt=$(read_log "$RUN_START")
  printf '%s\n' "$txt" |
    sed -n 's/.*Session ended: reason=[^,]*, active sessions=\([0-9][0-9]*\).*/\1/p' |
    tail -n1 || true
  return 0
}

# Prints the reasons of "Session ended" lines since epoch $1, joined with ';'.
reasons_since() {
  local txt
  txt=$(read_log "$1")
  printf '%s\n' "$txt" |
    sed -n 's/.*Session ended: reason=\([^,]*\),.*/\1/p' |
    paste -sd';' - || true
  return 0
}

# Exit 0 when a "Session ended" line appears in the log since epoch $1.
session_ended_since() {
  local txt
  txt=$(read_log "$1")
  [[ $txt == *"Session ended: reason="* ]]
}

# api_request METHOD PATH [extra curl args]: sets HTTP_CODE, body in $dir/body.
api_request() {
  local method=$1 path=$2
  shift 2
  HTTP_CODE=$(curl -sk --config "$API_CFG" -X "$method" -o "$dir/body" -w '%{http_code}' \
    --max-time 15 "$@" "$SUNSHINE_API$path" 2>/dev/null) || HTTP_CODE=000
  return 0
}

# json_field NAME FILE: prints a string or number field from a JSON file.
json_field() {
  if command -v jq >/dev/null 2>&1; then
    jq -r --arg k "$1" '.[$k] // empty' "$2" 2>/dev/null || true
  else
    sed -n "s/.*\"$1\" *: *\"\\{0,1\\}\\([^\",}]*\\)\"\\{0,1\\}.*/\\1/p" "$2" | head -n1 || true
  fi
  return 0
}

# Prints the number of sessions from GET /api/sessions, or n/a.
api_session_count() {
  local n=""
  api_request GET /api/sessions
  if [[ $HTTP_CODE != 200 ]]; then
    printf 'n/a'
    return 0
  fi
  if command -v jq >/dev/null 2>&1; then
    n=$(jq -r '.sessions | length' "$dir/body" 2>/dev/null) || n=""
  else
    n=$({ grep -o '"id"' "$dir/body" || true; } | wc -l)
    n=${n//[[:space:]]/}
  fi
  if [[ $n =~ ^[0-9]+$ ]]; then
    printf '%s' "$n"
  else
    printf 'n/a'
  fi
  return 0
}

# Prints the id of the first listed session, or nothing.
api_first_session_id() {
  api_request GET /api/sessions
  if [[ $HTTP_CODE != 200 ]]; then
    return 0
  fi
  if command -v jq >/dev/null 2>&1; then
    jq -r '.sessions[0].id // empty' "$dir/body" 2>/dev/null || true
  else
    sed -n 's/.*"id" *: *"\{0,1\}\([0-9][0-9]*\)"\{0,1\}.*/\1/p' "$dir/body" | head -n1 || true
  fi
  return 0
}

# Prints a CSRF token from GET /api/csrf-token (field csrf_token). Fails when absent.
api_csrf() {
  local tok
  api_request GET /api/csrf-token
  [[ $HTTP_CODE == 200 ]] || return 1
  tok=$(json_field csrf_token "$dir/body")
  [[ -n $tok ]] || return 1
  printf '%s' "$tok"
}

# api_close ID: POST /api/sessions/ID/close with a CSRF token.
api_close() {
  local id=$1 tok
  if ! tok=$(api_csrf); then
    log "CSRF token unavailable: cannot close session $id"
    return 1
  fi
  api_request POST "/api/sessions/$id/close" -H "X-CSRF-Token: $tok"
  [[ $HTTP_CODE == 200 ]] || return 1
  grep -Eq '"status" *: *true' "$dir/body"
}

# Sets API_OK. Credentials go to curl through a config file, never the command line.
api_init() {
  API_OK=0
  if [[ -z $SUNSHINE_API_USER || -z $SUNSHINE_API_PASS ]]; then
    log "session API disabled: SUNSHINE_API_USER and SUNSHINE_API_PASS are not both set"
    return 0
  fi
  API_CFG="$dir/curl.conf"
  printf 'user = "%s"\n' "$(curl_escape "$SUNSHINE_API_USER:$SUNSHINE_API_PASS")" >"$API_CFG"
  api_request GET /api/sessions
  case $HTTP_CODE in
    200)
      API_OK=1
      log "session API available"
      ;;
    401 | 403)
      log "session API rejected the request (HTTP $HTTP_CODE): API checks disabled"
      ;;
    *)
      log "session API unavailable (HTTP $HTTP_CODE): API checks disabled"
      ;;
  esac
}

# Prints the active session count: log first, then API, else n/a.
current_sessions() {
  local n
  n=$(newest_session_count)
  if [[ -n $n ]]; then
    printf '%s' "$n"
  elif (( API_OK )); then
    api_session_count
  else
    printf 'n/a'
  fi
  return 0
}

# ---------------------------------------------------------------------------
# Client hooks (operator supplied commands, run with bash -c)
# ---------------------------------------------------------------------------
run_hook() {
  local name=$1 cmd=${!1:-}
  if [[ -z $cmd ]]; then
    log "hook $name is not set"
    return 1
  fi
  timeout "$HOOK_TIMEOUT" bash -c "$cmd" >>"$OUT/hooks.log" 2>&1
}

run_hook_optional() {
  if [[ -n ${!1:-} ]]; then
    run_hook "$1" || log "optional hook $1 returned an error"
  fi
  return 0
}

# ---------------------------------------------------------------------------
# Settling and baseline
# ---------------------------------------------------------------------------
# take_baseline: waits (up to SETTLE_TIMEOUT) for no session, then records the
# baseline metrics for the current scenario.
take_baseline() {
  local deadline=$((SECONDS + SETTLE_TIMEOUT)) sessions
  BASE_NOTE=""
  while :; do
    sessions=$(current_sessions)
    if sessions_idle "$sessions"; then
      break
    fi
    if (( SECONDS >= deadline )); then
      BASE_NOTE="baseline-not-idle"
      break
    fi
    sleep 1
  done
  measure_proc
  M_sessions=$sessions
  SETTLED=n/a
  local m
  for m in "${METRICS[@]}"; do
    BASEV[$m]=$(metric_now "$m")
  done
}

# settle: polls every second until sessions are idle and threads are within
# tolerance of the baseline, or SETTLE_TIMEOUT expires. Leaves M_* measured.
settle() {
  local deadline=$((SECONDS + SETTLE_TIMEOUT)) sessions
  SETTLED=0
  while :; do
    measure_threads
    sessions=$(current_sessions)
    if sessions_idle "$sessions" && within "$M_threads" "${BASEV[threads]}" "$TOLERANCE_THREADS"; then
      SETTLED=1
      break
    fi
    if (( SECONDS >= deadline )); then
      break
    fi
    sleep 1
  done
  measure_proc
  M_sessions=$sessions
}

# ---------------------------------------------------------------------------
# Scenario bodies
# ---------------------------------------------------------------------------
# Sunshine-only wedge: the client keeps pinging but stops reading media.
wedge_sunshine_only() {
  local n id t0 gone=0 i
  if ! (( API_OK )); then
    fail "stuck-session-no-api"
    return 0
  fi
  n=$(api_session_count)
  if ! [[ $n =~ ^[0-9]+$ ]] || (( n == 0 )); then
    fail "session-ended-without-api-close"
    CYC_CLOSED=0
    return 0
  fi
  info "still-listed-after-ping-timeout"
  id=$(api_first_session_id)
  t0=$SECONDS
  if [[ -z $id ]] || ! api_close "$id"; then
    fail "api-close-failed"
    CYC_CLOSED=0
    return 0
  fi
  while (( SECONDS - t0 <= SETTLE_TIMEOUT )); do
    n=$(api_session_count)
    if [[ $n == 0 ]]; then
      gone=1
      break
    fi
    sleep 1
  done
  if (( gone )); then
    CYC_CLOSED=1
    CYC_GONE=$((SECONDS - t0))
  else
    fail "api-close-session-still-listed"
    CYC_CLOSED=0
  fi
  return 0
}

do_wedge() {
  local wedge_epoch window i ended=0 t0
  wedge_epoch=$(date +%s)
  if ! run_hook CLIENT_WEDGE; then
    fail "wedge-hook-failed"
  fi
  if [[ $MODE == full-stack ]]; then
    window=$((PING_TIMEOUT + WATCHDOG_DEADLINE + 5))
    t0=$SECONDS
    while (( SECONDS - t0 <= window )); do
      if session_ended_since "$wedge_epoch"; then
        ended=1
        break
      fi
      if (( API_OK )) && [[ $(api_session_count) == 0 ]]; then
        ended=1
        break
      fi
      sleep 1
    done
    if (( ended )); then
      CYC_GONE=$((SECONDS - t0))
    else
      fail "session-not-ended-within-deadline"
    fi
  else
    sleep $((PING_TIMEOUT + 5))
    wedge_sunshine_only
  fi
  run_hook_optional CLIENT_CLEANUP
  return 0
}

do_drop() {
  local cstart=$1 brief n
  if ! run_hook CLIENT_DROP; then
    fail "drop-hook-failed"
  fi
  sleep $((PING_TIMEOUT + 5))
  if session_ended_since "$cstart"; then
    info "dropped-session-ended-by-itself"
  else
    info "dropped-session-no-end-line"
  fi
  if ! run_hook CLIENT_RECONNECT; then
    fail "reconnect-hook-failed"
  fi
  brief=$(( STREAM_SECONDS < 5 ? STREAM_SECONDS : 5 ))
  sleep "$brief"
  if (( API_OK )); then
    n=$(api_session_count)
    if [[ $n =~ ^[0-9]+$ ]] && (( n > 0 )); then
      CYC_RECON=yes
    else
      CYC_RECON=no
      info "reconnect-no-session-listed"
    fi
  fi
  if ! run_hook CLIENT_STOP; then
    fail "stop-hook-failed"
  fi
  return 0
}

check_cycle() {
  [[ $SETTLED == 1 ]] || fail "not-settled"
  within "$M_threads" "${BASEV[threads]}" "$TOLERANCE_THREADS" || fail "threads"
  within "$M_render" "${BASEV[render]}" "$TOLERANCE_FDS" || fail "render-fds"
  within "$M_dmabuf" "${BASEV[dmabuf]}" "$TOLERANCE_FDS" || fail "dmabuf-fds"
  within "$M_buf15" "${BASEV[buf15]}" "$TOLERANCE_FDS" || fail "buf15-count"
  within "$M_drm_mem" "${BASEV[drm_mem]}" "$TOLERANCE_MIB" || fail "drm-mem"
  within "$M_drm_total" "${BASEV[drm_total]}" "$TOLERANCE_MIB" || fail "drm-total"
  within "$M_drm_res" "${BASEV[drm_res]}" "$TOLERANCE_MIB" || fail "drm-resident"
  return 0
}

# run_cycle SCENARIO: start, stream, end (per scenario), settle, measure, check.
run_cycle() {
  local sc=$1 cstart
  CYC_CLOSED=n/a
  CYC_GONE=n/a
  CYC_RECON=n/a
  CYC_FAIL=""
  CYC_INFO=""
  cstart=$(date +%s)
  if ! run_hook CLIENT_START; then
    fail "start-hook-failed"
  fi
  sleep "$STREAM_SECONDS"
  case $sc in
    clean)
      run_hook CLIENT_STOP || fail "stop-hook-failed"
      ;;
    kill)
      run_hook CLIENT_KILL || fail "kill-hook-failed"
      ;;
    wedge)
      do_wedge
      ;;
    drop)
      do_drop "$cstart"
      ;;
  esac
  settle
  CYC_REASONS=$(reasons_since "$cstart")
  check_cycle
  if [[ -n $CYC_FAIL ]]; then
    CYC_RESULT=FAIL
  else
    CYC_RESULT=PASS
  fi
  return 0
}

# csv_row SCENARIO CYCLE PHASE: appends one row to $CSV.
csv_row() {
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$1" "$2" "$3" "$M_threads" "$M_sessions" "$M_render" "$M_dmabuf" "$M_drm_fds" \
    "$M_drm_mem" "$M_drm_total" "$M_drm_res" "$M_buf15" "$SETTLED" \
    "$CYC_CLOSED" "$CYC_GONE" "$CYC_RECON" "$CYC_REASONS" "$CYC_RESULT" \
    "${CYC_FAIL}${CYC_INFO}" >>"$CSV"
}

# is_trend_increase V...: exit 0 when the values are non-decreasing, the last
# is larger than the first, and there are at least 5 of them.
is_trend_increase() {
  (( $# >= 5 )) || return 1
  printf '%s\n' "$@" | awk '
    NR == 1 { first = $1 + 0 }
    { if (NR > 1 && $1 + 0 < prev) bad = 1; prev = $1 + 0 }
    END { exit (!bad && prev > first) ? 0 : 1 }'
}

run_scenario() {
  local sc=$1 c m
  SC_FAILED[$sc]=0
  SC_CYCLES[$sc]=0
  for m in "${METRICS[@]}"; do
    SERIES["$sc|$m"]=""
  done
  log "scenario $sc: baseline"
  run_hook_optional CLIENT_CLEANUP
  take_baseline
  CYC_CLOSED=n/a
  CYC_GONE=n/a
  CYC_RECON=n/a
  CYC_REASONS=""
  CYC_FAIL=""
  CYC_INFO="$BASE_NOTE"
  CYC_RESULT=baseline
  csv_row "$sc" 0 baseline

  for ((c = 1; c <= N; c++)); do
    log "scenario $sc: cycle $c/$N"
    run_cycle "$sc" "$c"
    csv_row "$sc" "$c" cycle
    SC_CYCLES[$sc]=$c
    if [[ $CYC_RESULT == FAIL ]]; then
      SC_FAILED[$sc]=$((SC_FAILED[$sc] + 1))
    fi
    for m in "${METRICS[@]}"; do
      SERIES["$sc|$m"]+="$(metric_now "$m") "
    done
    log "scenario $sc: cycle $c result=$CYC_RESULT ${CYC_FAIL}${CYC_INFO}"
  done

  local -a vals
  local trend_fail=0
  for m in "${METRICS[@]}"; do
    read -ra vals <<<"${SERIES["$sc|$m"]}" || true
    if is_trend_increase ${vals[@]+"${vals[@]}"}; then
      TREND["$sc|$m"]=FAIL
      trend_fail=1
    else
      TREND["$sc|$m"]=ok
    fi
  done
  if (( SC_FAILED[$sc] == 0 && trend_fail == 0 )); then
    SC_RESULT[$sc]=PASS
  else
    SC_RESULT[$sc]=FAIL
  fi
}

# series_max V...: prints the max of the values (baseline included by caller).
series_max() {
  printf '%s\n' "$@" | awk 'NR == 1 || $1 + 0 > m { m = $1 + 0 } END { print m + 0 }'
}

print_summary() {
  local sc m base final mx trend overall=PASS
  local -a vals
  for sc in "${SC_LIST[@]}"; do
    [[ ${SC_RESULT[$sc]:-FAIL} == PASS ]] || overall=FAIL
  done
  {
    printf 'leak-cycles summary: mode=%s cycles=%s tolerances threads=%s fds=%s mib=%s\n' \
      "$MODE" "$N" "$TOLERANCE_THREADS" "$TOLERANCE_FDS" "$TOLERANCE_MIB"
    printf 'session API: %s\n\n' "$([[ $API_OK == 1 ]] && echo available || echo unavailable)"
    printf '%-10s %-6s %-8s %s\n' SCENARIO RESULT CYCLES FAILED_CYCLES
    for sc in "${SC_LIST[@]}"; do
      printf '%-10s %-6s %-8s %s\n' "$sc" "${SC_RESULT[$sc]:-FAIL}" "${SC_CYCLES[$sc]:-0}" \
        "${SC_FAILED[$sc]:-0}"
      printf '  %-12s %-12s %-12s %-12s %s\n' metric baseline final max trend
      for m in "${METRICS[@]}"; do
        base=${BASEV[$m]:-n/a}
        read -ra vals <<<"${SERIES["$sc|$m"]:-}" || true
        if (( ${#vals[@]} > 0 )); then
          final=${vals[${#vals[@]} - 1]}
          mx=$(series_max "$base" "${vals[@]}")
        else
          final=$base
          mx=$base
        fi
        if (( ${#vals[@]} >= 5 )); then
          trend=${TREND["$sc|$m"]:-n/a}
        else
          trend="not-checked(<5)"
        fi
        printf '  %-12s %-12s %-12s %-12s %s\n' "$m" "$base" "$final" "$mx" "$trend"
      done
      printf '\n'
    done
    printf 'OVERALL %s\n' "$overall"
  } | tee "$OUT/summary.txt"
  [[ $overall == PASS ]]
}

# ---------------------------------------------------------------------------
# Entry points
# ---------------------------------------------------------------------------
dry_run() {
  local pid=$1
  [[ $pid =~ ^[1-9][0-9]*$ ]] || die "--dry-run needs a PID (argument or SUNSHINE_PID)"
  [[ -d /proc/$pid/task ]] || die "PID $pid is not a running process"
  PID=$pid
  measure_proc
  printf 'pid=%s\n' "$PID"
  printf 'threads=%s\n' "$M_threads"
  printf 'render_fds=%s\n' "$M_render"
  printf 'dmabuf_fds=%s\n' "$M_dmabuf"
  printf 'buf_14_16mib=%s\n' "$M_buf15"
  printf 'drm_fds=%s\n' "$M_drm_fds"
  printf 'drm_mem_mib=%s\n' "$M_drm_mem"
  printf 'drm_total_mib=%s\n' "$M_drm_total"
  printf 'drm_resident_mib=%s\n' "$M_drm_res"
}

main() {
  local sc old_umask

  DRY_RUN=0
  DRY_PID=""
  while (( $# > 0 )); do
    case $1 in
      --dry-run)
        DRY_RUN=1
        shift
        if [[ ${1:-} =~ ^[0-9]+$ ]]; then
          DRY_PID=$1
          shift
        fi
        ;;
      -h | --help)
        usage
        exit 0
        ;;
      *)
        die "unknown argument: $1"
        ;;
    esac
  done

  if (( DRY_RUN )); then
    dry_run "${DRY_PID:-$SUNSHINE_PID}"
    exit 0
  fi

  validate_config
  resolve_pid
  read -ra SC_LIST <<<"$SCENARIOS"

  mkdir -p -- "$OUT"
  CSV="$OUT/cycles.csv"

  old_umask=$(umask)
  umask 077
  dir=$(mktemp -d "${TMPDIR:-/tmp}/leak-cycles.XXXXXX")
  umask "$old_umask"
  trap 'rm -rf -- "${dir:?}"' EXIT

  RUN_START=$(date +%s)
  api_init
  printf 'scenario,cycle,phase,threads,sessions,render_fds,dmabuf_fds,drm_fds,drm_mem_mib,drm_total_mib,drm_resident_mib,buf15,settled,closed_via_api,session_gone_s,reconnected,log_reasons,result,notes\n' >"$CSV"

  for sc in "${SC_LIST[@]}"; do
    run_scenario "$sc"
  done

  print_summary
}

main "$@"
