"""Diagnosis of #663: one firmware upload to a board running the repro663 firmware, with the memory it had.

The board is brought to a chosen amount of free data heap, a number of browser-like listeners are put on its
event stream, and a firmware image is uploaded the way the web page does it. The report says how low the heap
went, which allocations found no room, and whether the board restarted on its own.
"""

import argparse
import http.client
import io
import json
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any

type Json = dict[str, Any]

HTTP_TIMEOUT_SECONDS = 8
UPLOAD_TIMEOUT_SECONDS = 240
BOARD_BACK_TIMEOUT_SECONDS = 90
VALUES_EVENT_TIMEOUT_SECONDS = 70
CORRUPTED_BYTE_OFFSET_FROM_END = 4096


@dataclass
class Experiment:
    board: str
    firmware: Path
    wanted_free_bytes: int | None
    listeners: int
    keep_board_on_its_firmware: bool
    settle_seconds: int
    seconds_after_values_event: int | None


@dataclass
class BoardState:
    uptime_seconds: int
    reset_reason: str
    free_bytes: int
    lowest_free_bytes: int
    largest_block_bytes: int
    ballast_bytes: int
    sampled_lowest_bytes: int
    sampled_lowest_stage: str
    failed_allocations: Json
    failed_allocations_previous_boot: Json

    def restarted_since(self, earlier: "BoardState", seconds_between: float) -> bool:
        return self.uptime_seconds < earlier.uptime_seconds + seconds_between / 2


def get_json(board: str, path: str) -> Json:
    with urllib.request.urlopen(f"http://{board}{path}", timeout=HTTP_TIMEOUT_SECONDS) as response:
        return json.load(response)


def read_board_state(board: str, restart_sampling: bool = False) -> BoardState:
    status = get_json(board, "/api/status")
    heap = get_json(board, "/api/repro/heap?restart=1" if restart_sampling else "/api/repro/heap")
    return BoardState(
        uptime_seconds=int(status["uptime"]),
        reset_reason=str(status["reset_reason"]),
        free_bytes=int(heap["free"]),
        lowest_free_bytes=int(heap["lowest"]),
        largest_block_bytes=int(heap["block"]),
        ballast_bytes=int(heap["ballast"]),
        sampled_lowest_bytes=int(heap["sampled"]["lowest"]),
        sampled_lowest_stage=str(heap["sampled"]["stage"]),
        failed_allocations=heap["failed"],
        failed_allocations_previous_boot=heap["failed_previous_boot"],
    )


def wait_until_board_answers(board: str) -> BoardState | None:
    deadline = time.monotonic() + BOARD_BACK_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        try:
            return read_board_state(board)
        except (urllib.error.URLError, OSError, ValueError):
            time.sleep(2)
    return None


def hold_back_memory(board: str, wanted_free_bytes: int | None) -> None:
    if wanted_free_bytes is not None:
        get_json(board, f"/api/repro/ballast?free={wanted_free_bytes}")


class EventStreamListener(threading.Thread):
    """Reads the board's event stream like an open web page does, and counts what arrives."""

    def __init__(self, board: str) -> None:
        super().__init__(daemon=True)
        self.board = board
        self.received_bytes = 0
        self.values_events = 0
        self.stopped = threading.Event()

    def run(self) -> None:
        try:
            connection = http.client.HTTPConnection(self.board, timeout=UPLOAD_TIMEOUT_SECONDS)
            connection.request("GET", "/events", headers={"Accept": "text/event-stream"})
            stream = connection.getresponse()
            while not self.stopped.is_set():
                line = stream.readline()
                if not line:
                    return
                self.received_bytes += len(line)
                if line.startswith(b"event: values"):
                    self.values_events += 1
        except OSError:
            return


def start_listeners(board: str, count: int) -> list[EventStreamListener]:
    listeners = [EventStreamListener(board) for _ in range(count)]
    for listener in listeners:
        listener.start()
    return listeners


def seconds_since_last_poll(board: str) -> int:
    return int(get_json(board, "/api/status")["hp"]["last_poll"])


