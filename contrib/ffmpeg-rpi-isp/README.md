# Raspberry Pi hardware BGR and direct DRM output

English | [日本語](README_ja.md)

This directory builds a separate FFmpeg executable for Raspberry Pi Zero
(ARMv6, 32-bit hard-float). It does not replace the distribution FFmpeg.

The preferred path with the NV12-capable Sharp DRM driver is:

```text
H.264 -> bcm2835-codec BGR4 DMA-BUF -> vout_drm -> Sharp DRM -> row SPI
```

Raspberry Pi's H.264 V4L2 decoder can emit `BGR4` (`AV_PIX_FMT_BGR0`)
directly. This performs YUV-to-BGR conversion in the hardware pipeline and
avoids the separate `scale_v4l2m2m` filter. `vout_drm` imports the capture
DMA-BUF directly, so the normal path has neither a DRM PRIME download nor an
fbdev full-frame copy. The vout patch enables universal planes because the
Sharp driver exposes one primary plane. The driver maps imported planes with
`drm_gem_fb_vmap()` so the exporter mapping and plane offset are honored.

The build retains fbdev output as a compatibility fallback. Its local fbdev
patch writes BGR0 to a BGRA framebuffer without inserting a redundant
BGR0-to-BGRA format conversion and uses BGR0's padded 32-bit storage size.

## Build host

Use Raspberry Pi OS Trixie 64-bit in an ARM64 VM or Apple Container:

```sh
sudo dpkg --add-architecture armhf
sudo apt update
sudo apt install \
  build-essential crossbuild-essential-armhf curl patch pkg-config \
  libc6-dev:armhf libdrm-dev:armhf
```

Run:

```sh
./build-armhf.sh
```

The script downloads checksum-pinned FFmpeg, Raspberry Pi patch, ARMv6 glibc,
and Raspbian libgcc inputs. The resulting binary uses ARMv6/VFP instructions
and runs on the original Raspberry Pi Zero:

```text
build-ffmpeg-rpi-isp/ffmpeg-rpi-isp_7.1.5_armhf.tar.xz
```

An emulated 32-bit build container is not required. The ARM64 container uses
an ARMHF cross compiler while the script pins the ARMv6 startup and runtime
objects that Debian's generic ARMHF cross toolchain does not provide.

## Install without replacing distribution FFmpeg

On the Raspberry Pi Zero:

```sh
sudo tar -C / -xJf ffmpeg-rpi-isp_7.1.5_armhf.tar.xz
```

## Playback

```sh
/opt/ffmpeg-rpi-isp/bin/ffmpeg-rpi-isp \
  -re \
  -no_cvt_hw \
  -c:v h264_v4l2m2m \
  -pixel_format bgr0 \
  -i umehara_non_sound.m4v \
  -an \
  -f vout_drm \
  -drm_module sharp_drm -
```

Keep `-re` for normal playback. Remove it only for throughput benchmarks.
The video dimensions should match the DRM mode because this SPI display driver
does not scale. All panel updates remain complete-row updates.

For example, a 272x204 H.264 video cannot use `vout_drm` on a 272x451 panel:
the driver requires a 272x451 framebuffer, so `drmModeAddFB2WithModifiers`
returns `Invalid argument`. To keep the video's aspect ratio, use the
distribution FFmpeg with hardware decoding and pad to the panel size before
writing to the Sharp framebuffer (check `/proc/fb` for its device number):

```sh
ffmpeg -re -no_cvt_hw -c:v h264_v4l2m2m -pixel_format bgr0 \
  -i umehara_272x204.mp4 -an \
  -vf 'hwdownload,format=bgr0,pad=272:451:0:123:color=black,format=bgra' \
  -pix_fmt bgra -f fbdev /dev/fb1
```

The driver backpressures an imported update while both SPI TX slots are
occupied. The patched `vout_drm` keeps one pending frame and replaces it with
the newest frame while its display thread is blocked. The driver therefore
retains normal DRM commit semantics, while stale video frames are superseded
before submission. This does not introduce arbitrary pixel updating.

For the legacy fbdev path:

```sh
/opt/ffmpeg-rpi-isp/bin/ffmpeg-rpi-isp \
  -re -no_cvt_hw -c:v h264_v4l2m2m -pixel_format bgr0 \
  -i umehara_non_sound.m4v \
  -vf 'hwdownload,format=bgr0' -pix_fmt bgr0 \
  -f fbdev /dev/fb1
```

The distribution FFmpeg can use the hardware BGR4 capture too, but its fbdev
output requires an additional format step:

```sh
ffmpeg -re -no_cvt_hw -c:v h264_v4l2m2m -pixel_format bgr0 \
  -i umehara_non_sound.m4v \
  -vf 'hwdownload,format=bgr0,format=bgra' \
  -pix_fmt bgra -f fbdev /dev/fb1
```

## Raspberry Pi Zero result

Measured on a 32-bit Raspberry Pi Zero with a 400x240 JDI panel, FFmpeg 7.1.5,
and a 61-second 400x240 H.264 30 fps file:

| Path | `-re` wall | user + sys CPU | CPU vs baseline |
|---|---:|---:|---:|
| Distribution FFmpeg, software YUV-to-BGRA | 63.50 s | 27.94 s | baseline |
| Distribution FFmpeg, hardware BGR4 plus BGR0-to-BGRA copy | 63.46 s | 23.97 s | -14.2% |
| This build, hardware BGR4 to fbdev | 60.48 s | 14.53 s | -48.0% |
| This build, BGR4 DMA-BUF direct to DRM | 60.63 s | 14.38 s | -48.5% |

For the former fbdev path, the driver measured another 7.32 seconds in its
asynchronous conversion worker; that time is outside the FFmpeg process CPU
above. Direct DRM charges conversion to the FFmpeg ioctl and already includes
its 9.99 seconds of driver conversion in the 14.38-second process total.
Comparing the measured FFmpeg plus driver-conversion work therefore gives
approximately 21.85 seconds for fbdev versus 14.38 seconds for direct DRM, a
34% reduction. The direct run accepted 956 imported updates, backpressured 930
of them, and kept SPI busy for 59.13 seconds. FFmpeg replaced older pending
frames while the display thread waited, so the queue remained latest-oriented
without making the driver discard committed updates.

In a separate fbdev throughput comparison without `-re`, the distribution
baseline completed in 33.74 seconds and this build in 16.55 seconds, a 2.04x
improvement. Those runs are not normal playback measurements.

The Sharp DRM driver still accepts full framebuffer writes and updates the
panel only in row units. This FFmpeg patch does not add arbitrary pixel
updates.

## License and sources

This directory's scripts and patches use the repository's
[GPL-2.0-or-later license](../../LICENSE). The build downloads FFmpeg and
Raspberry Pi/Debian patches and runtime libraries; those components retain
their upstream licenses. Download URLs and SHA-256 checksums are recorded in
`build-armhf.sh`.

FFmpeg is built with `--enable-gpl`, so the resulting FFmpeg build uses
GPL-2.0-or-later. See [FFmpeg's licensing page](https://ffmpeg.org/legal.html)
and [Raspberry Pi's FFmpeg source archive](https://archive.raspberrypi.com/debian/pool/main/f/ffmpeg/).
No FFmpeg binaries or downloaded runtime libraries are included in this
repository's release assets.
