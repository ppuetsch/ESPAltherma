"""Measures what publishing a poll's values takes from the heap, with and without web pages open (#692 follow-up).

On a board running the repro663 harness with its simulated unit: for a number of polls, the heap sampler is
restarted just after a poll was published and read just after the next one, so each figure is the lowest free data
heap of one publish. The pages are event-stream listeners, as an open web page is. One JSON object is printed.
"""

import argparse
import http.client
import io
import json
import statistics
import sys
import threading
import time
import urllib.request
from typing import Any

type Json = dict[str, Any]

HTTP_TIMEOUT_SECONDS = 10
SETTLE_SECONDS = 8


def get_json(board: str, path: str) -> Any:
    with urllib.request.urlopen(f"http://{board}{path}", timeout=HTTP_TIMEOUT_SECONDS) as response:
        return json.load(response)


class OpenPage(threading.Thread):
    """Reads the board's event stream like an open web page."""

    def __init__(self, board: str) -> None:
        super().__init__(daemon=True)
        self.board = board
        self.values_events = 0
        self.last_values_text = ""
        self.stopped = threading.Event()

    def run(self) -> None:
        try:
            connection = http.client.HTTPConnection(self.board, timeout=120)
            connection.request("GET", "/events", headers={"Accept": "text/event-stream"})
            stream = connection.getresponse()
            while not self.stopped.is_set():
                line = stream.readline()
                if not line:
                    return
                if line.startswith(b"event: values"):
                    self.values_events += 1
                    data_line = stream.readline()
                    self.last_values_text = data_line[len(b"data: "):].decode("utf-8", "replace").strip()
        except OSError:
            return


def wait_for_a_poll(board: str) -> None:
    seen = get_json(board, "/api/status")["hp"]["last_poll"]
    while True:
        time.sleep(0.5)
        now = get_json(board, "/api/status")["hp"]["last_poll"]
        if now < seen:
            return
        seen = now


def lowest_during_publishes(board: str, polls: int) -> list[Json]:
    results = []
    wait_for_a_poll(board)
    time.sleep(SETTLE_SECONDS)
    for _ in range(polls):
        get_json(board, "/api/repro/heap?restart=1")
        wait_for_a_poll(board)
        time.sleep(SETTLE_SECONDS)
        sampled = get_json(board, "/api/repro/heap")["sampled"]
        results.append({"lowest": sampled["lowest"], "stage": sampled["stage"],
                        "lowest_while_publishing": sampled["lowest_while_publishing"]})
    return results


def measure(board: str, pages: int, polls: int) -> Json:
    listeners = [OpenPage(board) for _ in range(pages)]
    for listener in listeners:
        listener.start()
    time.sleep(3)
    free_at_rest = get_json(board, "/api/repro/heap")["free"]
    samples = lowest_during_publishes(board, polls)
    for listener in listeners:
        listener.stopped.set()
    lowest = [sample["lowest"] for sample in samples]
    while_publishing = [sample["lowest_while_publishing"] for sample in samples]
    return {"pages": pages, "free_at_rest": free_at_rest, "samples": samples,
            "taken_median": free_at_rest - int(statistics.median(lowest)), "taken_most": free_at_rest - min(lowest),
            "taken_while_publishing_median": free_at_rest - int(statistics.median(while_publishing)),
            "taken_while_publishing_most": free_at_rest - min(while_publishing),
            "values_events": [listener.values_events for listener in listeners],
            "values_text": listeners[0].last_values_text if listeners else ""}


def main() -> None:
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board")
    parser.add_argument("--pages", type=int, default=0)
    parser.add_argument("--polls", type=int, default=4)
    arguments = parser.parse_args()
    print(json.dumps(measure(arguments.board, arguments.pages, arguments.polls)))


if __name__ == "__main__":
    main()
