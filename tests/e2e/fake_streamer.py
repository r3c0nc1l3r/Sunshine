#!/usr/bin/env python3
"""Fake moonlight-web streamer for testing mlw_driver.py (stdlib only).

Speaks the streamer's stdio IPC: reads Init, sends Setup, expects SetTransport
and StartStream, sends ConnectionComplete, then sends one RTT packet and logs
whether the echo came back. Diagnostics go to stderr, which the driver keeps in
its streamer.log. It also prints a fake PEM block on stderr so the log filter
can be checked.

Environment:
  FAKE_WEDGE=1  ignore "Stop" and EOF (a wedged streamer, --no-watchdog)
  FAKE_WEDGE=2  on "Stop" exit with code 2 after one second (watchdog)
"""

import json
import os
import sys
import time


def say(text):
    sys.stderr.write("fake: %s\n" % text)
    sys.stderr.flush()


def send(obj):
    sys.stdout.write(json.dumps(obj, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def main():
    wedge = os.environ.get("FAKE_WEDGE", "")
    say("fault=%s no_watchdog=%s" % (os.environ.get("MLW_TEST_FAULT", ""),
                                     os.environ.get("MLW_TEST_FAULT_NO_WATCHDOG", "")))
    first = sys.stdin.readline()
    if not first:
        return 0
    init = json.loads(first)["Init"]
    for key in ("config", "host_address", "host_http_port", "client_unique_id",
                "client_private_key", "client_certificate", "server_certificate",
                "app_id", "video_frame_queue_size", "audio_sample_queue_size",
                "permissions"):
        if key not in init:
            say("INIT MISSING %s" % key)
            return 10
    say("init ok app_id=%s client_id=%s wd=%s" % (
        init["app_id"], init["client_unique_id"], init["config"]["stop_watchdog_ms"]))
    sys.stderr.write("-----BEGIN FAKE PRIVATE KEY-----\nQUJDREVGRw==\n-----END FAKE PRIVATE KEY-----\n")
    sys.stderr.flush()
    send({"WebSocket": {"Setup": {"ice_servers": []}}})

    got_transport = False
    sent_rtt = False
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        msg = json.loads(line)
        if msg == "Stop":
            say("got Stop")
            if wedge == "1":
                continue
            if wedge == "2":
                time.sleep(1)
                return 2
            return 0
        if isinstance(msg, dict) and "WebSocket" in msg:
            inner = msg["WebSocket"]
            if inner == {"SetTransport": "WebSocket"}:
                got_transport = True
                say("got SetTransport")
            elif isinstance(inner, dict) and "StartStream" in inner:
                s = inner["StartStream"]["settings"]
                say("got StartStream %sx%s@%s %skbps codecs=%s" % (
                    s["width"], s["height"], s["fps"], s["bitrate_kbps"], s["supported_codecs"]))
                if not got_transport:
                    say("StartStream BEFORE SetTransport")
                    return 11
                send({"WebSocket": {"ConnectionComplete": {
                    "capabilities": {"touch": False}, "format": 1, "width": s["width"],
                    "height": s["height"], "fps": s["fps"], "audio_sample_rate": 48000,
                    "audio_channel_count": 2, "audio_streams": 1,
                    "audio_coupled_streams": 1, "audio_samples_per_frame": 240,
                    "audio_mapping": [0, 1, 0, 0, 0, 0, 0, 0]}}})
                if not sent_rtt:
                    sent_rtt = True
                    send({"WebSocketTransport": [26, 0, 0, 7]})
        elif isinstance(msg, dict) and "WebSocketTransport" in msg:
            if msg["WebSocketTransport"] == [26, 0, 0, 7]:
                say("got rtt echo")
    say("stdin EOF")
    return 0 if wedge != "1" else time.sleep(3600) or 0


if __name__ == "__main__":
    sys.exit(main())
