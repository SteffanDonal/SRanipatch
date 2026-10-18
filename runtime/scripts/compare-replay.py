"""Compare matching numeric columns from two replay CSV files."""

import argparse
import csv
import math
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--prefix", default="raw_")
    args = parser.parse_args()

    with args.reference.open(newline="") as reference_file, args.candidate.open(
        newline=""
    ) as candidate_file:
        reference = csv.DictReader(reference_file)
        candidate = csv.DictReader(candidate_file)
        columns = [
            name
            for name in reference.fieldnames or []
            if name.startswith(args.prefix) and name in (candidate.fieldnames or [])
        ]
        if not columns:
            raise ValueError("No matching output columns")
        stats = {
            name: {"n": 0, "absolute": 0.0, "square": 0.0, "max": 0.0,
                   "bias": 0.0, "x": 0.0, "y": 0.0, "xx": 0.0, "yy": 0.0,
                   "xy": 0.0}
            for name in columns
        }
        frames = 0
        for reference_row, candidate_row in zip(reference, candidate, strict=True):
            if reference_row["sequence"] != candidate_row["sequence"]:
                raise ValueError(f"Sequence mismatch at row {frames + 2}")
            frames += 1
            for name in columns:
                x = float(reference_row[name])
                y = float(candidate_row[name])
                error = y - x
                item = stats[name]
                item["n"] += 1
                item["absolute"] += abs(error)
                item["square"] += error * error
                item["max"] = max(item["max"], abs(error))
                item["bias"] += error
                item["x"] += x
                item["y"] += y
                item["xx"] += x * x
                item["yy"] += y * y
                item["xy"] += x * y

    total_n = frames * len(columns)
    total_absolute = sum(item["absolute"] for item in stats.values())
    total_square = sum(item["square"] for item in stats.values())
    maximum = max(item["max"] for item in stats.values())
    print(f"frames: {frames}, outputs: {len(columns)}")
    print(
        f"aggregate MAE: {total_absolute / total_n:.8f}, "
        f"RMSE: {math.sqrt(total_square / total_n):.8f}, max: {maximum:.8f}"
    )
    print("output              MAE        RMSE       max        bias       corr")
    for name, item in stats.items():
        n = item["n"]
        covariance = item["xy"] - item["x"] * item["y"] / n
        variance_x = item["xx"] - item["x"] * item["x"] / n
        variance_y = item["yy"] - item["y"] * item["y"] / n
        denominator = math.sqrt(max(0.0, variance_x * variance_y))
        correlation = covariance / denominator if denominator else float("nan")
        print(
            f"{name:<18} {item['absolute'] / n:10.8f} "
            f"{math.sqrt(item['square'] / n):10.8f} {item['max']:10.8f} "
            f"{item['bias'] / n:10.8f} {correlation:10.6f}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
