#!/usr/bin/env python3
"""Export the recovered SRanipal lip weights as a fixed ONNX graph."""

import argparse
from pathlib import Path

import numpy as np
import onnx
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


def export(weights_path: Path, output_path: Path) -> None:
    with np.load(weights_path) as weights:
        if set(weights.files) != set(SHAPES):
            raise ValueError("lip weight archive has an unexpected tensor set")
        initializers = []
        for name, shape in SHAPES.items():
            value = weights[name]
            if value.shape != shape or value.dtype != np.float32:
                raise ValueError(f"{name}: expected float32 {shape}, got {value.dtype} {value.shape}")
            initializers.append(numpy_helper.from_array(value, name))

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


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("weights", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    export(args.weights, args.output)


if __name__ == "__main__":
    main()
