#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
"""Converts the official YOLOX-Nano checkpoint into a ZNET file.

The checkpoint is the release 0.1.1rc0 yolox_nano.pth of
Megvii-BaseDetection/YOLOX.  Its size and SHA-256 are checked before any
byte of it is parsed.  The checkpoint is a torch.save zip; its pickle is read
by an unpickler that allows only an ordered dictionary, the tensor rebuild
function and the storage classes, so nothing else in the file can run.  No
torch is needed.

Every convolution with a batch norm has the norm folded into its weights and
bias (eps 1e-3, in float64), and the stem's input channels are reordered from
BGR to RGB.  The ops are written in topological order with the tensors they
read and make; nothing else of the network is changed.

Usage: python3 -I yolox-convert.py --input yolox_nano.pth --output out.znet
       [--fp16]

The output is the same bytes for the same input on any host: float64
arithmetic and math.sqrt are correctly rounded, and struct packs a float32 to
the nearest even value.
"""

import argparse
import collections
import hashlib
import math
import os
import pickle
import struct
import sys
import zipfile

# The checkpoint this script accepts: release 0.1.1rc0 yolox_nano.pth.
SOURCE_SIZE = 7694953
SOURCE_SHA256 = "cd28f55fbbc1829f99d9ac9b38a16d259a22889739c8728ea877610201feff7b"

# The network: width 0.25, depth 0.33, depthwise, 80 classes, BN eps 1e-3.
BN_EPS = 1e-3
CLASSES = 80
INPUT_SIZE = 416
INPUT_MULTIPLE = 32
PAD_VALUE = 114
LEVEL_STRIDES = (8, 16, 32)

# The ZNET format, version 1 (design section 2.3).
ZNET_MAGIC = b"ZNET"
ZNET_VERSION = 1
ZNET_HEADER_SIZE = 256
ZNET_FLAG_FP16 = 0x1
ZNET_SECTION_ALIGN = 64
ZNET_ARRAY_ALIGN = 16
ZNET_TENSOR_INPUT = 0x1
ZNET_TENSOR_HEAD_OUTPUT = 0x2
OP_SPACE_TO_DEPTH = 1
OP_CONV = 2
OP_MAXPOOL = 3
OP_UPSAMPLE = 4
OP_ADD = 5
OP_CONCAT = 6
ACT_NONE = 0
ACT_SILU = 1

# The storage classes a checkpoint's tensors may use, by element size.
STORAGE_ELEMENT = {"FloatStorage": ("<f", 4)}


class Storage:
    """One storage record of the checkpoint: its key and element type."""

    def __init__(self, kind, key, numel):
        self.kind = kind
        self.key = key
        self.numel = numel


class Tensor:
    """One tensor rebuilt from the checkpoint: its storage, offset, shape and stride."""

    def __init__(self, storage, offset, size, stride):
        self.storage = storage
        self.offset = offset
        self.size = tuple(size)
        self.stride = tuple(stride)


def rebuild_tensor(storage, offset, size, stride, *unused):
    """Stands in for torch._utils._rebuild_tensor_v2."""
    return Tensor(storage, offset, size, stride)


class StorageType:
    """Stands in for a torch.*Storage class named by the pickle."""

    def __init__(self, name):
        self.name = name


class RestrictedUnpickler(pickle.Unpickler):
    """Allows only the classes a state dictionary of float tensors needs."""

    def find_class(self, module, name):
        # An ordered dictionary holds the state.
        if module == "collections" and name == "OrderedDict":
            return collections.OrderedDict

        # A tensor is rebuilt from its storage.
        if module == "torch._utils" and name == "_rebuild_tensor_v2":
            return rebuild_tensor

        # A storage class only names the element type.
        if module == "torch" and name.endswith("Storage"):
            return StorageType(name)

        # Anything else is refused.
        raise pickle.UnpicklingError("refused class %s.%s" % (module, name))

    def persistent_load(self, pid):
        # A storage is ('storage', class, key, location, numel).
        if not isinstance(pid, tuple) or len(pid) < 5 or pid[0] != "storage":
            raise pickle.UnpicklingError("unexpected persistent id")
        kind = pid[1].name if isinstance(pid[1], StorageType) else str(pid[1])
        return Storage(kind, str(pid[2]), int(pid[4]))


