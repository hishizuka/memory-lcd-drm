#!/bin/sh
set -eu

PREFIX=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$PREFIX/bin/ffmpeg" "$@"
