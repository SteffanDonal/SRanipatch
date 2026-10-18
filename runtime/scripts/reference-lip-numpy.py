"""Run the recovered lip network directly with NumPy and OpenCV."""

import argparse
import csv
import struct
from pathlib import Path

import cv2
import numpy as np


def convolution(value, weight, bias):
    height, width = weight.shape[2:]
    windows = np.lib.stride_tricks.sliding_window_view(
        value, (height, width), axis=(2, 3)
    )
    result = np.einsum("nchwkl,ockl->nohw", windows, weight, optimize=True)
    return result + bias[None, :, None, None]


def max_pool(value):
    height = value.shape[2] // 2
    width = value.shape[3] // 2
    return value[:, :, : height * 2, : width * 2].reshape(
        value.shape[0], value.shape[1], height, 2, width, 2
    ).max(axis=(3, 5))


def predict(value, weights):
    value = np.maximum(convolution(value, weights["conv1_weight"], weights["conv1_bias"]), 0)
    value = max_pool(value)
    value = np.maximum(convolution(value, weights["conv2_weight"], weights["conv2_bias"]), 0)
    value = max_pool(value)
    value = np.maximum(convolution(value, weights["conv3_weight"], weights["conv3_bias"]), 0)
    value = value.reshape(value.shape[0], -1)
    value = value @ weights["fc5_ft_weight"].T + weights["fc5_ft_bias"]
    return np.maximum(value @ weights["fc6_10_weight"].T + weights["fc6_10_bias"], 0)


def read_frame(path, index):
    with path.open("rb") as source:
        header = source.read(32)
        magic, version, header_size, width, height, pixels, _ = struct.unpack(
            "<8s6I", header
        )
        if magic != b"SOFDATA\0" or version != 1 or header_size != 32 or pixels != 2:
            raise ValueError("Expected a version 1 YUYV dataset")
        payload_size = width * height * 2
        source.seek(32 + index * (16 + payload_size))
        sequence, timestamp = struct.unpack("<QQ", source.read(16))
        payload = source.read(payload_size)
        if len(payload) != payload_size:
            raise ValueError(f"Frame {index} is outside the dataset")
    return sequence, timestamp, np.frombuffer(payload, np.uint8).reshape(height, width * 2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("weights", type=Path)
    parser.add_argument("dataset", type=Path)
    parser.add_argument("--frame", type=int, action="append", default=[])
    parser.add_argument("--reference", type=Path)
    args = parser.parse_args()

    frames = args.frame or [0]
    reference = {}
    if args.reference:
        with args.reference.open(newline="") as source:
            for row in csv.DictReader(source):
                reference[int(row["sequence"])] = np.array(
                    [float(row[f"raw_{i}"]) for i in range(30)], dtype=np.float32
                )

    with np.load(args.weights) as weights:
        for frame_index in frames:
            sequence, timestamp, packed = read_frame(args.dataset, frame_index)
            merged = cv2.merge((packed[:, :400], packed[:, 400:]))
            resized = cv2.resize(merged, (100, 100), interpolation=cv2.INTER_CUBIC)
            model_input = resized.transpose(2, 0, 1)[None].astype(np.float32) / 255.0
            output = predict(model_input, weights)[0]
            print(f"frame={frame_index} sequence={sequence} timestamp_ns={timestamp}")
            print(",".join(f"{value:.9g}" for value in output))
            if sequence in reference:
                error = output - reference[sequence]
                print(
                    f"reference_mae={np.abs(error).mean():.9g} "
                    f"reference_max={np.abs(error).max():.9g}"
                )


if __name__ == "__main__":
    main()
