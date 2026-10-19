#!/usr/bin/env python3
"""Drive the moonlight-web `streamer` binary directly over its stdio IPC.

This is the client side of the leak_cycles.sh hooks (CLIENT_START, CLIENT_STOP,
CLIENT_KILL, CLIENT_WEDGE, CLIENT_DROP, CLIENT_RECONNECT, CLIENT_CLEANUP). It
needs no web server and no browser: it speaks the same newline-delimited JSON
protocol as the streamer's own process tests (streamer/tests/process.rs).

Run it as `python3 -I tests/e2e/mlw_driver.py [global options] <command>`.
Standard library only. See tests/e2e/README.md for the hook recipes.

Commands:
  start    spawn a detached holder that runs the streamer and keeps it streaming
  stop     ask the holder to send "Stop" to the streamer; print its exit code
  kill     SIGKILL the streamer (then the holder if it lingers)
  freeze   SIGSTOP the streamer (dead network path: no ACKs, no pings)
  cleanup  SIGCONT + SIGKILL leftovers and remove the state files
  status   print a JSON summary

Exit status: 0 success, 1 error, 2 usage error, 3 `stop` timed out because the
streamer is still running (the expected result for a wedged streamer).

Secrets: the pair info (PEM key and certificates) is read from data.json in
memory only and handed to the streamer on its stdin. It is never printed,
never logged and never written to disk. Streamer stderr is filtered so PEM
blocks never reach the log. data.json is only read, never written.
"""

import argparse
import json
import os
import queue
import select
import signal
import socket
import sys
import threading
import time

PROG = "mlw_driver"
DEFAULT_WATCHDOG_MS = 8000  # common/src/ipc.rs DEFAULT_STOP_WATCHDOG_MS
H264_FORMAT_BIT = 0x0001  # moonlight VIDEO_FORMAT_H264 (see README: unconfirmed)
RTT_PREFIX = '{"WebSocketTransport":[26,0,'  # channel 26 = RTT, type 0

EX_OK = 0
EX_ERR = 1
EX_STILL_RUNNING = 3


class DriverError(Exception):
    """A user-facing error: the message is printed and the exit status is 1."""


# ---------------------------------------------------------------------------
# State directory and process bookkeeping
# ---------------------------------------------------------------------------


def default_state_dir():
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if runtime and os.path.isdir(runtime):
        return os.path.join(runtime, "mlw-driver")
    return "/tmp/mlw-driver-%d" % os.getuid()


class State:
    """Files in the state directory (all created 0600 in a 0700 directory)."""

    def __init__(self, path):
        self.dir = path
        self.holder_pid = os.path.join(path, "holder.pid")
        self.streamer_pid = os.path.join(path, "streamer.pid")
        self.streamer_path = os.path.join(path, "streamer.path")
        self.ready = os.path.join(path, "ready")
        self.exit_code = os.path.join(path, "exit_code")
        self.sock = os.path.join(path, "ctl.sock")
        self.log = os.path.join(path, "streamer.log")

    def ensure(self):
        try:
            os.mkdir(self.dir, 0o700)
        except FileExistsError:
            pass
        st = os.lstat(self.dir)
        if not os.path.isdir(self.dir) or os.path.islink(self.dir):
            raise DriverError("state dir is not a plain directory: %s" % self.dir)
        if st.st_uid != os.getuid():
            raise DriverError("state dir is not owned by this user: %s" % self.dir)
        if st.st_mode & 0o077:
            os.chmod(self.dir, 0o700)

    def exists(self):
        return os.path.isdir(self.dir)

    def write(self, path, text):
        tmp = path + ".tmp"
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(text)
        os.replace(tmp, path)

    def read(self, path):
        try:
            with open(path) as f:
                return f.read().strip()
        except OSError:
            return None

    def remove_runtime_files(self):
        for p in (self.holder_pid, self.streamer_pid, self.streamer_path,
                  self.ready, self.exit_code, self.sock):
            for q in (p, p + ".tmp"):
                try:
                    os.unlink(q)
                except OSError:
                    pass


