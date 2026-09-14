"""Turns a DLSSVideoPlayer.log into one row of the field verification matrix.

The matrix wants, per machine: GPU, driver, generation, the measured render pace,
the render receipt and the cold-start breakdown. Every one of those is already a
single line in the player's log, so collecting a row is reading the log rather
than transcribing a session by hand.

    python tools/verification/log_matrix_row.py DLSSVideoPlayer.log
    python tools/verification/log_matrix_row.py DLSSVideoPlayer.log --json

Standard library only. The last occurrence of each line wins, because a log
spans a whole session and the newest render is the one being reported. A line
that is not in the log is reported as absent, never as a zero or a guess: the
exit status is 1 when none of the four were found, so a wrong log file does not
read as a machine that failed.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# src/main.cpp: the bootstrap line, the pace line, and the two lines the receipt
# and the cold-start breakdown are logged under.
STARTUP = re.compile(r"Isolated neural helper available.*?GPU=(?P<gpu>.*?)"
                     r" driver=(?P<driver>\S*) generation=(?P<generation>\S+)")
PACE = re.compile(r"Measured neural render pace: (?P<pace>.+)$")
RECEIPT = re.compile(r"Neural render receipt: (?P<receipt>.+)$")
COLD_START = re.compile(r"Neural cold start: (?P<cold>.+)$")
# src/NeuralReceipt.cpp SummarizeNeuralReceiptForLog: gpu="..." driver=... .
RECEIPT_GPU = re.compile(r'gpu="(?P<gpu>[^"]*)"')
RECEIPT_DRIVER = re.compile(r"\sdriver=(?P<driver>\S+)")
# src/NeuralReceipt.cpp SummarizeNeuralColdStartForLog: key=1.234s or key=-.
COLD_START_FIELD = re.compile(r"(?P<key>[A-Za-z]+)=(?:(?P<seconds>[0-9.]+)s|-)")
ABSENT = "-"


def scan(text: str) -> dict:
    """Last occurrence of each line the matrix needs."""
    found: dict[str, object] = {}
    for line in text.splitlines():
        if match := STARTUP.search(line):
            found.update(gpu=match["gpu"], driver=match["driver"], generation=match["generation"])
        if match := PACE.search(line):
            found["pace"] = match["pace"].strip()
        if match := RECEIPT.search(line):
            found["receipt"] = match["receipt"].strip()
        if match := COLD_START.search(line):
            found["cold_start"] = match["cold"].strip()
    # The receipt line carries the probe's own GPU and driver strings, which are
    # the ones the runtime saw. They stand in when the bootstrap line is missing
    # (a log rotated mid-session starts after startup).
    receipt = str(found.get("receipt", ""))
    if receipt:
        if "gpu" not in found and (match := RECEIPT_GPU.search(receipt)):
            found["gpu"] = match["gpu"]
        if "driver" not in found and (match := RECEIPT_DRIVER.search(receipt)):
            found["driver"] = match["driver"]
    return found


def cold_start_phases(line: str) -> dict[str, float | None]:
    """Phase name to seconds; None for a phase the render never entered."""
    return {match["key"]: (float(match["seconds"]) if match["seconds"] is not None else None)
            for match in COLD_START_FIELD.finditer(line)}


def row(found: dict) -> dict:
    cold = str(found.get("cold_start", ""))
    return {
        "gpu": found.get("gpu") or None,
        "driver": found.get("driver") or None,
        "generation": found.get("generation") or None,
        "pace": found.get("pace") or None,
        "receipt": found.get("receipt") or None,
        "coldStart": cold or None,
        "coldStartSeconds": cold_start_phases(cold) if cold else {},
    }


def markdown(value: dict) -> str:
    """One matrix row. Absent cells are dashes, never blanks or zeros."""
    cells = [value["gpu"], value["driver"], value["generation"], value["pace"],
             value["coldStart"], value["receipt"]]
    return "| " + " | ".join(str(cell) if cell else ABSENT for cell in cells) + " |"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("log", type=Path, help="path to DLSSVideoPlayer.log")
    parser.add_argument("--json", action="store_true", help="emit the row as JSON")
    arguments = parser.parse_args(argv)
    try:
        text = arguments.log.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        print(f"cannot read {arguments.log}: {error}", file=sys.stderr)
        return 2
    value = row(scan(text))
    if arguments.json:
        print(json.dumps(value, indent=2))
    else:
        print("| GPU | Driver | Generation | Pace | Cold start | Receipt |")
        print("|-----|--------|------------|------|------------|---------|")
        print(markdown(value))
    missing = [name for name in ("gpu", "pace", "receipt", "coldStart") if not value[name]]
    if len(missing) == 4:
        print(f"{arguments.log}: no neural evidence in this log "
              f"(no startup, pace, receipt or cold-start line)", file=sys.stderr)
        return 1
    if missing:
        print(f"{arguments.log}: absent from this log: {', '.join(missing)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