def check_source(path):
    """Checks the checkpoint's size and SHA-256 and reports the digest bytes."""
    size = os.path.getsize(path)
    if size != SOURCE_SIZE:
        raise SystemExit("yolox-convert: %s: size %d, expected %d" % (path, size, SOURCE_SIZE))
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    if digest.hexdigest() != SOURCE_SHA256:
        raise SystemExit("yolox-convert: %s: SHA-256 %s, expected %s" % (path, digest.hexdigest(), SOURCE_SHA256))
    return digest.digest()


def load_state(path):
    """Reads the checkpoint's model state into lists of floats by name."""
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        pickles = [name for name in names if name.endswith("/data.pkl")]
        if len(pickles) != 1:
            raise SystemExit("yolox-convert: no single data.pkl in the checkpoint")
        prefix = pickles[0][: -len("data.pkl")]
        with archive.open(pickles[0]) as stream:
            checkpoint = RestrictedUnpickler(stream).load()
        if not isinstance(checkpoint, dict) or "model" not in checkpoint:
            raise SystemExit("yolox-convert: the checkpoint has no model state")

        # Each tensor's elements, contiguous, from its storage.
        state = {}
        cache = {}
        for name, tensor in checkpoint["model"].items():
            if not isinstance(tensor, Tensor):
                continue
            # A batch norm's step counter (a LongStorage) is not a weight; it is never read.
            if name.endswith(".num_batches_tracked"):
                continue
            if tensor.storage.kind not in STORAGE_ELEMENT:
                raise SystemExit("yolox-convert: %s: storage %s" % (name, tensor.storage.kind))
            code, width = STORAGE_ELEMENT[tensor.storage.kind]
            count = 1
            for dimension in tensor.size:
                count *= dimension
            expected = []
            step = 1
            for dimension in reversed(tensor.size):
                expected.insert(0, step)
                step *= dimension
            if count > 0 and tuple(expected) != tensor.stride:
                raise SystemExit("yolox-convert: %s: not contiguous" % name)
            if tensor.offset + count > tensor.storage.numel:
                raise SystemExit("yolox-convert: %s: past its storage" % name)
            key = tensor.storage.key
            if key not in cache:
                raw = archive.read(prefix + "data/" + key)
                if len(raw) != tensor.storage.numel * width:
                    raise SystemExit("yolox-convert: storage %s: %d bytes" % (key, len(raw)))
                cache[key] = struct.unpack("<%d%s" % (tensor.storage.numel, code[1]), raw)
            values = cache[key][tensor.offset : tensor.offset + count]
            state[name] = (tensor.size, [float(value) for value in values])
    return state


