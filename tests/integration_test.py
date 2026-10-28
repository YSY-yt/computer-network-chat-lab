#!/usr/bin/env python3
"""End-to-end test for the C chat client/server.

The test starts the real server and three real client processes, exchanges
Chinese and English messages, verifies broadcast delivery, and checks that
each client's server->client sequence numbers are contiguous.
"""

from __future__ import annotations

import os
import re
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SERVER = ROOT / "bin" / "chat_server"
CLIENT = ROOT / "bin" / "chat_client"
OUT = ROOT / "build" / "integration"
PORT = os.environ.get("CHAT_TEST_PORT", "19001")
SEQ_RE = re.compile(r"\] #(\d+) ")
DELAY = float(os.environ.get("CHAT_TEST_DELAY", "0"))


def pause() -> None:
    if DELAY > 0:
        time.sleep(DELAY)


def wait_until(predicate, timeout: float, what: str) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError(f"timeout waiting for {what}")


def wait_text(path: Path, needle: str, timeout: float = 5.0) -> None:
    wait_until(
        lambda: path.exists() and needle in path.read_text(encoding="utf-8"),
        timeout,
        f"{needle!r} in {path.name}",
    )


def start_client(nickname: str, log_dir: Path):
    path = log_dir / f"{nickname}.log"
    stream = path.open("w", encoding="utf-8")
    process = subprocess.Popen(
        [str(CLIENT), "-s", "127.0.0.1", PORT, nickname],
        stdin=subprocess.PIPE,
        stdout=stream,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        bufsize=1,
    )
    return process, stream, path


def send(process: subprocess.Popen, text: str) -> None:
    assert process.stdin is not None
    process.stdin.write(text + "\n")
    process.stdin.flush()


def stop_client(process: subprocess.Popen, stream) -> None:
    if process.poll() is None:
        try:
            send(process, "/quit")
        except (BrokenPipeError, ValueError):
            pass
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)
    stream.close()


def check_sequence(path: Path) -> int:
    text = path.read_text(encoding="utf-8")
    assert "序号不连续" not in text, f"sequence gap reported in {path.name}"
    seqs = [int(value) for line in text.splitlines()
            for value in SEQ_RE.findall(line)]
    assert seqs, f"no sequenced frames in {path.name}"
    assert seqs == list(range(1, len(seqs) + 1)), (
        f"non-contiguous sequence in {path.name}: {seqs}"
    )
    return len(seqs)


def main() -> int:
    if not SERVER.exists() or not CLIENT.exists():
        print("binaries missing; run make first", file=sys.stderr)
        return 2

    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.log"):
        old.unlink()

    server_log = (OUT / "server.log").open("w", encoding="utf-8")
    server = subprocess.Popen(
        [str(SERVER), PORT],
        stdout=server_log,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
    )

    clients: dict[str, tuple[subprocess.Popen, object, Path]] = {}
    try:
        wait_until(
            lambda: _port_is_open(PORT),
            5.0,
            f"server on port {PORT}",
        )

        alice = start_client("Alice", OUT)
        clients["Alice"] = alice
        wait_text(alice[2], "Alice 加入了聊天室")
        pause()

        bob = start_client("Bob", OUT)
        clients["Bob"] = bob
        wait_text(bob[2], "Bob 加入了聊天室")
        wait_text(alice[2], "Bob 加入了聊天室")
        pause()

        ming = start_client("小明", OUT)
        clients["小明"] = ming
        wait_text(ming[2], "小明 加入了聊天室")
        wait_text(alice[2], "小明 加入了聊天室")
        wait_text(bob[2], "小明 加入了聊天室")
        pause()

        rejected = subprocess.run(
            [str(CLIENT), "127.0.0.1", PORT, "Alice"],
            input="", capture_output=True, text=True, encoding="utf-8",
            timeout=5,
        )
        assert rejected.returncode == 1, "duplicate nickname was accepted"
        assert "NICK_TAKEN" in (rejected.stdout + rejected.stderr), (
            "duplicate nickname did not return NICK_TAKEN"
        )

        send(alice[0], "大家好，我是 Alice。Hello, this is Alice.")
        pause()
        wait_text(bob[2], "大家好，我是 Alice。Hello, this is Alice.")
        wait_text(ming[2], "大家好，我是 Alice。Hello, this is Alice.")
        pause()

        send(bob[0], "Hi Alice! 中英混合 mixed message 😀")
        wait_text(alice[2], "Hi Alice! 中英混合 mixed message 😀")
        wait_text(ming[2], "Hi Alice! 中英混合 mixed message 😀")
        pause()

        send(ming[0], "三人都在：Hello 大家好 123，管道符 | 也可以发送。")
        wait_text(alice[2], "三人都在：Hello 大家好 123，管道符 | 也可以发送。")
        wait_text(bob[2], "三人都在：Hello 大家好 123，管道符 | 也可以发送。")
        pause()

        send(bob[0], "/ping")
        wait_text(bob[2], "PONG")
        pause()
        send(bob[0], "/list")
        wait_text(bob[2], "在线用户（3）")
        wait_text(bob[2], "Alice")
        wait_text(bob[2], "小明")
        pause()

        stop_client(alice[0], alice[1])
        wait_text(bob[2], "Alice 离开了聊天室")
        wait_text(ming[2], "Alice 离开了聊天室")
        pause()

        stop_client(bob[0], bob[1])
        wait_text(ming[2], "Bob 离开了聊天室")
        pause()

        stop_client(ming[0], ming[1])
        time.sleep(0.3)

        counts = {name: check_sequence(path)
                  for name, (_, _, path) in clients.items()}
        server_text = (OUT / "server.log").read_text(encoding="utf-8")
        for nickname in clients:
            assert f'joined as "{nickname}"' in server_text
    finally:
        server.send_signal(signal.SIGINT)
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=2)
        server_log.close()

    print("integration test: PASS")
    for name, count in counts.items():
        print(f"  {name:6s}: {count} contiguous server frames")
    print(f"  logs: {OUT}")
    return 0


def _port_is_open(port: str) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", int(port)), timeout=0.2):
            return True
    except OSError:
        return False


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"integration test: FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
