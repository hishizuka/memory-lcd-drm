# Raspberry PiのハードウェアBGR・DRM直接出力

[English](README.md) | 日本語

このディレクトリは、Raspberry Pi Zero向け（ARMv6、32bit hard-float）の
FFmpegを別実行ファイルとしてビルドします。OS標準のFFmpegは置き換えません。

NV12対応Sharp DRMドライバーで推奨する経路は次のとおりです。

```text
H.264 -> bcm2835-codecのBGR4 DMA-BUF -> vout_drm -> Sharp DRM -> 行SPI
```

Raspberry PiのH.264 V4L2デコーダーは、`BGR4`（`AV_PIX_FMT_BGR0`）を
直接出力できます。YUV→BGR変換はハードウェア側で行われるため、別の
`scale_v4l2m2m`フィルターは不要です。`vout_drm`はcapture DMA-BUFを直接
importするため、通常経路にはDRM PRIME downloadもfbdev全画面copyもありません。
Sharpドライバーのprimary planeを列挙するため、voutパッチはuniversal planeを
有効化します。driverはimport planeを`drm_gem_fb_vmap()`でmapするため、
exporter固有のmappingとplane offsetを正しく扱います。

互換用にfbdev出力も残します。fbdev用パッチは冗長なBGR0→BGRA形式変換を
挿入せず、BGR0の24色成分bitを3byte/pixelと誤認しないよう、paddingを含む
32bitの格納幅でcopyします。

## ビルド環境

ARM64 VMまたはApple Container上のRaspberry Pi OS Trixie 64bitを使います。

```sh
sudo dpkg --add-architecture armhf
sudo apt update
sudo apt install \
  build-essential crossbuild-essential-armhf curl patch pkg-config \
  libc6-dev:armhf libdrm-dev:armhf
```

次を実行します。

```sh
./build-armhf.sh
```

スクリプトは、チェックサムを固定したFFmpeg、Raspberry Piパッチ、
ARMv6 glibc、Raspbian libgccを取得します。成果物はARMv6/VFP命令だけを
使用するため、初代Raspberry Pi Zeroで動作します。

```text
build-ffmpeg-rpi-isp/ffmpeg-rpi-isp_7.1.5_armhf.tar.xz
```

32bit環境をQEMUで実行する必要はありません。ARM64コンテナ上でARMHF
クロスコンパイラーを使い、Debian汎用ARMHFツールチェーンに含まれない
ARMv6用の起動・ランタイムオブジェクトをスクリプト側で固定しています。

## OS標準FFmpegを置き換えずにインストール

Raspberry Pi Zero上で次を実行します。

```sh
sudo tar -C / -xJf ffmpeg-rpi-isp_7.1.5_armhf.tar.xz
```

## 動画再生

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

通常再生では`-re`を残します。処理能力の測定時だけ外します。動画サイズは
DRM modeに合わせてください。このSPIディスプレイドライバーはscaleしません。
パネル更新は常に完全行単位です。

たとえば272x451パネルでは、272x204のH.264動画を`vout_drm`へ直接渡せません。
ドライバーが272x451のframebufferを要求するため、
`drmModeAddFB2WithModifiers`が`Invalid argument`を返します。縦横比を保つには、
OS標準のFFmpegでハードウェアデコードし、黒帯を付けてSharpのframebufferへ
出力します（デバイス番号は`/proc/fb`で確認してください）:

```sh
ffmpeg -re -no_cvt_hw -c:v h264_v4l2m2m -pixel_format bgr0 \
  -i umehara_272x204.mp4 -an \
  -vf 'hwdownload,format=bgr0,pad=272:451:0:123:color=black,format=bgra' \
  -pix_fmt bgra -f fbdev /dev/fb1
```

SPI TX slotが2個とも使用中なら、ドライバーはimport更新を待機させます。
patch済み`vout_drm`は待機frameを1枚だけ保持し、display threadが停止している
間は最新frameで置換します。これによりドライバーは通常のDRM commit semantics
を保ち、古い動画frameは送信前に置換されます。任意ピクセル更新は行いません。

従来のfbdev経路は次のとおりです。

```sh
/opt/ffmpeg-rpi-isp/bin/ffmpeg-rpi-isp \
  -re -no_cvt_hw -c:v h264_v4l2m2m -pixel_format bgr0 \
  -i umehara_non_sound.m4v \
  -vf 'hwdownload,format=bgr0' -pix_fmt bgr0 \
  -f fbdev /dev/fb1
```

OS標準FFmpegでもハードウェアBGR4出力は使えますが、fbdev出力の制約に
より、追加の形式変換が必要です。

```sh
ffmpeg -re -no_cvt_hw -c:v h264_v4l2m2m -pixel_format bgr0 \
  -i umehara_non_sound.m4v \
  -vf 'hwdownload,format=bgr0,format=bgra' \
  -pix_fmt bgra -f fbdev /dev/fb1
```

## Raspberry Pi Zeroでの結果

32bit Raspberry Pi Zero、400x240 JDIパネル、FFmpeg 7.1.5、
61秒・400x240・H.264・30fpsの動画で測定しました。

| 経路 | `-re`実時間 | user + sys CPU | 基準比 |
|---|---:|---:|---:|
| OS標準FFmpeg、ソフトウェアYUV→BGRA | 63.50秒 | 27.94秒 | 基準 |
| OS標準FFmpeg、BGR4ハードウェア変換＋BGR0→BGRAコピー | 63.46秒 | 23.97秒 | -14.2% |
| 本ビルド、BGR4ハードウェア変換→fbdev | 60.48秒 | 14.53秒 | -48.0% |
| 本ビルド、BGR4 DMA-BUF→DRM直接 | 60.63秒 | 14.38秒 | -48.5% |

従来のfbdev経路では、上表のFFmpeg process CPUとは別に、ドライバーの非同期
変換workerで7.32秒を計測しました。DRM直接経路の変換はFFmpegのioctlに課金
されるため、ドライバー変換9.99秒は14.38秒に含まれます。FFmpegとドライバー
変換を合わせた計測対象workは、fbdevの約21.85秒からDRM直接の14.38秒へ約34%
減少しました。直接経路はimport更新を956回受け付け、そのうち930回が
backpressureで待機し、SPIを59.13秒稼働させました。display threadの待機中は
FFmpegが古い待機frameを新しいframeへ置換するため、driverがcommit済み更新を
捨てることなく、queueを最新優先に保ちます。

別途実施したfbdevの`-re`なし比較では、OS標準の基準経路が33.74秒、
本ビルドが16.55秒で完走し、処理スループットは2.04倍になりました。
これは通常再生の測定ではありません。

Sharp DRMドライバーは引き続きframebuffer全体への書込みを受け付け、
パネルを完全行単位だけで更新します。このFFmpegパッチによって任意ピクセル
更新が追加されることはありません。