def proc_stat(pid):
    """Return (state_char, starttime) from /proc/<pid>/stat, or None."""
    try:
        with open("/proc/%d/stat" % pid) as f:
            data = f.read()
    except OSError:
        return None
    rest = data[data.rfind(")") + 2:].split()
    try:
        return rest[0], rest[19]
    except IndexError:
        return None


def read_pidfile(path):
    """Return (pid, starttime) or None."""
    try:
        with open(path) as f:
            pid, start = f.read().split()
        return int(pid), start
    except (OSError, ValueError):
        return None


def write_pidfile(state, path, pid):
    st = proc_stat(pid)
    state.write(path, "%d %s\n" % (pid, st[1] if st else "0"))


def cmdline(pid):
    try:
        with open("/proc/%d/cmdline" % pid, "rb") as f:
            return [a.decode("utf-8", "replace") for a in f.read().split(b"\0") if a]
    except OSError:
        return []


def is_streamer(pid, target):
    """True when the process runs `target` (a binary, or a script via its interpreter)."""
    try:
        exe = os.readlink("/proc/%d/exe" % pid)
    except OSError:
        return False
    if exe.endswith(" (deleted)"):
        exe = exe[: -len(" (deleted)")]
    if os.path.realpath(exe) == target:
        return True
    return any(os.path.realpath(a) == target for a in cmdline(pid)[1:])


def is_holder(pid):
    return any(PROG in a for a in cmdline(pid))


def verified(rec, kind, target=None):
    """Is the recorded process alive, the very one we started, and ours?"""
    if not rec:
        return False
    pid, start = rec
    st = proc_stat(pid)
    if not st or st[0] in ("Z", "X") or st[1] != start:
        return False
    if kind == "streamer":
        return bool(target) and is_streamer(pid, target)
    return is_holder(pid)


def streamer_target(state, args):
    stored = state.read(state.streamer_path)
    if stored:
        return stored
    if getattr(args, "streamer", None):
        return os.path.realpath(args.streamer)
    return None


def signal_verified(rec, kind, target, sig):
    if verified(rec, kind, target):
        try:
            os.kill(rec[0], sig)
            return True
        except ProcessLookupError:
            return False
    return False


