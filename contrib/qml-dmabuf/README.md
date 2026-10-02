# Sharp DRM DMA-BUF presenter

English | [日本語](README_ja.md)

## Overview

This userspace library manages two linear XRGB8888 GBM buffers rendered by VC4
OpenGL, imports them into Sharp DRM through PRIME, and submits them with atomic
commits. It preserves normal DRM atomic semantics: only a `READY` frame that has
not entered a commit may be replaced by a newer render.

## Pipeline

```text
OpenGL render
  -> GBM buffer A/B
  -> at most one READY frame (replaceable before commit)
  -> submit worker (the atomic commit may block)
  -> sharp-drm desired/displayed rows
  -> immutable TX slot A/B
  -> SPI
```

Each input buffer moves through
`FREE -> RENDERING -> READY -> SUBMITTING -> ACTIVE`. `SUBMITTING` and `ACTIVE`
buffers are immutable. If no free buffer exists but the worker has not acquired
the `READY` buffer, the producer can replace that pending frame with the newest
scene. A committed frame is never silently discarded.

The worker reports completions through `eventfd`. Qt applications can monitor
it with `QSocketNotifier` and render only the newest scene when a buffer becomes
available, without a fixed frame-rate timer.

`sharp_presenter_submit_auto_damage()` compares the newly rendered linear
buffer with the active buffer in the submit worker and builds row damage clips.
The render thread returns after GPU completion and queueing, without waiting for
the CPU comparison or DRM commit. Unchanged frames are dropped before a commit;
submitted commits retain normal DRM lifetime semantics. Use
`sharp_presenter_submit()` when the producer already knows the damage range.

## Build and install

```sh
sudo apt install pkg-config libdrm-dev libgbm-dev libegl-dev libgles-dev
make
sudo make install
```

Creating and destroying the library requires a current EGL/OpenGL context in
the calling thread. `sharp_presenter_submit()` currently calls `glFinish()` for
correctness; a future version can replace it with explicit fences.

## Qt Quick smoke test

Run the example under a VC4 EGLFS screen and provide an `Item`-root QML file:

```sh
python3 qml_presenter_demo.py \
  --qml qml_dmabuf_demo.qml \
  --library "$PWD/libsharp_presenter.so"
```

Pass `--full-damage` to compare producer-supplied full-screen damage with the
default automatic row damage.
