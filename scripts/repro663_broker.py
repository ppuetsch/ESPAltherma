"""Diagnosis of #663: an MQTT broker that only takes what a board publishes.

Enough of MQTT 3.1.1 for PubSubClient: CONNECT, SUBSCRIBE, PINGREQ and QoS 0 PUBLISH. Nothing is forwarded;
each publish is counted per topic so a run can tell what the board sent and when it reconnected.
"""

import argparse
import socket
import threading
import time
from collections import Counter

PACKET_CONNECT = 1
PACKET_PUBLISH = 3
PACKET_SUBSCRIBE = 8
PACKET_PINGREQ = 12
CONNACK_ACCEPTED = bytes([0x20, 0x02, 0x00, 0x00])
PINGRESP = bytes([0xD0, 0x00])
GRANTED_QOS_0 = 0x00

published_bytes_by_topic: Counter[str] = Counter()
published_count_by_topic: Counter[str] = Counter()


def log(text: str) -> None:
    print(f"{time.strftime('%H:%M:%S')} {text}", flush=True)


def read_exactly(connection: socket.socket, length: int) -> bytes:
    received = bytearray()
    while len(received) < length:
        piece = connection.recv(length - len(received))
        if not piece:
            raise ConnectionError("closed by the board")
        received += piece
    return bytes(received)


def read_remaining_length(connection: socket.socket) -> int:
    length, shift = 0, 0
    while True:
        digit = read_exactly(connection, 1)[0]
        length |= (digit & 0x7F) << shift
        if not digit & 0x80:
            return length
        shift += 7


def note_publish(body: bytes) -> None:
    topic_length = int.from_bytes(body[:2], "big")
    topic = body[2:2 + topic_length].decode("utf-8", "replace")
    payload_length = len(body) - 2 - topic_length
    published_bytes_by_topic[topic] += payload_length
    published_count_by_topic[topic] += 1
    is_log_line = topic.endswith("/log")
    if not is_log_line:
        log(f"publish {topic}: {payload_length} bytes")


def suback_for(body: bytes) -> bytes:
    packet_id = body[:2]
    topic_count = 0
    position = 2
    while position < len(body):
        topic_length = int.from_bytes(body[position:position + 2], "big")
        position += 2 + topic_length + 1
        topic_count += 1
    return bytes([0x90, 2 + topic_count]) + packet_id + bytes([GRANTED_QOS_0] * topic_count)


def serve_board(connection: socket.socket, address: tuple[str, int]) -> None:
    log(f"connection from {address[0]}")
    try:
        while True:
            packet_type = read_exactly(connection, 1)[0] >> 4
            body = read_exactly(connection, read_remaining_length(connection))
            if packet_type == PACKET_CONNECT:
                connection.sendall(CONNACK_ACCEPTED)
            elif packet_type == PACKET_PUBLISH:
                note_publish(body)
            elif packet_type == PACKET_SUBSCRIBE:
                connection.sendall(suback_for(body))
            elif packet_type == PACKET_PINGREQ:
                connection.sendall(PINGRESP)
    except (ConnectionError, OSError) as reason:
        log(f"connection from {address[0]} ended: {reason}")
    finally:
        connection.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=1883)
    arguments = parser.parse_args()
    listener = socket.create_server(("0.0.0.0", arguments.port))
    log(f"listening on port {arguments.port}")
    while True:
        connection, address = listener.accept()
        threading.Thread(target=serve_board, args=(connection, address), daemon=True).start()


if __name__ == "__main__":
    main()
