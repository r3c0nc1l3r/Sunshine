# Paired private-seat capture test

This opt-in test requires a newly provisioned, synthetic LanternBox fixture. It
exercises CPU/RAM wlr capture and the actual private-seat READY/ACTIVE handoff;
it then sends raw Sunshine keyboard and mouse packets and observes the synthetic
Wayland client, including held-input releases during teardown. It does not encode
video or establish HEVC, pairing, full-video, or host-desktop acceptance. The test
never fabricates capture proof or READY.

The lead provisions a separate compositor process as UID 1002 with one normal,
full 2560x1440@60 output named `LANTERN-1`, `WAYLAND_DISPLAY=qa-private`,
private-seat peer UID 0,
and private socket `<runtime>/private-seat/seat.sock`. Both sockets must be
owned by UID 1002 and live in the same canonical, isolated runtime directory
strictly below the test root; use no host socket. Give the synthetic root test process
access to `/dev/dri/renderD128`. No kernel input device is part of this fixture.
The explicit output selector is `LANTERN-1`; the test requires it to be the
sole advertised output. Run only this filter in a fresh process.
The fixture owner must ensure no personal desktop or live producer is using it:
private-seat connections can replace the existing producer.

Build `test_sunshine` with kbuild, at most eight jobs, and keep build, cache,
runtime, and temporary paths under the test root. Set
`SUNSHINE_PRIVATE_SEAT_TEST_ROOT` to an absolute, existing canonical directory
owned by the effective test UID and not writable by group or others. It must
not be a symlink, filesystem root, an account home directory, `/home`, or under
`/tmp` or `/dev/shm`. `TMPDIR` must resolve to an existing directory within
that root. Missing or invalid settings fail with a generic diagnostic in each
private-seat fixture using scratch space. The capture test runs as UID 0, so
the root must be owned by UID 0. Supply the directory containing the two extracted OpenSSL runtime
libraries as `OPENSSL_RUNTIME_LIB_DIR`. With absolute paths to existing fixture
and test binary, the native execution contract is:

```sh
TEST_ROOT='<canonical scratch root>'
RUNTIME='<canonical scratch root/runtime>'
SCRATCH='<absolute scratch root/scratch>'
TEST_BINARY='<absolute scratch root/tests/test_sunshine>'
OPENSSL_RUNTIME_LIB_DIR='<absolute scratch root/openssl-libs>'
env -u DISPLAY -u WAYLAND_SOCKET \
  SUNSHINE_PRIVATE_SEAT_CAPTURE_TEST=1 SYNTHETIC_ONLY=1 \
  SUNSHINE_PRIVATE_SEAT_TEST_ROOT="$TEST_ROOT" \
  SUNSHINE_PRIVATE_SEAT_EVENT_LOG="$TEST_ROOT/client-events.log" \
  XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY=qa-private TMPDIR="$SCRATCH" \
  LD_LIBRARY_PATH="$OPENSSL_RUNTIME_LIB_DIR" \
  setsid timeout --signal=TERM --kill-after=2s 60s \
  "$TEST_BINARY" --gtest_filter=PrivateSeatCapture.SyntheticOutput
```

Launch as UID 0 only in the synthetic guest. The `setsid`/`timeout` wrapper
bounds the test process group to at most 62 seconds, including a stuck capture
or teardown. A timeout, skip, capture failure, or absent ACTIVE is not a pass.
The gtest stops capture via callback, then requests and checks the input cleanup
barrier. Its publication callback only assigns an in-memory image slot. It also
observes actual numeric client events for keys, relative and absolute motion,
buttons and both scroll directions. Start the fullscreen synthetic client before
the test and keep it dispatching through cleanup. Its event log must be a fresh,
canonical regular file under the test root, owned by the compositor or producer
UID, with one link and unchanged inode. Flush each numeric event immediately;
put diagnostics elsewhere. The initial pointer must be away from (640,360).
No host input or replayed events may contribute to this log.
