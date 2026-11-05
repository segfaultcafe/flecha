#!/usr/bin/env python3
"""Deterministic competitive-language performance kernels."""

from __future__ import annotations

import sys


MASK = (1 << 64) - 1
ROUND_COUNT = 10_000_000
COLLECTION_COUNT = 4_000_000
COLLECTION_GROWTH_COUNT = 1_000_000
UTF8_GROUPS = 8_192
UTF8_REPETITIONS = 128
FNV_BYTES = 262_144
FNV_REPETITIONS = 64


def rotate_left(value: int, count: int) -> int:
    return ((value << count) | (value >> (64 - count))) & MASK


def rounds() -> int:
    left = 81_985_529_216_486_895
    right = 18_364_758_544_493_064_720
    for index in range(ROUND_COUNT):
        left = rotate_left(left, 7) ^ right
        right = (rotate_left(right, 13) + left + index) & MASK
        left ^= rotate_left(right, 17)
    return left ^ right


def collections_workload() -> int:
    values: list[int] = []
    for index in range(COLLECTION_COUNT):
        values.append(index)
    total = 0
    for value in values:
        total = (total + value) & MASK
    return total


def collections_growth_workload() -> int:
    values: list[int] = []
    for index in range(COLLECTION_GROWTH_COUNT):
        values.append(index)
    total = 0
    for value in values:
        total = (total + value) & MASK
    return total


def utf8_workload() -> int:
    group = bytes((65, 194, 162, 226, 130, 172, 240, 159, 152, 128))
    values = group * UTF8_GROUPS
    total = 0
    for _ in range(UTF8_REPETITIONS):
        offset = 0
        while offset < len(values):
            first = values[offset]
            width = 1
            if first <= 127:
                width = 1
            elif 194 <= first <= 223:
                width = 2
                if len(values) - offset < width or not 128 <= values[offset + 1] <= 191:
                    raise ValueError("invalid UTF-8")
            elif 224 <= first <= 239:
                width = 3
                if len(values) - offset < width:
                    raise ValueError("truncated UTF-8")
                second = values[offset + 1]
                if (not 128 <= second <= 191 or not 128 <= values[offset + 2] <= 191
                        or (first == 224 and second < 160)
                        or (first == 237 and second > 159)):
                    raise ValueError("invalid UTF-8")
            elif 240 <= first <= 244:
                width = 4
                if len(values) - offset < width:
                    raise ValueError("truncated UTF-8")
                second = values[offset + 1]
                if (not 128 <= second <= 191 or not 128 <= values[offset + 2] <= 191
                        or not 128 <= values[offset + 3] <= 191
                        or (first == 240 and second < 144)
                        or (first == 244 and second > 143)):
                    raise ValueError("invalid UTF-8")
            else:
                raise ValueError("invalid UTF-8")
            total += 1
            offset += width
    return total


def fnv_workload() -> int:
    values = bytes((index * 31 + 7) & 0xFF for index in range(FNV_BYTES))
    result = 14_695_981_039_346_656_037
    for _ in range(FNV_REPETITIONS):
        for value in values:
            result = ((result ^ value) * 1_099_511_628_211) & MASK
    return result


def main() -> int:
    if len(sys.argv) != 2:
        return 2
    workloads = {
        "rounds": rounds,
        "collections": collections_workload,
        "collections_growth": collections_growth_workload,
        "utf8": utf8_workload,
        "fnv": fnv_workload,
    }
    operation = workloads.get(sys.argv[1])
    if operation is None:
        return 3
    print(operation())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