def wait_for_the_moment_in_the_poll_cycle(board: str, listeners: list[EventStreamListener], seconds_after_values_event: int | None) -> None:
    """A poll every 30 s and an upload of 12 s meet by chance only: start a chosen time after the last values went out."""
    if seconds_after_values_event is None:
        return
    deadline = time.monotonic() + VALUES_EVENT_TIMEOUT_SECONDS
    if listeners:
        seen = listeners[0].values_events
        while listeners[0].values_events == seen and time.monotonic() < deadline:
            time.sleep(0.05)
    else:
        while seconds_since_last_poll(board) > 1 and time.monotonic() < deadline:
            time.sleep(0.5)
    time.sleep(seconds_after_values_event)


def image_the_board_will_refuse(firmware: Path) -> Path:
    """The same bytes through the same flash writes, but the final check fails: the board keeps running, and its heap figures with it."""
    image = bytearray(firmware.read_bytes())
    image[-CORRUPTED_BYTE_OFFSET_FROM_END] ^= 0xFF
    refused = Path(tempfile.gettempdir()) / f"{firmware.stem}-refused.bin"
    refused.write_bytes(image)
    return refused


def upload(board: str, image: Path) -> str:
    form_field = f"firmware=@{image};filename={image.name}"
    command = ["curl", "-sS", "--max-time", str(UPLOAD_TIMEOUT_SECONDS), "-w", " http=%{http_code} seconds=%{time_total}",
               "-F", form_field, f"http://{board}/update"]
    finished = subprocess.run(command, capture_output=True, text=True)
    return (finished.stdout + " " + finished.stderr).strip()


def describe(label: str, state: BoardState) -> str:
    return (f"{label}: uptime {state.uptime_seconds} s, free {state.free_bytes}, lowest {state.lowest_free_bytes}, "
            f"largest block {state.largest_block_bytes}, held back {state.ballast_bytes}, "
            f"sampled lowest {state.sampled_lowest_bytes} in '{state.sampled_lowest_stage}', "
            f"failed allocations {state.failed_allocations['total']}")


def run(experiment: Experiment) -> None:
    hold_back_memory(experiment.board, experiment.wanted_free_bytes)
    listeners = start_listeners(experiment.board, experiment.listeners)
    time.sleep(experiment.settle_seconds)
    wait_for_the_moment_in_the_poll_cycle(experiment.board, listeners, experiment.seconds_after_values_event)
    before = read_board_state(experiment.board, restart_sampling=True)
    print(describe("before", before))

    image = image_the_board_will_refuse(experiment.firmware) if experiment.keep_board_on_its_firmware else experiment.firmware
    started = time.monotonic()
    print("upload:", upload(experiment.board, image))
    time.sleep(3)
    after = wait_until_board_answers(experiment.board)
    seconds_between = time.monotonic() - started
    for listener in listeners:
        listener.stopped.set()
    print("listeners:", [(listener.received_bytes, listener.values_events) for listener in listeners])
    report_outcome(before, after, seconds_between)


def report_outcome(before: BoardState, after: BoardState | None, seconds_between: float) -> None:
    if after is None:
        print("after: the board does not answer")
        return
    print(describe("after", after))
    if after.restarted_since(before, seconds_between):
        print(f"RESTARTED, reset reason: {after.reset_reason}")
        print("failed allocations before the restart:", json.dumps(after.failed_allocations_previous_boot))
    else:
        print("still running; failed allocations:", json.dumps(after.failed_allocations))


def parse_arguments() -> Experiment:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board")
    parser.add_argument("firmware", type=Path)
    parser.add_argument("--free", type=int, default=None, help="bytes of data heap left free before the upload")
    parser.add_argument("--listeners", type=int, default=1, help="open web pages (event stream connections)")
    parser.add_argument("--real", action="store_true", help="upload the image as it is: the board restarts on it")
    parser.add_argument("--settle", type=int, default=65, help="seconds with the listeners on before the upload")
    parser.add_argument("--after-values", type=int, default=None, help="start this many seconds after a values event")
    arguments = parser.parse_args()
    return Experiment(arguments.board, arguments.firmware, arguments.free, arguments.listeners, not arguments.real, arguments.settle, arguments.after_values)


def main() -> None:
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    run(parse_arguments())


if __name__ == "__main__":
    main()