class Graph:
    """The ZNET ops and tensors, built in topological order."""

    def __init__(self, state):
        self.state = state
        self.tensors = []  # (channels, divisor, flags)
        self.ops = []      # dict per op
        self.input = self.tensor(3, 1, ZNET_TENSOR_INPUT)

    def tensor(self, channels, divisor, flags=0):
        self.tensors.append([channels, divisor, flags])
        return len(self.tensors) - 1

    def op(self, name, kind, inputs, channels, divisor, **fields):
        output = self.tensor(channels, divisor)
        entry = {"name": name, "kind": kind, "inputs": list(inputs), "output": output,
                 "activation": ACT_NONE, "kernel": 0, "stride": 0, "groups": 0,
                 "weights": None, "bias": None}
        entry.update(fields)
        self.ops.append(entry)
        return output

    def channels(self, tensor):
        return self.tensors[tensor][0]

    def divisor(self, tensor):
        return self.tensors[tensor][1]

    def take(self, key):
        if key not in self.state:
            raise SystemExit("yolox-convert: the checkpoint has no %s" % key)
        return self.state[key]

    def folded(self, key, cin_groups, cout, k):
        """A BaseConv's weights with its batch norm folded in (float64)."""
        size, weights = self.take(key + ".conv.weight")
        if size != (cout, cin_groups, k, k):
            raise SystemExit("yolox-convert: %s: shape %s" % (key, size))
        _, gamma = self.take(key + ".bn.weight")
        _, beta = self.take(key + ".bn.bias")
        _, mean = self.take(key + ".bn.running_mean")
        _, var = self.take(key + ".bn.running_var")
        per = cin_groups * k * k
        out_w = []
        out_b = []
        for o in range(cout):
            scale = gamma[o] / math.sqrt(var[o] + BN_EPS)
            out_w.extend(weights[o * per + i] * scale for i in range(per))
            out_b.append(beta[o] - mean[o] * scale)
        return out_w, out_b

    def plain(self, key, cin, cout):
        """A prediction conv's own weights and bias."""
        size, weights = self.take(key + ".weight")
        if size != (cout, cin, 1, 1):
            raise SystemExit("yolox-convert: %s: shape %s" % (key, size))
        _, bias = self.take(key + ".bias")
        return list(weights), list(bias)

    def conv(self, name, key, x, cout, k, s, depthwise=False, activation=True, folded=True):
        cin = self.channels(x)
        groups = cin if depthwise else 1
        if folded:
            weights, bias = self.folded(key, cin // groups, cout, k)
        else:
            weights, bias = self.plain(key, cin, cout)
        divisor = self.divisor(x) * s
        return self.op(name, OP_CONV, [x], cout, divisor, kernel=k, stride=s, groups=groups,
                       activation=ACT_SILU if activation else ACT_NONE, weights=weights, bias=bias)


def backbone_key(name):
    """The checkpoint key of a layer: the backbone's, the PAFPN's or the head's."""
    if name.startswith(("dark", "stem")):
        return "backbone.backbone." + name
    if name.startswith("head."):
        return name
    return "backbone." + name


def build(state):
    """Builds the YOLOX-Nano graph (design sections 1.2 and 1.3)."""
    g = Graph(state)

    def base(name, x, cout, k, s):
        return g.conv(name, backbone_key(name), x, cout, k, s)

    def dwconv(name, x, cout, k, s):
        y = g.conv(name + ".dconv", backbone_key(name + ".dconv"), x, g.channels(x), k, s, depthwise=True)
        return g.conv(name + ".pconv", backbone_key(name + ".pconv"), y, cout, 1, 1)

    def add(name, a, b):
        return g.op(name, OP_ADD, [a, b], g.channels(a), g.divisor(a))

    def cat(name, xs):
        return g.op(name, OP_CONCAT, xs, sum(g.channels(x) for x in xs), g.divisor(xs[0]))

    def up(name, x):
        return g.op(name, OP_UPSAMPLE, [x], g.channels(x), g.divisor(x) // 2)

    def pool(name, x, k):
        return g.op(name, OP_MAXPOOL, [x], g.channels(x), g.divisor(x), kernel=k, stride=1)

    def bottleneck(name, x, cout, shortcut):
        y = base(name + ".conv1", x, cout, 1, 1)
        y = dwconv(name + ".conv2", y, cout, 3, 1)
        if shortcut and g.channels(x) == cout:
            y = add(name + ".add", y, x)
        return y

    def csp(name, x, cout, n, shortcut=True):
        hid = cout // 2
        x1 = base(name + ".conv1", x, hid, 1, 1)
        x2 = base(name + ".conv2", x, hid, 1, 1)
        for i in range(n):
            x1 = bottleneck("%s.m.%d" % (name, i), x1, hid, shortcut)
        return base(name + ".conv3", cat(name + ".cat", [x1, x2]), cout, 1, 1)

    def spp(name, x, cout):
        y = base(name + ".conv1", x, g.channels(x) // 2, 1, 1)
        pools = [pool("%s.m.%d" % (name, i), y, k) for i, k in enumerate((5, 9, 13))]
        return base(name + ".conv2", cat(name + ".cat", [y] + pools), cout, 1, 1)

    # Focus: space-to-depth, then the stem's 3x3 conv with its inputs in RGB order.
    focus = g.op("focus", OP_SPACE_TO_DEPTH, [g.input], 12, 2)
    x = g.conv("stem", "backbone.backbone.stem.conv", focus, 16, 3, 1)
    stem = g.ops[-1]
    stem["weights"] = rgb_order(stem["weights"], 16, 12, 3)

    x = dwconv("dark2.0", x, 32, 3, 2)
    x = csp("dark2.1", x, 32, 1)
    x = dwconv("dark3.0", x, 64, 3, 2)
    d3 = csp("dark3.1", x, 64, 3)
    x = dwconv("dark4.0", d3, 128, 3, 2)
    d4 = csp("dark4.1", x, 128, 3)
    x = dwconv("dark5.0", d4, 256, 3, 2)
    x = spp("dark5.1", x, 256)
    d5 = csp("dark5.2", x, 256, 1, False)
    f0 = base("lateral_conv0", d5, 128, 1, 1)
    x = csp("C3_p4", cat("cat_p4", [up("up0", f0), d4]), 128, 1, False)
    f1 = base("reduce_conv1", x, 64, 1, 1)
    p3 = csp("C3_p3", cat("cat_p3", [up("up1", f1), d3]), 64, 1, False)
    x = dwconv("bu_conv2", p3, 64, 3, 2)
    p4 = csp("C3_n3", cat("cat_n3", [x, f1]), 128, 1, False)
    x = dwconv("bu_conv1", p4, 128, 3, 2)
    p5 = csp("C3_n4", cat("cat_n4", [x, f0]), 256, 1, False)

    # The head: per level, the stem, the class branch and the box branch.
    levels = []
    for k, p in enumerate((p3, p4, p5)):
        s = base("head.stems.%d" % k, p, 64, 1, 1)
        c = dwconv("head.cls_convs.%d.0" % k, s, 64, 3, 1)
        c = dwconv("head.cls_convs.%d.1" % k, c, 64, 3, 1)
        cls = g.conv("head.cls_preds.%d" % k, "head.cls_preds.%d" % k, c, CLASSES, 1, 1, activation=False, folded=False)
        r = dwconv("head.reg_convs.%d.0" % k, s, 64, 3, 1)
        r = dwconv("head.reg_convs.%d.1" % k, r, 64, 3, 1)
        reg = g.conv("head.reg_preds.%d" % k, "head.reg_preds.%d" % k, r, 4, 1, 1, activation=False, folded=False)
        obj = g.conv("head.obj_preds.%d" % k, "head.obj_preds.%d" % k, r, 1, 1, 1, activation=False, folded=False)
        for tensor in (reg, obj, cls):
            g.tensors[tensor][2] |= ZNET_TENSOR_HEAD_OUTPUT
        levels.append((reg, obj, cls))
    return g, levels


def rgb_order(weights, cout, cin, k):
    """Reorders the stem's 12 inputs: new patch*3+c takes old patch*3+(2-c)."""
    out = list(weights)
    area = k * k
    for o in range(cout):
        for channel in range(cin):
            patch, color = divmod(channel, 3)
            source = patch * 3 + (2 - color)
            for i in range(area):
                out[(o * cin + channel) * area + i] = weights[(o * cin + source) * area + i]
    return out


def align(value, boundary):
    return (value + boundary - 1) // boundary * boundary


def write_znet(path, graph, levels, source_digest, fp16):
    """Writes the ZNET file (design section 2.3)."""
    element = "<e" if fp16 else "<f"
    width = 2 if fp16 else 4

    # The data section: each conv's weights then bias, each array 16-byte aligned.
    data = bytearray()
    for op in graph.ops:
        for field in ("weights", "bias"):
            values = op[field]
            if values is None:
                op[field + "_offset"] = 0
                op[field + "_count"] = 0
                continue
            data.extend(b"\0" * (align(len(data), ZNET_ARRAY_ALIGN) - len(data)))
            op[field + "_offset"] = len(data)
            op[field + "_count"] = len(values)
            data.extend(struct.pack("<%d%s" % (len(values), element[1]), *values))

    # The name table.
    names = bytearray()
    for op in graph.ops:
        op["name_offset"] = len(names)
        names.extend(op["name"].encode("ascii") + b"\0")

    # The tensor and op tables.
    tensors = b"".join(struct.pack("<4I", channels, divisor, flags, 0) for channels, divisor, flags in graph.tensors)
    ops = bytearray()
    for op in graph.ops:
        inputs = op["inputs"] + [0] * (4 - len(op["inputs"]))
        ops.extend(struct.pack("<16I", op["kind"], op["activation"], op["kernel"], op["stride"], op["groups"],
                               len(op["inputs"]), inputs[0], inputs[1], inputs[2], inputs[3], op["output"],
                               op["weights_offset"], op["weights_count"], op["bias_offset"], op["bias_count"],
                               op["name_offset"]))

    # The sections' places, each 64-byte aligned after the header.
    tensor_offset = ZNET_HEADER_SIZE
    op_offset = align(tensor_offset + len(tensors), ZNET_SECTION_ALIGN)
    data_offset = align(op_offset + len(ops), ZNET_SECTION_ALIGN)
    name_offset = align(data_offset + len(data), ZNET_SECTION_ALIGN)

    # The header.
    header = bytearray(ZNET_HEADER_SIZE)
    header[0:4] = ZNET_MAGIC
    struct.pack_into("<14I", header, 4, ZNET_VERSION, ZNET_HEADER_SIZE, ZNET_FLAG_FP16 if fp16 else 0,
                     len(graph.tensors), len(graph.ops), tensor_offset, op_offset, data_offset, len(data),
                     name_offset, len(names), graph.input, 0, INPUT_SIZE)
    struct.pack_into("<5I", header, 60, INPUT_MULTIPLE, PAD_VALUE, CLASSES, len(LEVEL_STRIDES), LEVEL_STRIDES[0])
    struct.pack_into("<3I", header, 80, LEVEL_STRIDES[1], LEVEL_STRIDES[2], 0)
    for level, triple in enumerate(levels):
        struct.pack_into("<3I", header, 92 + 12 * level, *triple)
    header[160:192] = source_digest
    header[192:224] = hashlib.sha256(bytes(data)).digest()
    header[224:224 + len(b"yolox-nano")] = b"yolox-nano"

    # The file, with zero padding between the sections.
    out = bytearray(header)
    for offset, section in ((tensor_offset, tensors), (op_offset, ops), (data_offset, data), (name_offset, names)):
        out.extend(b"\0" * (offset - len(out)))
        out.extend(section)
    with open(path, "wb") as target:
        target.write(bytes(out))
    return len(out), hashlib.sha256(bytes(out)).hexdigest()


def main():
    parser = argparse.ArgumentParser(description="Converts yolox_nano.pth into ZNET.")
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--fp16", action="store_true", help="store weights and biases as fp16 (tests only)")
    arguments = parser.parse_args()

    digest = check_source(arguments.input)
    state = load_state(arguments.input)
    graph, levels = build(state)
    size, sha = write_znet(arguments.output, graph, levels, digest, arguments.fp16)
    print("yolox-convert: %s: %d ops, %d tensors, %d bytes, SHA-256 %s" %
          (arguments.output, len(graph.ops), len(graph.tensors), size, sha))
    return 0


if __name__ == "__main__":
    sys.exit(main())
