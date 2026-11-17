"""Runs one firmware image through both ways of publishing the values over MQTT (#696): one message, and one topic
per value. For each, the heap at rest and what the broker (a second board with the repro663 harness) received.
One JSON object is printed per image; two images publish the same when their "one_message" and "one_topic" parts
are equal apart from the heap figures.
"""

import argparse
import io
import json
import sys
import time
import urllib.request
from pathlib import Path
from typing import Any

from mqtt_ram_measure import forget_published_topics, get_json, heap_at_rest, published_topics, status_once_settled, upload

type Json = dict[str, Any]

TIME_DEPENDENT_KEYS = ("FreeMem", "Uptime", "WifiRSSI")
SECONDS_FOR_TWO_POLLS = 75
SETTLE_SECONDS = 100


def post_config(board: str, settings: Json) -> None:
    request = urllib.request.Request(f"http://{board}/api/config", data=json.dumps(settings).encode(),
                                     headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=15) as response:
        response.read()


def values_message_without_time(broker: str) -> Json:
    message = get_json(broker, "/api/repro/broker/values")
    values = message[0] if isinstance(message, list) else (message or {})
    return {key: value for key, value in values.items() if key not in TIME_DEPENDENT_KEYS}


def what_the_broker_got(board: str, broker: str) -> Json:
    forget_published_topics(broker)
    time.sleep(SECONDS_FOR_TWO_POLLS)
    topics = [topic for topic in published_topics(broker) if not topic["topic"].endswith(("/log", "/ATTR", "/LWT"))]
    message = values_message_without_time(broker)
    return {"heap": heap_at_rest(board), "topics": topics, "values_message_keys": len(message), "values_message": message}


def check(board: str, broker: str, image: Path) -> Json:
    upload_answer = upload(board, image)
    status = status_once_settled(board, SETTLE_SECONDS)
    post_config(board, {"output": {"one_topic": False}})
    one_message = what_the_broker_got(board, broker)
    post_config(board, {"output": {"one_topic": True}})
    time.sleep(5)
    one_topic = what_the_broker_got(board, broker)
    post_config(board, {"output": {"one_topic": False}})
    return {"image": image.name, "upload": upload_answer, "values": status["hp"]["values"],
            "one_message": one_message, "one_topic": one_topic}


def main() -> None:
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board")
    parser.add_argument("broker")
    parser.add_argument("image", type=Path)
    arguments = parser.parse_args()
    print(json.dumps(check(arguments.board, arguments.broker, arguments.image)))


if __name__ == "__main__":
    main()