def wait_gone(rec, kind, target, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not verified(rec, kind, target):
            return True
        time.sleep(0.05)
    return not verified(rec, kind, target)


# ---------------------------------------------------------------------------
# data.json and the IPC messages
# ---------------------------------------------------------------------------


def pick_only(mapping, wanted, what):
    if wanted is not None:
        if wanted not in mapping:
            raise DriverError("%s id %r not found in data.json" % (what, wanted))
        return wanted
    if len(mapping) == 1:
        return next(iter(mapping))
    ids = ", ".join(sorted(mapping)) or "none"
    raise DriverError("data.json has %d %ss (ids: %s); pass --%s-id"
                      % (len(mapping), what, ids, what))


def load_identity(args):
    """Read pair info and client id from data.json. Returns a dict; never print it."""
    try:
        with open(args.data_json) as f:
            data = json.load(f)
    except (OSError, ValueError) as err:
        raise DriverError("cannot read data.json: %s" % type(err).__name__)
    if str(data.get("version")) not in ("2", "3"):
        raise DriverError("unsupported data.json version (need 2 or 3)")
    hosts = data.get("hosts") or {}
    host_id = pick_only(hosts, args.host_id, "host")
    host = hosts[host_id]
    pair = host.get("pair_info")
    if not pair:
        raise DriverError("host %s in data.json is not paired" % host_id)
    users = data.get("users") or {}
    client_id = None
    if users or args.user_id is not None:
        user_id = pick_only(users, args.user_id, "user")
        client_id = users[user_id].get("client_unique_id")
    try:
        return {
            "host_address": host["address"],
            "host_http_port": int(host["http_port"]),
            "client_unique_id": client_id,
            "client_private_key": pair["client_private_key"],
            "client_certificate": pair["client_certificate"],
            "server_certificate": pair["server_certificate"],
        }
    except KeyError as err:
        raise DriverError("data.json is missing field %s" % err)


def build_init_line(args, ident):
    """The `Init` line, same JSON shape as init_line() in streamer/tests/process.rs."""
    init = {
        "config": {
            "webrtc": {},  # WebRtcConfig::default(): every field has a serde default
            "log_level": "INFO",
            "stop_watchdog_ms": args.watchdog_ms,
        },
        "host_address": ident["host_address"],
        "host_http_port": ident["host_http_port"],
        "client_unique_id": ident["client_unique_id"],
        "client_private_key": ident["client_private_key"],
        "client_certificate": ident["client_certificate"],
        "server_certificate": ident["server_certificate"],
        "app_id": args.app_id,
        "video_frame_queue_size": 8,
        "audio_sample_queue_size": 8,
        "permissions": {
            "allow_add_hosts": False,
            "maximum_bitrate_kbps": None,
            "allow_codec_h264": True,
            "allow_codec_h265": True,
            "allow_codec_av1": True,
            "allow_hdr": True,
            "allow_transport_webrtc": True,
            "allow_transport_websockets": True,
        },
    }
    return json.dumps({"Init": init}, separators=(",", ":")) + "\n"


def set_transport_line():
    return json.dumps({"WebSocket": {"SetTransport": "WebSocket"}}, separators=(",", ":")) + "\n"


def start_stream_line(args):
    settings = {
        "bitrate_kbps": args.bitrate,
        "width": args.width,
        "height": args.height,
        "fps": args.fps,
        "play_audio_local": False,
        "supported_codecs": H264_FORMAT_BIT,
        "hdr": False,
    }
    return json.dumps({"WebSocket": {"StartStream": {"settings": settings}}},
                      separators=(",", ":")) + "\n"


STOP_LINE = '"Stop"\n'  # serde_json of ServerIpcMessage::Stop


# ---------------------------------------------------------------------------
# Holder (detached process that owns the streamer)
# ---------------------------------------------------------------------------


class PemFilter:
    """Drops PEM blocks (and any line mentioning one) from streamer stderr."""

    def __init__(self):
        self.inside = False

    def keep(self, line):
        if "-----BEGIN" in line or "BEGIN " in line and "-----" in line:
            self.inside = "-----END" not in line
            return False
        if self.inside:
            if "-----END" in line:
                self.inside = False
            return False
        return "-----END" not in line


def holder_main(args, state, init_line, target):
    """Runs in the detached grandchild. Never returns."""
    import subprocess

    def fail(msg):
        state.write(state.ready, "fail: %s\n" % msg)

    code = 0
    proc = None
    try:
        os.setsid()
        devnull = os.open(os.devnull, os.O_RDWR)
        for fd in (0, 1, 2):
            os.dup2(devnull, fd)
        os.closerange(3, 1024)
        write_pidfile(state, state.holder_pid, os.getpid())

        env = dict(os.environ)
        env.pop("MLW_TEST_FAULT", None)
        env.pop("MLW_TEST_FAULT_NO_WATCHDOG", None)
        if args.fault:
            env["MLW_TEST_FAULT"] = args.fault
        if args.no_watchdog:
            env["MLW_TEST_FAULT_NO_WATCHDOG"] = "1"

        proc = subprocess.Popen([args.streamer], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                env=env, close_fds=True, start_new_session=True)
        write_pidfile(state, state.streamer_pid, proc.pid)

        logfd = os.open(state.log, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        log_lock = threading.Lock()

        def log(text):
            with log_lock:
                os.write(logfd, (text.rstrip("\n") + "\n").encode("utf-8", "replace"))

        log("--- %s start (streamer pid %d)" % (PROG, proc.pid))

        def stderr_pump():
            filt = PemFilter()
            for raw in proc.stderr:
                line = raw.decode("utf-8", "replace")
                if filt.keep(line):
                    log(line)

        t_err = threading.Thread(target=stderr_pump, daemon=True)
        t_err.start()

        out_q = queue.Queue()
        write_q = queue.Queue()
        handshake = {"done": False}

        def stdin_pump():
            while True:
                line = write_q.get()
                try:
                    proc.stdin.write(line.encode())
                    proc.stdin.flush()
                except (OSError, ValueError):
                    return

        threading.Thread(target=stdin_pump, daemon=True).start()

        def stdout_pump():
            # Media is discarded. RTT packets are echoed back (the browser does
            # the same), so the transport sees a live peer.
            for raw in proc.stdout:
                if raw.startswith(RTT_PREFIX.encode()):
                    write_q.put(raw.decode("ascii", "replace").rstrip("\n") + "\n")
                    continue
                if handshake["done"] or not raw.startswith(b'{"WebSocket":'):
                    continue
                try:
                    out_q.put(json.loads(raw)["WebSocket"])
                except ValueError:
                    pass
            out_q.put(None)

        threading.Thread(target=stdout_pump, daemon=True).start()

        def wait_msg(name, deadline):
            while True:
                left = deadline - time.monotonic()
                if left <= 0:
                    return None, "timeout waiting for %s" % name
                try:
                    msg = out_q.get(timeout=left)
                except queue.Empty:
                    return None, "timeout waiting for %s" % name
                if msg is None:
                    return None, "streamer exited before %s" % name
                key = next(iter(msg)) if isinstance(msg, dict) else msg
                if key == name:
                    return msg, None
                if key == "DebugLog":
                    log("[ipc] debug: %s" % json.dumps(msg[key].get("message")))
                    if msg[key].get("ty") in ("fatal", "fatalDescription"):
                        return None, "streamer reported a fatal error (see log)"
                if key == "ConnectionTerminated":
                    return None, "connection terminated before %s" % name

        deadline = time.monotonic() + args.ready_timeout
        write_q.put(init_line)
        msg, err = wait_msg("Setup", deadline)
        if err:
            fail(err)
            raise SystemExit(1)
        write_q.put(set_transport_line())
        time.sleep(0.3)
        write_q.put(start_stream_line(args))
        msg, err = wait_msg("ConnectionComplete", deadline)
        if err:
            fail(err)
            raise SystemExit(1)
        handshake["done"] = True
        log("[ipc] connection complete")

        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(state.sock)
        os.chmod(state.sock, 0o600)
        srv.listen(4)
        state.write(state.ready, "ok\n")

        stopped_sent = False
        while proc.poll() is None:
            r, _, _ = select.select([srv], [], [], 0.2)
            if not r:
                continue
            conn, _ = srv.accept()
            with conn:
                conn.settimeout(2)
                try:
                    cmd = conn.recv(64).decode().strip()
                except OSError:
                    continue
                if cmd == "stop":
                    if not stopped_sent:
                        write_q.put(STOP_LINE)
                        stopped_sent = True
                        log("[ipc] Stop sent")
                    conn.sendall(b"ok\n")
                else:
                    conn.sendall(b"unknown\n")
        code = proc.returncode
    except SystemExit as exc:
        code = exc.code if isinstance(exc.code, int) else 1
    except BaseException as exc:  # the holder must never die silently
        fail("holder error: %s" % type(exc).__name__)
        code = 1
    finally:
        if proc is not None and proc.poll() is None and not state.read(state.ready) == "ok":
            try:
                proc.kill()
            except OSError:
                pass
        if proc is not None:
            try:
                proc.wait(timeout=5)
            except Exception:
                pass
            if proc.returncode is not None:
                state.write(state.exit_code, "%d\n" % proc.returncode)
        try:
            os.unlink(state.sock)
        except OSError:
            pass
    os._exit(code if code in range(0, 256) else 1)


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------


def holder_rec(state):
    return read_pidfile(state.holder_pid)


def streamer_rec(state):
    return read_pidfile(state.streamer_pid)


def is_active(state, target):
    return (verified(holder_rec(state), "holder")
            or verified(streamer_rec(state), "streamer", target))


def cmd_start(args, state):
    for opt in ("streamer", "data_json", "app_id"):
        if getattr(args, opt) is None:
            raise DriverError("start needs --%s" % opt.replace("_", "-"))
    args.streamer = os.path.realpath(args.streamer)
    if not (os.path.isfile(args.streamer) and os.access(args.streamer, os.X_OK)):
        raise DriverError("--streamer is not an executable file")
    state.ensure()
    if is_active(state, state.read(state.streamer_path) or args.streamer):
        raise DriverError("a stream is already active in %s (run cleanup first)" % state.dir)
    state.remove_runtime_files()

    ident = load_identity(args)
    init_line = build_init_line(args, ident)
    state.write(state.streamer_path, args.streamer + "\n")

    sys.stdout.flush()
    sys.stderr.flush()
    pid = os.fork()
    if pid == 0:
        try:
            if os.fork() != 0:
                os._exit(0)
            holder_main(args, state, init_line, args.streamer)
        finally:
            os._exit(1)
    os.waitpid(pid, 0)
    del init_line, ident

    deadline = time.monotonic() + args.ready_timeout + 5
    while time.monotonic() < deadline:
        ready = state.read(state.ready)
        if ready == "ok":
            srec = streamer_rec(state)
            print("started streamer_pid=%s holder_pid=%s"
                  % (srec[0] if srec else "?", (holder_rec(state) or ("?",))[0]))
            return EX_OK
        if ready:
            _abort_start(state, args)
            raise DriverError("start failed: %s (see %s)" % (ready, state.log))
        time.sleep(0.1)
    _abort_start(state, args)
    raise DriverError("start timed out after %ds (see %s)" % (args.ready_timeout, state.log))


def _abort_start(state, args):
    target = args.streamer
    signal_verified(streamer_rec(state), "streamer", target, signal.SIGKILL)
    signal_verified(holder_rec(state), "holder", None, signal.SIGKILL)


def read_exit_code(state):
    value = state.read(state.exit_code)
    try:
        return int(value) if value is not None else None
    except ValueError:
        return None


def cmd_stop(args, state):
    target = streamer_target(state, args)
    holder = holder_rec(state)
    srec = streamer_rec(state)
    if not verified(srec, "streamer", target):
        code = read_exit_code(state)
        if code is None:
            raise DriverError("no active stream in %s" % state.dir)
        print("exit_code=%d (already exited)" % code)
        return EX_OK
    if not verified(holder, "holder"):
        raise DriverError("streamer is running but its holder is gone; use kill or cleanup")
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(5)
            s.connect(state.sock)
            s.sendall(b"stop\n")
            s.recv(16)
    except OSError as err:
        raise DriverError("cannot reach the holder: %s" % type(err).__name__)
    if wait_gone(srec, "streamer", target, args.stop_timeout):
        # the holder records the exit code right after reaping the child
        for _ in range(40):
            code = read_exit_code(state)
            if code is not None:
                break
            time.sleep(0.05)
        print("exit_code=%s" % (code if code is not None else "unknown"))
        return EX_OK
    print("still running after %ss" % args.stop_timeout)
    return EX_STILL_RUNNING


def cmd_kill(args, state):
    target = streamer_target(state, args)
    srec = streamer_rec(state)
    holder = holder_rec(state)
    if not signal_verified(srec, "streamer", target, signal.SIGCONT):
        print("no running streamer")
    signal_verified(srec, "streamer", target, signal.SIGKILL)
    if not wait_gone(holder, "holder", None, 3):
        signal_verified(holder, "holder", None, signal.SIGKILL)
    wait_gone(srec, "streamer", target, 3)
    print("killed")
    return EX_OK


def cmd_freeze(args, state):
    target = streamer_target(state, args)
    if not signal_verified(streamer_rec(state), "streamer", target, signal.SIGSTOP):
        raise DriverError("no running streamer to freeze")
    print("frozen")
    return EX_OK


def cmd_cleanup(args, state):
    if not state.exists():
        return EX_OK
    target = streamer_target(state, args)
    srec = streamer_rec(state)
    holder = holder_rec(state)
    signal_verified(srec, "streamer", target, signal.SIGCONT)
    signal_verified(srec, "streamer", target, signal.SIGKILL)
    signal_verified(holder, "holder", None, signal.SIGKILL)
    wait_gone(srec, "streamer", target, 3)
    wait_gone(holder, "holder", None, 3)
    state.remove_runtime_files()
    if args.purge:
        try:
            os.unlink(state.log)
        except OSError:
            pass
        try:
            os.rmdir(state.dir)
        except OSError:
            pass
    return EX_OK


def cmd_status(args, state):
    target = streamer_target(state, args)
    srec = streamer_rec(state)
    holder = holder_rec(state)
    st = proc_stat(srec[0]) if srec else None
    out = {
        "state_dir": state.dir,
        "holder": {"pid": holder[0] if holder else None,
                   "alive": verified(holder, "holder")},
        "streamer": {"pid": srec[0] if srec else None,
                     "alive": verified(srec, "streamer", target),
                     "stopped": bool(st and st[0] == "T")},
        "ready": state.read(state.ready),
        "exit_code": read_exit_code(state),
    }
    print(json.dumps(out))
    return EX_OK


COMMANDS = {
    "start": cmd_start,
    "stop": cmd_stop,
    "kill": cmd_kill,
    "freeze": cmd_freeze,
    "cleanup": cmd_cleanup,
    "status": cmd_status,
}


def parse_args(argv):
    p = argparse.ArgumentParser(
        prog=PROG, description="Drive the moonlight-web streamer over stdio IPC "
        "for leak_cycles.sh client hooks.")
    p.add_argument("--state-dir", default=default_state_dir(),
                   help="state directory, created 0700 (default: $XDG_RUNTIME_DIR/mlw-driver "
                   "or /tmp/mlw-driver-$UID)")
    p.add_argument("--streamer", help="path to the streamer binary (start)")
    p.add_argument("--data-json", help="moonlight-web data.json, read only (start)")
    p.add_argument("--host-id", help="key in data.json hosts (default: the only host)")
    p.add_argument("--user-id", help="key in data.json users for client_unique_id "
                   "(default: the only user)")
    p.add_argument("--app-id", type=int, help="Sunshine app id (required for start)")
    p.add_argument("--width", type=int, default=1280)
    p.add_argument("--height", type=int, default=720)
    p.add_argument("--fps", type=int, default=30)
    p.add_argument("--bitrate", type=int, default=5000, help="kbps")
    p.add_argument("--fault", help="set MLW_TEST_FAULT for the streamer, e.g. wedge-audio-stop "
                   "(needs a streamer built with the test-faults feature)")
    p.add_argument("--no-watchdog", action="store_true",
                   help="set MLW_TEST_FAULT_NO_WATCHDOG=1 (the stop watchdog never fires)")
    p.add_argument("--watchdog-ms", type=int, default=DEFAULT_WATCHDOG_MS,
                   help="stop_watchdog_ms sent in Init (default %(default)s)")
    p.add_argument("--ready-timeout", type=int, default=30,
                   help="seconds until ConnectionComplete (start)")
    p.add_argument("--stop-timeout", type=int, default=15,
                   help="seconds to wait for the streamer to exit (stop)")
    p.add_argument("--purge", action="store_true",
                   help="cleanup: also delete streamer.log and the state directory")
    p.add_argument("command", choices=sorted(COMMANDS))
    return p.parse_args(argv)


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    state = State(os.path.abspath(args.state_dir))
    try:
        if args.command != "cleanup":
            state.ensure()
        return COMMANDS[args.command](args, state)
    except DriverError as err:
        print("%s: %s" % (PROG, err), file=sys.stderr)
        return EX_ERR


if __name__ == "__main__":
    sys.exit(main())
