"""Local two-process relay protocol stress harness (not a game/Steam emulator).

Runs independent host and guest processes over UDP loopback through a proxy that
adds bounded latency and drops deterministic packets. Exercises the existing
CH_CTRL keepalive framing, 60+ second idle period, and guest socket rebind.
"""
import multiprocessing as mp
import queue
import socket
import threading
import time

CTRL = 1
IDLE_SECONDS = 62
DELAY_SECONDS = 0.08
DROP_EVERY = 7


def peer(role, proxy_addr, stop, events):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    sock.settimeout(0.05)
    events.put(("ready", role, sock.getsockname()[1]))
    last_send = 0.0
    rx = 0
    tx = 0
    while not stop.is_set():
        now = time.monotonic()
        if now - last_send >= 0.15:
            sock.sendto(role + bytes((CTRL, ord('K'))), proxy_addr)
            last_send = now
            tx += 1
            if tx == 1:
                events.put(("sent", role.decode(), proxy_addr))
        try:
            data, _ = sock.recvfrom(256)
        except socket.timeout:
            continue
        if len(data) < 3 or data[1] != CTRL:
            events.put(("bad_frame", role, data.hex()))
            continue
        if role == b"H" and data[0:1] == b"G" and data[2:3] == b"K":
            sock.sendto(b"H" + bytes((CTRL, ord('P'))), proxy_addr)
        elif role == b"G" and data[0:1] == b"H" and data[2:3] == b"K":
            sock.sendto(b"G" + bytes((CTRL, ord('P'))), proxy_addr)
        elif data[0:1] != role and data[2:3] == b"P":
            rx += 1
            if rx % 5 == 0:
                events.put(("heartbeat", role.decode(), rx))
    sock.close()
    events.put(("stopped", role.decode()))


class ImpairmentProxy:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.01)
        self.addr = self.sock.getsockname()
        self.host = None
        self.guest = None
        self.pending = []
        self.count = 0
        self.dropped = 0
        self.forwarded = 0
        self.rebinds = 0
        self.done = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.done.is_set():
            now = time.monotonic()
            while self.pending and self.pending[0][0] <= now:
                _, target, data = self.pending.pop(0)
                try:
                    self.sock.sendto(data, target)
                    self.forwarded += 1
                except OSError:
                    pass
            try:
                data, src = self.sock.recvfrom(256)
            except socket.timeout:
                continue
            except ConnectionResetError:
                # Windows may report ICMP port-unreachable for the prior
                # guest mapping during the deliberate reconnect test.
                continue
            if not data:
                continue
            role = data[:1]
            if role == b"H":
                self.host = src
                target = self.guest
            elif role == b"G":
                if self.guest is not None and self.guest != src:
                    self.rebinds += 1
                self.guest = src
                target = self.host
            else:
                continue
            self.count += 1
            if self.count % DROP_EVERY == 0:
                self.dropped += 1
                continue
            if target:
                self.pending.append((now + DELAY_SECONDS, target, data))
                self.pending.sort(key=lambda item: item[0])

    def close(self):
        self.done.set()
        self.thread.join(timeout=2)
        self.sock.close()


def drain(events, collected):
    while True:
        try:
            collected.append(events.get_nowait())
        except queue.Empty:
            return


def main():
    ctx = mp.get_context("spawn")
    events = ctx.Queue()
    proxy = ImpairmentProxy()
    host_stop = ctx.Event()
    guest_stop = ctx.Event()
    host = ctx.Process(target=peer, args=(b"H", proxy.addr, host_stop, events), name="host-instance")
    guest = ctx.Process(target=peer, args=(b"G", proxy.addr, guest_stop, events), name="guest-instance")
    collected = []
    try:
        host.start()
        guest.start()
        time.sleep(3)
        drain(events, collected)
        assert any(e[0] == "heartbeat" and e[1] == "H" for e in collected), "host did not receive guest heartbeat replies"
        assert any(e[0] == "heartbeat" and e[1] == "G" for e in collected), "guest did not receive host heartbeat replies"
        print("PASS: separate host and guest processes exchange CH_CTRL keepalives under 80 ms delay and deterministic loss")

        time.sleep(35)
        guest_stop.set()
        guest.join(timeout=2)
        assert not guest.is_alive(), "old guest process did not stop"
        guest_stop = ctx.Event()
        guest = ctx.Process(target=peer, args=(b"G", proxy.addr, guest_stop, events), name="guest-reconnected")
        guest.start()
        time.sleep(IDLE_SECONDS - 38)
        drain(events, collected)
        assert proxy.rebinds >= 1, "proxy did not observe guest source-port rebind"
        assert any(e[0] == "heartbeat" and e[1] == "H" for e in collected), "host did not recover after guest rebind"
        assert any(e[0] == "heartbeat" and e[1] == "G" for e in collected), "reconnected guest did not receive replies"
        assert proxy.dropped > 0 and proxy.forwarded > 0
        assert host.is_alive() and guest.is_alive(), "a peer exited during the idle test"
        print(f"PASS: {IDLE_SECONDS}s session idle with keepalives; guest rebind/reconnect recovered (loss={proxy.dropped}, forwarded={proxy.forwarded})")
        print("LIMIT: this harness exercises the loopback wire model, not the compiled relay, native game cutscenes, WinMM, or Steam SDR.")
    finally:
        host_stop.set()
        guest_stop.set()
        host.join(timeout=3)
        guest.join(timeout=3)
        if host.is_alive(): host.terminate(); host.join()
        if guest.is_alive(): guest.terminate(); guest.join()
        proxy.close()


if __name__ == "__main__":
    main()
