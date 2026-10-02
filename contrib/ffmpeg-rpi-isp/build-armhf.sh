#!/bin/sh
set -eu

# Build a small Raspberry Pi FFmpeg for the ARMv6 hard-float ABI.
# Run this script on Raspberry Pi OS Trixie arm64 after installing the
# prerequisites documented in README.md.

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORK_DIR=${WORK_DIR:-"$PWD/build-ffmpeg-rpi-isp"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}

FFMPEG_VERSION=7.1.5
DEBIAN_VERSION=0+deb13u1+rpt1
ARCHIVE_BASE=https://archive.raspberrypi.com/debian/pool/main/f/ffmpeg
RPI_GLIBC_BASE=https://archive.raspberrypi.com/debian/pool/main/g/glibc
RASPBIAN_GCC_BASE=http://raspbian.raspberrypi.com/raspbian/pool/main/g/gcc-14
ORIG_TAR=ffmpeg_${FFMPEG_VERSION}.orig.tar.xz
DEBIAN_TAR=ffmpeg_${FFMPEG_VERSION}-${DEBIAN_VERSION}.debian.tar.xz
RPI_PATCH=ffmpeg-${FFMPEG_VERSION}-rpi_29.patch
LIBC_DEB=libc6_2.41-12+rpt1+deb13u3_armhf.deb
LIBC_DEV_DEB=libc6-dev_2.41-12+rpt1+deb13u3_armhf.deb
LIBGCC_DEV_DEB=libgcc-14-dev_14.2.0-19+rpi1_armhf.deb

ORIG_SHA256=de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f
DEBIAN_SHA256=1cfdb0bd348c24a4dd1738d24f2e6a0d5c471bd12f98c61f6cf72a292f88665f
LIBC_SHA256=9d3f847110a10e6ac2886046b16abd5f2eb022be24a7ea1b69ba8e1f5b75e9f0
LIBC_DEV_SHA256=c996061ae658c3ba286c7018bc72c952da22e47d563b81c7805ab8080d5cabc4
LIBGCC_DEV_SHA256=02552a49f7c754961de873133d6d2ec40a6ee3cb7205e37b3751019face9af3b

DOWNLOAD_DIR=$WORK_DIR/download
SOURCE_DIR=$WORK_DIR/ffmpeg-$FFMPEG_VERSION
BUILD_DIR=$WORK_DIR/build-armhf
STAGE_DIR=$WORK_DIR/stage
TOOLCHAIN_DIR=$WORK_DIR/armv6-toolchain
OUTPUT_ARCHIVE=$WORK_DIR/ffmpeg-rpi-isp_${FFMPEG_VERSION}_armhf.tar.xz

mkdir -p "$DOWNLOAD_DIR"

download()
{
    name=$1
    base=$2
    if [ ! -f "$DOWNLOAD_DIR/$name" ]; then
        curl -fL "$base/$name" -o "$DOWNLOAD_DIR/$name"
    fi
}

verify()
{
    expected=$1
    file=$2
    actual=$(sha256sum "$file" | awk '{print $1}')
    if [ "$actual" != "$expected" ]; then
        echo "Checksum mismatch: $file" >&2
        exit 1
    fi
}

download "$ORIG_TAR" "$ARCHIVE_BASE"
download "$DEBIAN_TAR" "$ARCHIVE_BASE"
download "$LIBC_DEB" "$RPI_GLIBC_BASE"
download "$LIBC_DEV_DEB" "$RPI_GLIBC_BASE"
download "$LIBGCC_DEV_DEB" "$RASPBIAN_GCC_BASE"
verify "$ORIG_SHA256" "$DOWNLOAD_DIR/$ORIG_TAR"
verify "$DEBIAN_SHA256" "$DOWNLOAD_DIR/$DEBIAN_TAR"
verify "$LIBC_SHA256" "$DOWNLOAD_DIR/$LIBC_DEB"
verify "$LIBC_DEV_SHA256" "$DOWNLOAD_DIR/$LIBC_DEV_DEB"
verify "$LIBGCC_DEV_SHA256" "$DOWNLOAD_DIR/$LIBGCC_DEV_DEB"

rm -rf "$SOURCE_DIR"
tar -xf "$DOWNLOAD_DIR/$ORIG_TAR" -C "$WORK_DIR"
tar -xf "$DOWNLOAD_DIR/$DEBIAN_TAR" -C "$SOURCE_DIR"
patch -d "$SOURCE_DIR" -p1 \
    -i "$SOURCE_DIR/debian/patches/$RPI_PATCH"
