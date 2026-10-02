#!/usr/bin/env python3
"""Exercise the double-buffered Sharp presenter with Qt Quick."""

import argparse
import ctypes
from pathlib import Path
import select
import sys
import time

from PySide6.QtCore import QSize, QUrl
from PySide6.QtGui import (
    QGuiApplication,
    QOffscreenSurface,
    QOpenGLContext,
    QSurfaceFormat,
)
from PySide6.QtQml import QQmlComponent, QQmlEngine
from PySide6.QtQuick import (
    QQuickGraphicsDevice,
    QQuickItem,
    QQuickRenderControl,
    QQuickRenderTarget,
    QQuickWindow,
    QSGRendererInterface,
)

WIDTH = 272
HEIGHT = 451
RENDER_NODE = b"/dev/dri/by-path/platform-soc:gpu-render"
SHARP_CARD = b"/dev/dri/by-path/platform-3f204000.spi-cs-0-card"


class PresenterStats(ctypes.Structure):
    _fields_ = [
        ("submitted_frames", ctypes.c_uint64),
        ("presented_frames", ctypes.c_uint64),
        ("replaced_pending_frames", ctypes.c_uint64),
        ("busy_acquires", ctypes.c_uint64),
        ("commit_errors", ctypes.c_uint64),
        ("unchanged_frames", ctypes.c_uint64),
        ("damage_rows", ctypes.c_uint64),
    ]


class SharpPresenter:
    ACQUIRED = 0
    BUSY = 1

    def __init__(self, library_path):
        self.library = ctypes.CDLL(str(library_path))
        self.library.sharp_presenter_last_error.argtypes = [ctypes.c_void_p]
        self.library.sharp_presenter_last_error.restype = ctypes.c_char_p
        self.library.sharp_presenter_create.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
        ]
        self.library.sharp_presenter_create.restype = ctypes.c_void_p
        self.library.sharp_presenter_acquire.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self.library.sharp_presenter_acquire.restype = ctypes.c_int
        self.library.sharp_presenter_submit.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_uint32,
        ]
        self.library.sharp_presenter_submit.restype = ctypes.c_int
        self.library.sharp_presenter_submit_auto_damage.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
        ]
        self.library.sharp_presenter_submit_auto_damage.restype = ctypes.c_int
        self.library.sharp_presenter_event_fd.argtypes = [ctypes.c_void_p]
        self.library.sharp_presenter_event_fd.restype = ctypes.c_int
        self.library.sharp_presenter_dispatch.argtypes = [ctypes.c_void_p]
        self.library.sharp_presenter_dispatch.restype = ctypes.c_int
        self.library.sharp_presenter_stride.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
        ]
        self.library.sharp_presenter_stride.restype = ctypes.c_uint32
        self.library.sharp_presenter_modifier.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
        ]
        self.library.sharp_presenter_modifier.restype = ctypes.c_uint64
        self.library.sharp_presenter_dump_active_ppm.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
        ]
        self.library.sharp_presenter_dump_active_ppm.restype = ctypes.c_int
        self.library.sharp_presenter_get_stats.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(PresenterStats),
        ]
        self.library.sharp_presenter_get_stats.restype = ctypes.c_int
        self.library.sharp_presenter_restore.argtypes = [ctypes.c_void_p]
        self.library.sharp_presenter_restore.restype = ctypes.c_int
        self.library.sharp_presenter_destroy.argtypes = [ctypes.c_void_p]
        self.context = self.library.sharp_presenter_create(
            RENDER_NODE, SHARP_CARD, WIDTH, HEIGHT
        )
        if not self.context:
            raise RuntimeError(self.last_error())

    def last_error(self):
        error = self.library.sharp_presenter_last_error(self.context)
        return error.decode() if error else "Unknown presenter error"

    @property
    def event_fd(self):
        return self.library.sharp_presenter_event_fd(self.context)

    def acquire(self):
        index = ctypes.c_uint32()
        renderbuffer = ctypes.c_uint32()
        result = self.library.sharp_presenter_acquire(
            self.context, ctypes.byref(index), ctypes.byref(renderbuffer)
        )
        if result == self.BUSY:
            return None
        if result != self.ACQUIRED:
            raise RuntimeError(self.last_error())
        return index.value, renderbuffer.value

    def submit(self, index, auto_damage=True):
        if auto_damage:
            result = self.library.sharp_presenter_submit_auto_damage(
                self.context, index
            )
        else:
            result = self.library.sharp_presenter_submit(self.context, index, 0, HEIGHT)
        if result != 0:
            raise RuntimeError(self.last_error())

    def dispatch(self):
        if self.library.sharp_presenter_dispatch(self.context) != 0:
            raise RuntimeError(self.last_error())

    def buffer_info(self, index):
        return (
            self.library.sharp_presenter_stride(self.context, index),
            self.library.sharp_presenter_modifier(self.context, index),
        )

    def stats(self):
        stats = PresenterStats()
        if (
            self.library.sharp_presenter_get_stats(self.context, ctypes.byref(stats))
            != 0
        ):
            raise RuntimeError(self.last_error())
        return stats

    def dump(self, path):
        if (
            self.library.sharp_presenter_dump_active_ppm(
                self.context, str(path).encode()
            )
            != 0
        ):
            raise RuntimeError(self.last_error())

    def close(self):
        if self.context:
            self.library.sharp_presenter_destroy(self.context)
            self.context = None


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qml", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=30)
    parser.add_argument("--interval", type=float, default=0.05)
    parser.add_argument("--dump", type=Path)
    parser.add_argument("--full-damage", action="store_true")
    return parser.parse_args()


