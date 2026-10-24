#!/usr/bin/env python3
"""Convert SRanipal LPrediction's TVM weights to the recovered MNN lip graph."""

import argparse
import hashlib
import io
import math
import struct
import tempfile
from pathlib import Path

import numpy as np
import onnx
import _tools as mnn_tools
from onnx import TensorProto, helper, numpy_helper


SHAPES = {
    "conv1_weight": (20, 2, 5, 5),
    "conv1_bias": (20,),
    "conv2_weight": (48, 20, 5, 5),
    "conv2_bias": (48,),
    "conv3_weight": (64, 48, 3, 3),
    "conv3_bias": (64,),
    "fc5_ft_weight": (500, 25600),
    "fc5_ft_bias": (500,),
    "fc6_10_weight": (30, 500),
    "fc6_10_bias": (30,),
}


MODEL_SHA256 = "3d8ffd9cca1a237e38ce96f05d8ae99b3a21281537f9723dce174e5293355e01"


def load_weights(path: Path) -> dict[str, np.ndarray]:
    # TVM SaveParamDict: name vector followed by serialized CPU NDArrays.
    source = io.BytesIO(path.read_bytes())

    def read(size):
        value = source.read(size)
        if len(value) != size:
            raise ValueError("Truncated TVM parameter dictionary")
        return value

    def unpack(fmt):
        return struct.unpack(fmt, read(struct.calcsize(fmt)))

    if unpack("<QQQ") != (0xF7E58D4F05049CB7, 0, len(SHAPES)):
        raise ValueError("Expected the SRanipal TVM SaveParamDict header")
    names = []
    for _ in SHAPES:
        size, = unpack("<Q")
        if size > 128:
            raise ValueError("Invalid TVM parameter name length")
        names.append(read(size).decode("utf-8"))
    if len(set(names)) != len(names) or set(names) != set(SHAPES):
        raise ValueError("Unexpected lip parameter names")
    if unpack("<Q")[0] != len(names):
        raise ValueError("TVM parameter count does not match names")
    weights = {}
    for name in names:
        if unpack("<QQ") != (0xDD5E40F096B4A13F, 0):
            raise ValueError(f"{name}: invalid TVM NDArray header")
        device, device_id, ndim = unpack("<iii")
        dtype = unpack("<BBH")
        expected = SHAPES[name]
        if device != 1 or device_id != 0 or ndim != len(expected) or dtype != (2, 32, 1):
            raise ValueError(f"{name}: expected a float32 CPU tensor with {len(expected)} dimensions")
        shape = unpack("<" + "q" * ndim)
        size, = unpack("<q")
        if shape != expected or size != math.prod(expected) * 4:
            raise ValueError(f"{name}: unexpected shape or byte count")
        weights[name] = np.frombuffer(read(size), dtype="<f4").reshape(shape)
    if source.read(1):
        raise ValueError("Unexpected trailing TVM parameter data")
    return weights


def export(weights: dict[str, np.ndarray], output_path: Path) -> None:
    initializers = [numpy_helper.from_array(weights[name], name) for name in SHAPES]

    nodes = [
        helper.make_node("Conv", ["stereo", "conv1_weight", "conv1_bias"], ["conv1"]),
        helper.make_node("Relu", ["conv1"], ["relu1"]),
        helper.make_node("MaxPool", ["relu1"], ["pool1"], kernel_shape=[2, 2], strides=[2, 2]),
        helper.make_node("Conv", ["pool1", "conv2_weight", "conv2_bias"], ["conv2"]),
        helper.make_node("Relu", ["conv2"], ["relu2"]),
        helper.make_node("MaxPool", ["relu2"], ["pool2"], kernel_shape=[2, 2], strides=[2, 2]),
        helper.make_node("Conv", ["pool2", "conv3_weight", "conv3_bias"], ["conv3"]),
        helper.make_node("Relu", ["conv3"], ["relu3"]),
        helper.make_node("Flatten", ["relu3"], ["flat"], axis=1),
        helper.make_node(
            "Gemm", ["flat", "fc5_ft_weight", "fc5_ft_bias"], ["dense1"], transB=1
        ),
        helper.make_node(
            "Gemm", ["dense1", "fc6_10_weight", "fc6_10_bias"], ["dense2"], transB=1
        ),
        helper.make_node("Relu", ["dense2"], ["lip_raw"]),
    ]
    graph = helper.make_graph(
        nodes,
        "SRanipal lip prediction",
        [helper.make_tensor_value_info("stereo", TensorProto.FLOAT, [1, 2, 100, 100])],
        [helper.make_tensor_value_info("lip_raw", TensorProto.FLOAT, [1, 30])],
        initializers,
    )
    model = helper.make_model(
        graph,
        producer_name="sranipal-on-frame",
        opset_imports=[helper.make_opsetid("", 13)],
    )
    onnx.checker.check_model(model)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    onnx.save_model(model, output_path)


def model_digest(path: Path) -> str:
    data = bytearray(path.read_bytes())
    root, = struct.unpack_from("<I", data)

    def field_target(table, index):
        vtable = table - struct.unpack_from("<i", data, table)[0]
        offset, = struct.unpack_from("<H", data, vtable + 4 + 2 * index)
        if not offset:
            raise ValueError("MNN model is missing required metadata")
        location = table + offset
        return location + struct.unpack_from("<I", data, location)[0]

    def clear_string(location, expected_size):
        size, = struct.unpack_from("<I", data, location)
        if size != expected_size:
            raise ValueError("Unexpected MNN metadata length")
        data[location + 4:location + 4 + size] = b"0" * size

    # Net.mnn_uuid (11), Net.extraInfo (2), ExtraInfo.version (2).
    clear_string(field_target(root, 11), 36)
    clear_string(field_target(field_target(root, 2), 2), 5)
    return hashlib.sha256(data).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="SRanipal LPrediction directory")
    parser.add_argument("--output", type=Path, default=Path(__file__).with_name("lip_model.mnn"))
    args = parser.parse_args()
    weights = load_weights(args.source / "lip_opencl.params")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lip-converter-", dir=args.output.parent) as directory:
        intermediate = Path(directory) / "lip.onnx"
        result = Path(directory) / "lip.mnn"
        export(weights, intermediate)
        # The MNN wheel's native binding avoids the CLI wrapper's logging setup.
        mnn_tools.mnnconvert([
            "mnnconvert", "-f", "ONNX", "--modelFile", str(intermediate),
            "--MNNModel", str(result), "--bizCode", "sranipal-on-frame",
        ])
        if not result.is_file() or model_digest(result) != MODEL_SHA256:
            raise ValueError("Converted graph/weights differ from the reference lip_model.mnn; output was not replaced")
        result.replace(args.output)
    print(f"Saved {args.output.resolve()} (matches reference except UUID/version metadata)")


if __name__ == "__main__":
    main()
