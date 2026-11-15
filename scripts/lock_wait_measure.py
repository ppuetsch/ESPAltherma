"""Measures how long the heat pump task waits for the values lock while the broker is slow (#680).

The image is uploaded to a board running the repro663 harness with a simulated unit. A second board is the broker
and takes each piece of data late. Three situations are run for two minutes each, with a poll every 5 s:
the values in one message, one topic per value, and a Home Assistant discovery sent again every 15 s.
One JSON object is printed per run.
"""

import argparse
import io
import json
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

type Json = dict[str, Any]

HTTP_TIMEOUT_SECONDS = 15
UPLOAD_TIMEOUT_SECONDS = 240
BOOT_TIMEOUT_SECONDS = 180
SITUATION_SECONDS = 120
SECONDS_BETWEEN_DISCOVERIES = 15
POLL_EVERY_MS = 5000
USUAL_POLL_EVERY_MS = 30000


def get_json(host: str, path: str) -> Any:
    with urllib.request.urlopen(f"http://{host}{path}", timeout=HTTP_TIMEOUT_SECONDS) as response:
        return json.load(response)


def post_config(board: str, settings: Json) -> None:
    request = urllib.request.Request(f"http://{board}/api/config", data=json.dumps(settings).encode(),
                                     headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT_SECONDS) as response:
        response.read()


def upload(board: str, image: Path) -> str:
    form_field = f"firmware=@{image};filename={image.name}"
    command = ["curl", "-sS", "--max-time", str(UPLOAD_TIMEOUT_SECONDS), "-w", " http=%{http_code}", "-F", form_field,
               f"http://{board}/update"]
    finished = subprocess.run(command, capture_output=True, text=True)
    return (finished.stdout + " " + finished.stderr).strip()


def wait_until_polling(board: str) -> Json:
    deadline = time.monotonic() + BOOT_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        time.sleep(3)
        try:
            status = get_json(board, "/api/status")
        except (urllib.error.URLError, OSError, ValueError):
            continue
        on_the_new_image = "Firmware update" in status.get("restart_cause", "") and status["uptime"] >= 45
        if on_the_new_image and status["mqtt"]["connected"] and status["hp"]["values"] > 0:
            return status
    raise TimeoutError("the board did not come back polling")


def set_broker_delay(broker: str, milliseconds: int) -> None:
    get_json(broker, f"/api/repro/broker?slow_ms={milliseconds}")


def lock_wait_over(board: str, seconds: int, every_seconds: int = 0, action: Any = None) -> Json:
    """The lock waits counted from now for the given time; action is called every few seconds meanwhile."""
    get_json(board, "/api/repro/lockwait?restart=1")
    started = time.monotonic()
    turn = 0
    while time.monotonic() - started < seconds:
        if action is not None:
            action(turn)
            turn += 1
        time.sleep(every_seconds if action is not None else seconds)
    waits = get_json(board, "/api/repro/lockwait")
    status = get_json(board, "/api/status")
    return {"heat_pump_task": waits["heat_pump_task"], "other_tasks": waits["other_tasks"],
            "mqtt_connected": status["mqtt"]["connected"], "seconds_since_last_poll": status["hp"]["last_poll"]}


def measure(board: str, broker: str, image: Path, slow_ms: int) -> Json:
    set_broker_delay(broker, 0)
    upload_answer = upload(board, image)
    status = wait_until_polling(board)
    selection = get_json(board, "/api/config")["hp"]["labels"]

    def discover_again(turn: int) -> None:
        """Dropping and restoring the last value reloads the selection, which sends the discovery again."""
        labels = selection[:-1] if turn % 2 == 0 else selection
        post_config(board, {"hp": {"labels": labels}})

    post_config(board, {"hp": {"frequency": POLL_EVERY_MS}, "output": {"one_topic": False}})
    time.sleep(10)
    set_broker_delay(broker, slow_ms)
    one_message = lock_wait_over(board, SITUATION_SECONDS)
    post_config(board, {"output": {"one_topic": True}})
    time.sleep(10)
    one_topic_per_value = lock_wait_over(board, SITUATION_SECONDS)
    post_config(board, {"output": {"one_topic": False}})
    time.sleep(10)
    discovery = lock_wait_over(board, SITUATION_SECONDS, SECONDS_BETWEEN_DISCOVERIES, discover_again)
    set_broker_delay(broker, 0)
    post_config(board, {"hp": {"frequency": USUAL_POLL_EVERY_MS, "labels": selection}, "output": {"one_topic": False}})
    return {"image": image.name, "upload": upload_answer, "firmware": status["fw"], "values": status["hp"]["values"],
            "slow_ms": slow_ms, "one_message": one_message, "one_topic_per_value": one_topic_per_value,
            "discovery_every_15_s": discovery}


def main() -> None:
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board")
    parser.add_argument("broker")
    parser.add_argument("image", type=Path)
    parser.add_argument("--slow-ms", type=int, default=300, help="how late the broker takes each piece of data")
    arguments = parser.parse_args()
    print(json.dumps(measure(arguments.board, arguments.broker, arguments.image, arguments.slow_ms)))


if __name__ == "__main__":
    main()
