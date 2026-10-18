# End-to-end leak-cycle check

`leak_cycles.sh` runs repeated stream start and stop cycles against a running
Sunshine and checks for session and capture leaks: threads, render-node and
dmabuf fds, DRM memory, ~15 MiB dmabuf buffers, and the active-session count.
It runs on the test host, where Sunshine is a systemd user unit and a streaming
client runs next to it. It works against both unpatched and patched builds. The
session API and the "Session ended" log line are optional, and the script
reports them as `n/a` when they are missing.

## Requirements

- bash 4.4 or newer, coreutils, awk, sed, curl, `timeout`
- `systemctl --user` (only when `SUNSHINE_PID` is not set)
- `journalctl --user` or a `SUNSHINE_LOG_CMD` that prints the Sunshine log
- `jq` (optional; a sed fallback is used without it)
- Read access to `/proc/<pid>/{task,fd,fdinfo}` for the Sunshine PID

## Usage

```sh
# one measurement pass on any PID, prints metrics, no side effects
tests/e2e/leak_cycles.sh --dry-run 12345

# full run
SUNSHINE_API_USER=... SUNSHINE_API_PASS=... \
CLIENT_START='...' CLIENT_STOP='...' CLIENT_KILL='...' CLIENT_WEDGE='...' \
CLIENT_DROP='...' CLIENT_RECONNECT='...' CLIENT_CLEANUP='...' \
tests/e2e/leak_cycles.sh
```

Output goes to `$OUT` (default `./leak-cycles-<date>`):

- `cycles.csv`: one `baseline` row per scenario, then one row per cycle.
- `summary.txt`: per scenario PASS or FAIL, cycle counts, and baseline, final
  and max for each metric, plus the trend verdict. The same text goes to stdout.
- `hooks.log`: stdout and stderr of the client hooks.

Exit status is 0 only when every scenario passes. It is 1 when a scenario fails
and 2 on configuration or setup errors.

## Environment

| Variable | Default | Meaning |
| --- | --- | --- |
| `SUNSHINE_UNIT` | `sunshine-lanternbox.service` | User unit, used to resolve the PID |
| `SUNSHINE_PID` | (unset) | Use this PID instead of resolving the unit |
| `SUNSHINE_LOG_CMD` | `journalctl --user -u $SUNSHINE_UNIT -o cat --since=@SINCE@` | Command that prints Sunshine log lines since an epoch. `@SINCE@` is replaced with the epoch |
| `SUNSHINE_API` | `https://localhost:47990` | Base URL of the session API |
| `SUNSHINE_API_USER`, `SUNSHINE_API_PASS` | (unset) | Required for API checks. The password is passed to curl through a config file in a `umask 077` temp dir and is never printed |
| `CLIENT_START` | | Starts one low-res stream and returns when streaming |
| `CLIENT_STOP` | | Clean stop of the client |
| `CLIENT_KILL` | | SIGKILL the client |
| `CLIENT_WEDGE` | | Stops the client so it wedges in its audio stop path (pings continue, no media is read) |
| `CLIENT_DROP` | | Drops the network or connection |
| `CLIENT_RECONNECT` | | Reconnects the client |
| `CLIENT_CLEANUP` | | Kills leftover clients (optional) |
| `N` | `20` | Cycles per scenario |
| `SCENARIOS` | `clean kill wedge drop` | Space separated list of `clean`, `kill`, `wedge`, `drop` |
| `STREAM_SECONDS` | `10` | Streaming time before each end action |
| `SETTLE_TIMEOUT` | `30` | Seconds to wait for a cycle to settle |
| `PING_TIMEOUT` | `10` | Sunshine ping timeout, used for the wedge and drop waits |
| `WATCHDOG_DEADLINE` | `0` | Client-side stop watchdog deadline (full-stack wedge) |
| `MODE` | `sunshine-only` | `sunshine-only` or `full-stack` |
| `TOLERANCE_THREADS` | `2` | Allowed thread-count difference from baseline |
| `TOLERANCE_FDS` | `2` | Allowed render-node and dmabuf fd difference; also used for the 15 MiB buffer count |
| `TOLERANCE_MIB` | `64` | Allowed DRM memory difference, in MiB |
| `HOOK_TIMEOUT` | `120` | Seconds before a single hook is killed |
| `OUT` | `./leak-cycles-<date>` | Output directory |

Hooks are run with `bash -c`. The script refuses to start when a hook needed by
the selected scenarios is unset.

