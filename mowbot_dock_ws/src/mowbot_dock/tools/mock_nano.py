#!/usr/bin/env python3
"""Mock dock Nano for testing dock_agent_node without hardware.

Creates a pty, symlinks it at --link, and speaks the dock serial protocol
(02_ARDUINO_FIRMWARE.md): 10 Hz 8-field status frames out, 4-field command
frames in, EVT: lines for boot/self-test.

Scenario is time-driven so it runs unattended:
  robot seats after --seat-after, charges for --charge-seconds (current decays
  2.0 -> 0.4 A), drains, then COMPLETE. Watchdog fault 4 is reported (and
  charging continues) if command frames stop for >2 s mid-charge.

This is a behavioral approximation for integration tests, not a firmware
reference: timings are compressed and chargeEnable=0 simply parks the state
machine in SEATED.

Usage:
  mock_nano.py --link /tmp/mock_nano_pty --seat-after 2 --charge-seconds 6
  dock_agent_node --ros-args -p serial_device:=/tmp/mock_nano_pty
"""
import argparse
import os
import pty
import select
import sys
import time

IDLE, SEATED, RAMP, CHARGING, DRAIN, COMPLETE, SELFTEST, FAULT = range(8)

FRAME_PERIOD = 0.1
WATCHDOG_TIMEOUT = 2.0


class MockNano:
    def __init__(self, args):
        self.args = args
        self.state = IDLE
        self.state_since = time.monotonic()
        self.boot_time = time.monotonic()
        self.fault = 0
        self.charge_enable = True
        self.prev_self_test = False
        self.last_cmd_time = None
        self.self_test_pending = False

    def uptime(self):
        return int(time.monotonic() - self.boot_time)

    def since(self):
        return time.monotonic() - self.state_since

    def go(self, state):
        self.state = state
        self.state_since = time.monotonic()

    def seated(self):
        t = time.monotonic() - self.boot_time
        if t < self.args.seat_after:
            return False
        if self.args.unseat_after > 0 and t > self.args.unseat_after:
            return False
        return True

    def handle_command(self, line):
        parts = line.strip().split(",")
        if len(parts) != 4:
            return
        try:
            charge_enable, clear_fault, _led, self_test = (int(p) for p in parts)
        except ValueError:
            return
        self.last_cmd_time = time.monotonic()
        self.charge_enable = charge_enable == 1
        if self.fault == 4:  # watchdog fault clears when the Pi comes back
            self.fault = 0
        if clear_fault == 1 and self.state == FAULT:
            self.fault = 0
            self.go(IDLE)
        if self_test == 1 and not self.prev_self_test and self.state in (IDLE, COMPLETE):
            self.self_test_pending = True
        self.prev_self_test = self_test == 1

    def step(self):
        """Advance the state machine; returns a list of extra EVT lines."""
        events = []
        seated = self.seated()

        if self.self_test_pending and self.state in (IDLE, COMPLETE):
            self.self_test_pending = False
            self.go(SELFTEST)

        if self.state == IDLE and seated:
            self.go(SEATED)
        elif self.state == SEATED:
            if not seated:
                self.go(IDLE)
            elif self.charge_enable and self.since() > 0.5:
                self.go(RAMP)
        elif self.state == RAMP:
            if not seated:
                events.append("EVT:EMERGENCY:SWITCH")
                self.go(IDLE)
            elif self.since() > 0.5:
                self.go(CHARGING)
        elif self.state == CHARGING:
            if not seated:
                events.append("EVT:EMERGENCY:SWITCH")
                self.go(IDLE)
            elif not self.charge_enable:
                self.go(SEATED)
            elif self.since() > self.args.charge_seconds:
                self.go(DRAIN)
            elif (self.last_cmd_time is None or
                  time.monotonic() - self.last_cmd_time > WATCHDOG_TIMEOUT):
                self.fault = 4  # report-only; charge continues
        elif self.state == DRAIN:
            if self.since() > 1.5:
                self.go(COMPLETE)
        elif self.state == COMPLETE:
            if not seated:
                self.go(IDLE)
        elif self.state == SELFTEST:
            if self.since() > 1.5:
                events.append("EVT:SELFTEST:OK")
                self.go(COMPLETE if seated else IDLE)

        return events

    def telemetry(self):
        k1 = 1 if self.state in (SEATED, RAMP, CHARGING, DRAIN) else 0
        k2 = 1 if self.state in (RAMP, CHARGING, SELFTEST) else 0
        if self.state == CHARGING:
            progress = min(self.since() / max(self.args.charge_seconds, 0.1), 1.0)
            current = 2.0 - 1.6 * progress
            voltage = 41.8
        elif self.state == RAMP:
            current = 0.5
            voltage = 40.0
        elif self.state == DRAIN:
            current = 0.05
            voltage = 36.0
        elif self.state == SELFTEST:
            current = 0.0
            voltage = 41.0
        else:
            current = 0.0
            voltage = 0.1
        return (f"{self.state},{1 if self.seated() else 0},{k1},{k2},"
                f"{current:.2f},{voltage:.1f},{self.fault},{self.uptime()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--link", default="/tmp/mock_nano_pty",
                        help="symlink to create for the pty slave")
    parser.add_argument("--seat-after", type=float, default=3.0,
                        help="seconds until the robot seats")
    parser.add_argument("--unseat-after", type=float, default=0.0,
                        help="seconds until the robot leaves (0 = never)")
    parser.add_argument("--charge-seconds", type=float, default=10.0,
                        help="length of the CHARGING phase")
    parser.add_argument("--run-seconds", type=float, default=0.0,
                        help="exit after this long (0 = run forever)")
    args = parser.parse_args()

    master_fd, slave_fd = pty.openpty()
    slave_name = os.ttyname(slave_fd)
    if os.path.lexists(args.link):
        os.unlink(args.link)
    os.symlink(slave_name, args.link)
    print(f"mock_nano: pty at {slave_name} (link: {args.link})", flush=True)

    nano = MockNano(args)
    os.write(master_fd, b"EVT:BOOT:mock-0.1\n")

    rx = b""
    next_frame = time.monotonic()
    try:
        while args.run_seconds <= 0 or time.monotonic() - nano.boot_time < args.run_seconds:
            timeout = max(next_frame - time.monotonic(), 0)
            readable, _, _ = select.select([master_fd], [], [], timeout)
            if master_fd in readable:
                try:
                    rx += os.read(master_fd, 256)
                except OSError:
                    break  # peer closed
                while b"\n" in rx:
                    line, rx = rx.split(b"\n", 1)
                    nano.handle_command(line.decode(errors="replace"))
            if time.monotonic() >= next_frame:
                next_frame += FRAME_PERIOD
                for event in nano.step():
                    os.write(master_fd, event.encode() + b"\n")
                os.write(master_fd, nano.telemetry().encode() + b"\n")
    except KeyboardInterrupt:
        pass
    finally:
        if os.path.lexists(args.link):
            os.unlink(args.link)
    print(f"mock_nano: exiting in state {nano.state}", file=sys.stderr)


if __name__ == "__main__":
    main()