def main():
    args = parse_args()
    QQuickWindow.setGraphicsApi(QSGRendererInterface.GraphicsApi.OpenGL)
    app = QGuiApplication(sys.argv[:1])

    surface_format = QSurfaceFormat()
    surface_format.setRenderableType(QSurfaceFormat.RenderableType.OpenGLES)
    surface_format.setVersion(2, 0)
    context = QOpenGLContext()
    context.setFormat(surface_format)
    if not context.create():
        raise RuntimeError("QOpenGLContext.create() failed")
    surface = QOffscreenSurface()
    surface.setFormat(context.format())
    surface.create()
    if not surface.isValid() or not context.makeCurrent(surface):
        raise RuntimeError("Creating the Qt offscreen surface failed")

    render_control = QQuickRenderControl()
    quick_window = QQuickWindow(render_control)
    engine = QQmlEngine()
    if not engine.incubationController():
        engine.setIncubationController(quick_window.incubationController())
    component = QQmlComponent(engine, QUrl.fromLocalFile(str(args.qml.resolve())))
    if component.isError():
        raise RuntimeError("\n".join(error.toString() for error in component.errors()))
    root = component.create()
    if not isinstance(root, QQuickItem):
        raise RuntimeError("QML root object is not a QQuickItem")
    root.setParentItem(quick_window.contentItem())
    root.setSize(QSize(WIDTH, HEIGHT))
    quick_window.setGeometry(0, 0, WIDTH, HEIGHT)
    quick_window.setGraphicsDevice(QQuickGraphicsDevice.fromOpenGLContext(context))

    output = SharpPresenter(args.library)
    current_target = None
    submit_times = []

    def render_latest():
        nonlocal current_target
        acquired = output.acquire()
        if acquired is None:
            return False
        index, renderbuffer = acquired
        target = QQuickRenderTarget.fromOpenGLRenderBuffer(
            renderbuffer, QSize(WIDTH, HEIGHT)
        )
        target.setMirrorVertically(True)
        quick_window.setRenderTarget(target)
        current_target = target
        if not context.makeCurrent(surface):
            raise RuntimeError("QOpenGLContext.makeCurrent() failed")
        render_control.beginFrame()
        render_control.polishItems()
        render_control.sync()
        render_control.render()
        render_control.endFrame()
        submit_started = time.monotonic()
        output.submit(index, auto_damage=not args.full_damage)
        submit_times.append(time.monotonic() - submit_started)
        return True

    if not render_control.initialize():
        raise RuntimeError("QQuickRenderControl.initialize() failed")

    dirty = True
    rendered = 0
    started = time.monotonic()
    try:
        for frame in range(args.frames):
            root.setProperty("counter", frame)
            dirty = True
            deadline = time.monotonic() + args.interval
            while time.monotonic() < deadline:
                app.processEvents()
                readable, _, _ = select.select([output.event_fd], [], [], 0)
                if readable:
                    output.dispatch()
                if dirty and render_latest():
                    dirty = False
                    rendered += 1
                time.sleep(0.001)
        drain_deadline = time.monotonic() + 2.0
        while time.monotonic() < drain_deadline:
            app.processEvents()
            readable, _, _ = select.select([output.event_fd], [], [], 0.01)
            if readable:
                output.dispatch()
            if dirty and render_latest():
                dirty = False
                rendered += 1
        if args.dump is not None:
            output.dump(args.dump)
        stats = output.stats()
        ordered_submit_times = sorted(submit_times)
        submit_p95 = ordered_submit_times[
            min(len(ordered_submit_times) - 1, int(len(ordered_submit_times) * 0.95))
        ]
        print(
            f"inputs={args.frames} rendered={rendered} "
            f"submitted={stats.submitted_frames} "
            f"presented={stats.presented_frames} "
            f"replaced_pending={stats.replaced_pending_frames} "
            f"busy_acquires={stats.busy_acquires} "
            f"unchanged={stats.unchanged_frames} "
            f"damage_rows={stats.damage_rows} "
            f"commit_errors={stats.commit_errors} "
            f"elapsed={time.monotonic() - started:.3f}s"
        )
        print(
            f"submit_avg_ms={sum(submit_times) / len(submit_times) * 1000:.3f} "
            f"submit_p95_ms={submit_p95 * 1000:.3f}"
        )
        for index in range(2):
            stride, modifier = output.buffer_info(index)
            print(f"buffer={index} stride={stride} modifier=0x{modifier:016x}")
    finally:
        if context.makeCurrent(surface):
            quick_window.setRenderTarget(QQuickRenderTarget())
            render_control.invalidate()
            output.close()
            context.doneCurrent()
        del current_target
        del root
        del component
        del engine
        del quick_window
        del render_control


if __name__ == "__main__":
    main()