## Measurements

Taken at baseline (before the first cycle of each scenario, with no session) and
after each cycle once settled.

- threads: entries in `/proc/<pid>/task`
- active sessions: from the newest `Session ended: reason=<r>, active sessions=<N>`
  log line since the run started; else the length of `GET /api/sessions`; else `n/a`
- render-node fds: `/proc/<pid>/fd` links to `/dev/dri/renderD*`
- dmabuf fds: links starting with `/dmabuf:` or `anon_inode:dmabuf`
- DRM fds: fdinfo files containing `drm-driver:`
- DRM memory: sums of `drm-memory-*`, `drm-total-*` and `drm-resident-*` in MiB,
  deduplicated per `drm-client-id` (the largest value per client and key)
- 15 MiB buffers: dmabuf fdinfo entries whose `size:` is between 14 and 16 MiB
- Sunshine log reasons seen in the cycle

"Settled" means: sessions are 0 or `n/a`, and the thread count is within
`TOLERANCE_THREADS` of baseline. The script polls every second up to
`SETTLE_TIMEOUT`.

## Scenarios

Each cycle: start, stream `STREAM_SECONDS`, end action, settle, measure.

- `clean`: `CLIENT_STOP`.
- `kill`: `CLIENT_KILL`.
- `wedge`: `CLIENT_WEDGE`.
  - `sunshine-only`: wait `PING_TIMEOUT + 5` s and record whether the session is
    still listed. If the API exists, close it through `POST /api/sessions/<id>/close`
    (with a CSRF token) and record the time until it is gone. If there is no API,
    the cycle records `stuck-session-no-api` and fails. `CLIENT_CLEANUP` runs
    afterwards.
  - `full-stack`: the session must end by itself within
    `PING_TIMEOUT + WATCHDOG_DEADLINE + 5` s of the wedge, detected from the log
    or the API. No API close is used.
- `drop`: `CLIENT_DROP`, wait `PING_TIMEOUT + 5` s, `CLIENT_RECONNECT`, stream
  briefly (at most 5 s), check for a new session, then `CLIENT_STOP`.

## Pass and fail

A cycle fails when any of these hold:

- it does not settle (sessions not idle, or threads outside `TOLERANCE_THREADS`);
- render-node fds or dmabuf fds differ from baseline by more than `TOLERANCE_FDS`;
- the 15 MiB buffer count differs from baseline by more than `TOLERANCE_FDS`;
- DRM memory, total or resident differs from baseline by more than `TOLERANCE_MIB`;
- a hook returns an error;
- a wedge in `sunshine-only` mode was not closed through the API, or a wedge in
  `full-stack` mode did not end within its deadline.

A scenario fails when any cycle fails, or when a trend check fails. The trend
check fails when a metric (threads, render fds, dmabuf fds, 15 MiB count, DRM
memory total, resident and memory) is non-decreasing over at least 5 cycles and
its last value is greater than its first. This catches slow leaks that stay
inside the tolerance.

## Interpretation notes

- The spec default API address is the IPv4 loopback address. This script uses `localhost`
  instead, because the repository rules forbid IP addresses in scripts. Set
  `SUNSHINE_API` to change it.
- `TOLERANCE_MIB` applies to DRM memory. The 15 MiB buffer count is a count, not a
  size, so it uses `TOLERANCE_FDS`. Set `TOLERANCE_FDS` if a different allowance is
  wanted.
- A dmabuf fdinfo `size:` is read as bytes. A DRM `drm-memory-*` value with no
  unit is read as bytes, and KiB and MiB as units when present.
- The trend check covers the cycle values only, not the baseline, and needs at
  least 5 cycles.
- Without an API, `active sessions` comes from the log alone. A session that
  never ends has no log line, so the log can still show an earlier `0`. In that
  case the script can report a settled cycle while a session is stuck. The
  `stuck-session-no-api` failure covers the wedge case.
- Hook output goes to `hooks.log`, not to stdout, so client diagnostics do not
  mix with the summary.
- The `drop` scenario treats a missing new session after reconnect as a note,
  not a failure. The spec gives no pass rule for it.

## Privacy

The script reads only `/proc/<pid>/{task,fd,fdinfo}` for the Sunshine PID, plus
the Sunshine unit's log lines and the API. It never lists other processes and
never reads user files. The API password goes to curl only through a temp config
file, which is removed on exit.