patch -d "$SOURCE_DIR" -p1 \
    -i "$SCRIPT_DIR/0001-fbdev-accept-bgr0-for-bgra.patch"
patch -d "$SOURCE_DIR" -p1 \
    -i "$SCRIPT_DIR/0002-vout-drm-use-universal-planes.patch"
patch -d "$SOURCE_DIR" -p1 \
    -i "$SCRIPT_DIR/0003-vout-drm-keep-latest-frame.patch"

rm -rf "$TOOLCHAIN_DIR"
mkdir -p "$TOOLCHAIN_DIR"
dpkg-deb -x "$DOWNLOAD_DIR/$LIBC_DEB" "$TOOLCHAIN_DIR"
dpkg-deb -x "$DOWNLOAD_DIR/$LIBC_DEV_DEB" "$TOOLCHAIN_DIR"
dpkg-deb --fsys-tarfile "$DOWNLOAD_DIR/$LIBGCC_DEV_DEB" |
    tar -x -C "$TOOLCHAIN_DIR" -f - \
        ./usr/lib/gcc/arm-linux-gnueabihf/14/crtbegin.o \
        ./usr/lib/gcc/arm-linux-gnueabihf/14/crtbeginS.o \
        ./usr/lib/gcc/arm-linux-gnueabihf/14/crtbeginT.o \
        ./usr/lib/gcc/arm-linux-gnueabihf/14/crtend.o \
        ./usr/lib/gcc/arm-linux-gnueabihf/14/crtendS.o \
        ./usr/lib/gcc/arm-linux-gnueabihf/14/libgcc.a \
        ./usr/lib/gcc/arm-linux-gnueabihf/14/libgcc_eh.a \
        ./usr/lib/gcc/arm-linux-gnueabihf/14/libgcc_s.so

ARMV6_GCC_LIB=$TOOLCHAIN_DIR/usr/lib/gcc/arm-linux-gnueabihf/14
ARMV6_LIBC_LIB=$TOOLCHAIN_DIR/usr/lib/arm-linux-gnueabihf

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

export PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig
export PKG_CONFIG_PATH=

"$SOURCE_DIR/configure" \
    --prefix=/opt/ffmpeg-rpi-isp \
    --arch=arm \
    --cpu=arm1176jzf-s \
    --disable-armv6t2 \
    --disable-neon \
    --extra-cflags="-marm -mfpu=vfp" \
    --extra-ldflags="-marm -mfpu=vfp -B$ARMV6_GCC_LIB/ -B$ARMV6_LIBC_LIB/" \
    --target-os=linux \
    --enable-cross-compile \
    --cross-prefix=arm-linux-gnueabihf- \
    --pkg-config=pkg-config \
    --enable-gpl \
    --enable-libdrm \
    --enable-vout-drm \
    --enable-v4l2-m2m \
    --enable-shared \
    --disable-static \
    --disable-doc \
    --disable-debug \
    --disable-everything \
    --enable-ffmpeg \
    --enable-avcodec \
    --enable-avdevice \
    --enable-avfilter \
    --enable-avformat \
    --enable-swscale \
    --enable-protocol=file \
    --enable-demuxer=mov \
    --enable-parser=h264 \
    --enable-decoder=h264 \
    --enable-decoder=h264_v4l2m2m \
    --enable-encoder=rawvideo \
    --enable-encoder=wrapped_avframe \
    --enable-bsf=h264_mp4toannexb \
    --enable-filter=format \
    --enable-filter=hwdownload \
    --enable-filter=scale \
    --enable-outdev=fbdev

make -j"$JOBS"

rm -rf "$STAGE_DIR"
make DESTDIR="$STAGE_DIR" install

cp "$SCRIPT_DIR/run-ffmpeg.sh" \
    "$STAGE_DIR/opt/ffmpeg-rpi-isp/bin/ffmpeg-rpi-isp"
chmod 0755 "$STAGE_DIR/opt/ffmpeg-rpi-isp/bin/ffmpeg-rpi-isp"

tar -C "$STAGE_DIR" -cJf "$OUTPUT_ARCHIVE" opt
sha256sum "$OUTPUT_ARCHIVE"
echo "$OUTPUT_ARCHIVE"
