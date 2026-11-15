"""Measures one firmware image on a board for the MQTT memory work (#665): the heap at rest and what it publishes.

The image is uploaded, the board is left to connect to the broker and settle, then /api/heap is sampled and the
broker (a second board running the repro663 harness) tells what it last received on each topic, as a length and
a hash. Two images publish the same thing when those lines are equal. One JSON object is printed per run.
"""

import argparse
import io
import json
import statistics
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

type Json = dict[str, Any]

HTTP_TIMEOUT_SECONDS = 8
UPLOAD_TIMEOUT_SECONDS = 240
BOOT_TIMEOUT_SECONDS = 120
HEAP_SAMPLES = 7
SECONDS_BETWEEN_SAMPLES = 4


def get_json(host: str, path: str) -> Any:
    with urllib.request.urlopen(f"http://{host}{path}", timeout=HTTP_TIMEOUT_SECONDS) as response:
        return json.load(response)


def upload(board: str, image: Path) -> str:
    form_field = f"firmware=@{image};filename={image.name}"
    command = ["curl", "-sS", "--max-time", str(UPLOAD_TIMEOUT_SECONDS), "-w", " http=%{http_code}", "-F", form_field,
               f"http://{board}/update"]
    finished = subprocess.run(command, capture_output=True, text=True)
    return (finished.stdout + " " + finished.stderr).strip()


def status_once_settled(board: str, settle_seconds: int) -> Json:
    """The board restarted on the new image, has been up for the settle time and is connected to the broker."""
    deadline = time.monotonic() + BOOT_TIMEOUT_SECONDS + settle_seconds
    while time.monotonic() < deadline:
        time.sleep(3)
        try:
            status = get_json(board, "/api/status")
        except (urllib.error.URLError, OSError, ValueError):
            continue
        restarted_by_the_update = "Firmware update" in status.get("restart_cause", "")
        settled = settle_seconds <= status["uptime"] < settle_seconds + 60
        if restarted_by_the_update and settled and status["mqtt"]["connected"]:
            return status
    raise TimeoutError("the board did not settle on the new image")


def heap_at_rest(board: str) -> Json:
    samples = []
    for _ in range(HEAP_SAMPLES):
        samples.append(get_json(board, "/api/heap"))
        time.sleep(SECONDS_BETWEEN_SAMPLES)
    return {
        "total": samples[-1]["total"],
        "free_median": int(statistics.median(sample["free"] for sample in samples)),
        "free_lowest_sample": min(sample["free"] for sample in samples),
        "min_since_boot": samples[-1]["min"],
        "largest_block_median": int(statistics.median(sample["block"] for sample in samples)),
    }


def published_topics(broker: str) -> list[Json]:
    return sorted(get_json(broker, "/api/repro/broker")["topics"], key=lambda topic: topic["topic"])


def forget_published_topics(broker: str) -> None:
    get_json(broker, "/api/repro/broker?forget=1")


def measure(board: str, broker: str, image: Path, settle_seconds: int) -> Json:
    forget_published_topics(broker)
    upload_answer = upload(board, image)
    status = status_once_settled(board, settle_seconds)
    return {
        "image": image.name,
        "upload": upload_answer,
        "firmware": status["fw"],
        "uptime": status["uptime"],
        "values": status["hp"]["values"],
        "heap": heap_at_rest(board),
        "topics": published_topics(broker),
    }


def main() -> None:
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board")
    parser.add_argument("broker")
    parser.add_argument("image", type=Path)
    parser.add_argument("--settle", type=int, default=150, help="seconds of uptime before the heap is sampled")
    arguments = parser.parse_args()
    print(json.dumps(measure(arguments.board, arguments.broker, arguments.image, arguments.settle)))


if __name__ == "__main__":
    main()
