#!/usr/bin/env python3
"""Generate deterministic complete-row framebuffer update workloads."""

import argparse
import mmap
import os
import subprocess
import time


def read_fb_geometry(device: str) -> tuple[int, int, int, int]:
    fb_name = os.path.basename(device)
    sysfs = f"/sys/class/graphics/{fb_name}"

    with open(f"{sysfs}/virtual_size", encoding="ascii") as source:
        width, height = (int(value) for value in source.read().strip().split(","))
    with open(f"{sysfs}/bits_per_pixel", encoding="ascii") as source:
        bits_per_pixel = int(source.read().strip())
    with open(f"{sysfs}/stride", encoding="ascii") as source:
        stride = int(source.read().strip())

    return width, height, bits_per_pixel, stride


def write_all(
    fd: int,
    mapping: mmap.mmap | None,
    data: bytes,
    offset: int,
) -> None:
    if mapping is not None:
        mapping[offset : offset + len(data)] = data
        return

    written = os.pwrite(fd, data, offset)
    if written != len(data):
        raise OSError(f"short framebuffer write: {written}/{len(data)}")


def run_workload(
    fd: int,
    mode: str,
    frames: int,
    width: int,
    height: int,
    stride: int,
    interval: float,
    dirty_helper: str | None,
    card: str,
    mapping: mmap.mmap | None,
) -> None:
    frame_size = height * stride
    patterns = (bytes(frame_size), bytes([0xFF]) * frame_size)
    fixed_row = height // 2
    split_x = width // 2

    for frame_index in range(frames):
        pattern = patterns[frame_index & 1]
        dirty_x1, dirty_x2 = 0, width

        if mode == "full":
            write_all(fd, mapping, pattern, 0)
            dirty_y1, dirty_y2 = 0, height
        elif mode == "same":
            write_all(fd, mapping, patterns[0], 0)
            dirty_y1, dirty_y2 = 0, height
        elif mode == "stripe":
            write_all(fd, mapping, pattern[: (height // 2) * stride], 0)
            dirty_y1, dirty_y2 = 0, height // 2
        elif mode == "row":
            row_start = fixed_row * stride
            write_all(
                fd,
                mapping,
                pattern[row_start : row_start + stride],
                row_start,
            )
            dirty_y1, dirty_y2 = fixed_row, fixed_row + 1
        elif mode == "sparse":
            for row in range(0, height, 2):
                row_start = row * stride
                write_all(
                    fd,
                    mapping,
                    pattern[row_start : row_start + stride],
                    row_start,
                )
            dirty_y1, dirty_y2 = 0, height - 1
        elif mode in ("left", "right"):
            if mode == "left":
                dirty_x1, dirty_x2 = 0, split_x
            else:
                dirty_x1, dirty_x2 = split_x, width
            row_offset = dirty_x1 * 4
            row_length = (dirty_x2 - dirty_x1) * 4
            for row in range(height):
                row_start = row * stride + row_offset
                write_all(
                    fd,
                    mapping,
                    pattern[row_start : row_start + row_length],
                    row_start,
                )
            dirty_y1, dirty_y2 = 0, height
        else:
            raise ValueError(f"unsupported mode: {mode}")

        if dirty_helper:
            subprocess.run(
                (
                    dirty_helper,
                    card,
                    str(dirty_x1),
                    str(dirty_y1),
                    str(dirty_x2),
                    str(dirty_y2),
                ),
                check=True,
            )
        if interval:
            time.sleep(interval)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "mode",
        choices=("full", "same", "stripe", "row", "sparse", "left", "right"),
    )
    parser.add_argument("--device", default="/dev/fb0")
    parser.add_argument("--frames", type=int, default=10)
    parser.add_argument("--interval-ms", type=float, default=0)
    parser.add_argument("--drain-ms", type=float, default=1000)
    parser.add_argument("--dirty-helper")
    parser.add_argument("--card", default="/dev/dri/card1")
    args = parser.parse_args()

    if args.frames < 1 or args.interval_ms < 0 or args.drain_ms < 0:
        parser.error("frames must be positive and delays must be non-negative")

    width, height, bits_per_pixel, stride = read_fb_geometry(args.device)
    if bits_per_pixel != 32 or stride < width * 4:
        parser.error(
            f"expected 32-bpp framebuffer, got {bits_per_pixel} bpp, "
            f"stride {stride}"
        )

    started = time.monotonic()
    fd = os.open(args.device, os.O_RDWR)
    mapping = None
    try:
        if args.dirty_helper:
            mapping = mmap.mmap(
                fd,
                height * stride,
                flags=mmap.MAP_SHARED,
                prot=mmap.PROT_READ | mmap.PROT_WRITE,
            )
        run_workload(
            fd,
            args.mode,
            args.frames,
            width,
            height,
            stride,
            args.interval_ms / 1000,
            args.dirty_helper,
            args.card,
            mapping,
        )
    finally:
        if mapping is not None:
            mapping.close()
        os.close(fd)

    write_elapsed = time.monotonic() - started
    time.sleep(args.drain_ms / 1000)
    total_elapsed = time.monotonic() - started
    print(
        f"mode={args.mode} frames={args.frames} "
        f"geometry={width}x{height} stride={stride} "
        f"write_seconds={write_elapsed:.6f} "
        f"total_seconds={total_elapsed:.6f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
