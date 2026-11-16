"""Shows on a board what happens to the values of a registry when the unit answers it shorter (#667).

The image is uploaded to a board running the repro663 harness with its simulated unit. After a poll with the full
reply, the unit answers the registry a few bytes shorter, and the registry's values are read again after the next
poll. One JSON object is printed: the values before and after, and the reply length the board saw.
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

HTTP_TIMEOUT_SECONDS = 10
UPLOAD_TIMEOUT_SECONDS = 240
BOOT_TIMEOUT_SECONDS = 180
POLL_EVERY_MS = 10000
USUAL_POLL_EVERY_MS = 30000


def get_json(board: str, path: str) -> Any:
    with urllib.request.urlopen(f"http://{board}{path}", timeout=HTTP_TIMEOUT_SECONDS) as response:
        return json.load(response)


def post_config(board: str, settings: Json) -> None:
    request = urllib.request.Request(f"http://{board}/api/config", data=json.dumps(settings).encode(),
                                     headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT_SECONDS) as response:
        response.read()


def upload(board: str, image: Path) -> str:
    command = ["curl", "-sS", "--max-time", str(UPLOAD_TIMEOUT_SECONDS), "-w", " http=%{http_code}",
               "-F", f"firmware=@{image};filename={image.name}", f"http://{board}/update"]
    finished = subprocess.run(command, capture_output=True, text=True)
    return (finished.stdout + " " + finished.stderr).strip()


def wait_until_polling(board: str) -> None:
    deadline = time.monotonic() + BOOT_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        time.sleep(3)
        try:
            status = get_json(board, "/api/status")
        except (urllib.error.URLError, OSError, ValueError):
            continue
        on_the_new_image = "Firmware update" in status.get("restart_cause", "") and status["uptime"] >= 45
        if on_the_new_image and status["hp"]["values"] > 0 and status["hp"]["last_poll"] >= 0:
            return
    raise TimeoutError("the board did not come back polling")


def wait_for_a_new_poll(board: str) -> None:
    seen = get_json(board, "/api/status")["hp"]["last_poll"]
    while True:
        time.sleep(1)
        now = get_json(board, "/api/status")["hp"]["last_poll"]
        if now < seen:
            time.sleep(3)
            return
        seen = now


def registry_values(board: str, registry: int) -> dict[str, str]:
    return {value["l"]: value["v"] for value in get_json(board, "/api/values") if value["r"] == registry}


def measure(board: str, image: Path, registry: int, shorter_by: int) -> Json:
    upload_answer = upload(board, image)
    wait_until_polling(board)
    post_config(board, {"hp": {"frequency": POLL_EVERY_MS}})
    wait_for_a_new_poll(board)
    full = registry_values(board, registry)
    get_json(board, f"/api/repro/short?reg={registry}&by={shorter_by}")
    wait_for_a_new_poll(board)
    wait_for_a_new_poll(board)
    short = registry_values(board, registry)
    get_json(board, "/api/repro/short")
    post_config(board, {"hp": {"frequency": USUAL_POLL_EVERY_MS}})
    return {"image": image.name, "upload": upload_answer, "registry": hex(registry), "shorter_by": shorter_by,
            "full_reply": full, "short_reply": short,
            "changed": {name: [full[name], short.get(name)] for name in full if full[name] != short.get(name)}}


def main() -> None:
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board")
    parser.add_argument("image", type=Path)
    parser.add_argument("--registry", type=lambda text: int(text, 0), default=0x61)
    parser.add_argument("--by", type=int, default=4, help="bytes the unit leaves out at the end of the reply")
    arguments = parser.parse_args()
    print(json.dumps(measure(arguments.board, arguments.image, arguments.registry, arguments.by), ensure_ascii=False))


if __name__ == "__main__":
    main()
