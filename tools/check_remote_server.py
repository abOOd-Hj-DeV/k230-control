#!/usr/bin/env python3
"""Short real-TLS smoke check; public synthetic pixels, no Android claims."""

import argparse
import json
import os
from pathlib import Path
import secrets
import socket
import ssl
import struct
import subprocess
import tempfile
import time
import zlib


def send(sock, kind, payload):
    sock.sendall(struct.pack(">BI", kind, len(payload)) + payload)


def exact(sock, size):
    result = bytearray()
    while len(result) < size:
        block = sock.recv(size - len(result))
        if not block:
            raise EOFError("remote closed")
        result.extend(block)
    return bytes(result)


def receive(sock):
    kind, size = struct.unpack(">BI", exact(sock, 5))
    assert 0 < size <= 16384
    return kind, exact(sock, size)


def png(width, height):
    def chunk(kind, data):
        return (
            struct.pack(">I", len(data))
            + kind
            + data
            + struct.pack(">I", zlib.crc32(kind + data))
        )

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress((b"\0" + b"\x80\x80\x80" * width) * height))
        + chunk(b"IEND", b"")
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    args.work_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.work_dir) as directory:
        directory = Path(directory)
        cert, key = directory / "server.crt", directory / "server.key"
        subprocess.run(
            [
                "openssl",
                "req",
                "-x509",
                "-newkey",
                "rsa:2048",
                "-nodes",
                "-days",
                "1",
                "-subj",
                "/CN=localhost",
                "-addext",
                "subjectAltName=DNS:localhost",
                "-keyout",
                str(key),
                "-out",
                str(cert),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        token = secrets.token_hex(32)
        with socket.socket() as reserve:
            reserve.bind(("127.0.0.1", 0))
            port = reserve.getsockname()[1]
        env = dict(os.environ, K230_REMOTE_TOKEN=token)
        process = subprocess.Popen(
            [
                str(args.server.resolve()),
                "--cert",
                str(cert),
                "--key",
                str(key),
                "--port",
                str(port),
                "--ui-model",
                str(root / "models/android-ui-yolov8n.onnx"),
                "--nsfwjs-model",
                str(root / "models/nsfwjs-mobilenet-v2.onnx"),
            ],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        try:
            assert "TLS analyzer listening" in process.stdout.readline()
            context = ssl.create_default_context(cafile=str(cert))

            def connect():
                result = context.wrap_socket(
                    socket.create_connection(("127.0.0.1", port), timeout=5),
                    server_hostname="localhost",
                )
                result.settimeout(5)
                return result

            with connect() as wrong:
                send(wrong, 0, b"x" * 64)
                assert wrong.recv(1) == b""
            with connect() as sock:
                send(sock, 0, token.encode())

                def fixture(name):
                    return json.loads(
                        (root / f"protocol/v2/fixtures/{name}.json").read_text()
                    )

                hello = fixture("hello")

                def clock():
                    return time.monotonic_ns() // 1000

                hello["phone_time_us"] = str(clock())
                send(sock, 1, (json.dumps(hello) + "\n").encode())
                kind, payload = receive(sock)
                binding = json.loads(payload)
                assert (
                    kind == 1
                    and binding["capture"]["source"]
                    == "android-mediaprojection-display"
                )
                stream = binding["stream_id"]
                seq = 0
                state = fixture("state")
                state["session_id"] = hello["session_id"]
                state["stream_id"] = stream
                state["screen"]["width"] = 640
                state["screen"]["height"] = 640
                state["screen"]["valid_from_us"] = str(clock() - 10000)
                image = png(640, 640)

                def bound(command, status):
                    nonlocal seq
                    seq += 1
                    reply = fixture("bound")
                    reply.update(
                        session_id=hello["session_id"],
                        stream_id=stream,
                        seq=str(seq),
                        request_seq=command["seq"],
                        status=status,
                        error=None,
                        phone_time_us=str(clock()),
                    )
                    send(sock, 1, (json.dumps(reply) + "\n").encode())

                bound(binding, "pending")
                for index in range(3):
                    if index:
                        time.sleep(0.55)
                    pts = clock()
                    header = (
                        struct.pack(
                            ">QIIQ",
                            pts,
                            640,
                            640,
                            int(state["screen"]["content_epoch"]),
                        )
                        + state["screen"]["screen_token"].encode()
                    )
                    send(sock, 2, header + image)
                    kind, payload = receive(sock)
                    command = json.loads(payload)
                    assert kind == 1 and command["capture_pts_us"] == str(pts)
                    bound(command, "accepted" if index == 2 else "pending")
                    assert receive(sock) == (5, b"\x01")
                seq += 1
                state["seq"] = str(seq)
                state["phone_time_us"] = str(clock())
                state["screen"]["sampled_at_us"] = state["phone_time_us"]
                send(sock, 1, (json.dumps(state) + "\n").encode())
                assert receive(sock) == (3, b"\x01")
                time.sleep(0.12)
                pts = clock()
                header = (
                    struct.pack(
                        ">QIIQ", pts, 640, 640, int(state["screen"]["content_epoch"])
                    )
                    + state["screen"]["screen_token"].encode()
                )
                send(sock, 2, header + image)
                kind, payload = receive(sock)
                assert kind == 4 and payload in (b"\0", b"\1")
                assert receive(sock) == (5, b"\x01")
                sock.sendall(struct.pack(">BI", 2, 8 * 1024 * 1024 + 1))
                assert sock.recv(1) == b""
            print(
                "PASS: TLS certificate verification, wrong-token rejection, 3 clock probes, native state, real-model PNG ingestion, oversized-record rejection."
            )
        finally:
            process.terminate()
            process.wait(timeout=5)


if __name__ == "__main__":
    main()
