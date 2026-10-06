# Memory LCD DRM Driver

[English](README.md) | 日本語

https://github.com/hishizuka/memory-lcd-drm

Sharp モノクロ Memory LCD、JDI 8色 MIP パネル、AUO 64色 MIP パネルを含む、
Raspberry Pi 向け SPI メモリ液晶パネル用の Linux DRM/KMS カーネルドライバです。

<img src="docs/images/jdi_4.4inch.jpg" alt="地図を表示した JDI 4.4インチ MIP 液晶パネル">

JDI 4.4インチ MIP 液晶の表示例。

**動作動画・更新情報**

* [YouTube](https://www.youtube.com/channel/UCEk2Lxj_nTrZM3sOWQ39l6g):
  実機の動作動画を掲載しています。30 fps で動画を再生した実機の様子も確認できます。
* [X（@pi0bikecomputer）](https://x.com/pi0bikecomputer):
  Pi Zero Bikecomputer の更新情報を発信しています。

本リポジトリは実機での使いやすさを重視しています: 幅広いパネル対応、64色
パネルサポート、色拡張アルゴリズム、最適化された SPI 更新、そして同梱の
デバイスツリーオーバーレイと一致するピン配置ドキュメント。カーネルモジュール
とオーバーレイの名前は、既存のインストールスクリプトや Raspberry Pi の
`dtoverlay` 利用との互換性のため `sharp-drm` のままです。

## 主な特徴

モノクロから64色までのパネル対応に加え、表示品質と更新処理を改善しています。

* **幅広いパネル対応**: Sharp モノクロパネル、JDI 8色 MIP パネル、JDI
  4.4インチパネル、AUO 64色パネルを1つの DRM ドライバで扱います。
* **64色 MIP 対応**: AUO U340QBN01 を 272x451 のシングルパネルモードと、
  横に2枚並べて使う 544x451 のデュアルパネルモードをサポートします。
* **色拡張**: 8色/64色モードでは、順序ディザリングにより、8色で27色または125色、
  64色で343色または2197色相当の中間色を表現します。また、ブルーノイズディザリング、誤差拡散にも対応。
  これらの変換処理は、更新速度への影響をできるだけ抑えるよう最適化しています。
  [色数とディザリングの設定](#色数とディザリングの設定)で表示の違いを確認できます。
* **バックライト対応**: Linux 標準のバックライト制御機能で明るさを
  調整できます。明るさを変更したときの滑らかな切り替えにも対応します。
* **行単位の SPI トラフィック削減**: 更新を通知された範囲でも、表示内容が
  変わっていない行は送信を省き、SPI の通信量を減らします。
* **更新パイプライン**: SPI 転送中に次の画像の変換を進め、変換と転送を
  順番に行う場合に比べて、連続更新時の待ち時間を減らせます。
* **動画 DMA-BUF の直接入力**: デコード済み画像を表示用フレームバッファへ
  全画面コピーする処理を省けます。この経路を使う
  [FFmpeg パッチと再生手順](contrib/ffmpeg-rpi-isp/README_ja.md)を用意しています。
  アプリ側の対応方法は[動画再生](#動画再生)を参照してください。
* **ARM64 の画像変換高速化（NEON）**: NEON を使える64bit ARM 環境では、
  複数の画素をまとめて計算する CPU 機能で画像変換を高速化します。RGB 画像の
  2/8/64色への変換、順序ディザリング、64色のブルーノイズディザリングが
  対象です。NV12 動画では64色変換の一部に使い、誤差拡散には使いません。
  最終的な画面更新速度は SPI 転送にも制約されます。
* **ランタイム調整**: カラーモード、ディザリング、モノクロ閾値、表示反転、
  ブリンク、クリアを sysfs モジュールパラメータとして公開し、輝度は Linux
  標準のバックライトクラスで制御します。
* **ハードウェア設定のドキュメント化**: README に実際の Raspberry Pi
  ピン配置、`config.txt` の例、デュアルパネル配線の注意、ブザーなど他の
  PWM 利用者との共存ガイドを記載しています。

メインライン Linux 6.13 以降には `sharp-memory` DRM ドライバ
(`drivers/gpu/drm/tiny/sharp-memory.c`) が含まれますが、その対象はボードの
デバイスツリーで宣言されたモノクロ Sharp パネルです。本ドライバはそれに
対し、すぐ使える Raspberry Pi 環境を対象とします: JDI / AUO カラーパネル、
ディザリング、デュアルパネルモード、そしてカーネルを再ビルドせずに
`dtoverlay` で行う `config.txt` 設定です。

## 対応パネル

パネルを並べた外観比較です。画面の大きさや形状、接続端子の違いが分かります。

<img src="docs/images/panel-lineup.jpg" alt="大きさと形状の異なる4枚のメモリ液晶パネル">

左上: LS027B7DH01, 左下: LPM027M128B, 中央: U340QBN01, 右: LPM044M141A

購入リンクと販売状況は
[Pi Zero Bikecomputer のハードウェアガイド](https://github.com/hishizuka/pizero_bikecomputer/blob/master/doc/hardware_installation.md)
から転記しています。出品状況は変わるため、購入前にパネル型番とコネクタを
確認してください。

| ファミリ | パネル例 | モード | 入手元 |
| --- | --- | --- | --- |
| Sharp モノクロ | LS027B7DH01 | 400x240、2色 | [Adafruit Memory Display Breakout](https://www.adafruit.com/product/4694) |
| Sharp モノクロ | LS044Q7DH01 | 320x240、2色 | [Digikey](https://www.digikey.jp/ja/products/detail/sharp-microelectronics/LS044Q7DH01/5054070) |
| JDI 2.7インチ MIP | LPM027M128B / LPM027M128C | 400x240、2 / 8色 | [AliExpress パネル単体](https://www.aliexpress.com/item/1005002351792191.html)、[Wahoo wfcc4 (Roam V1)交換用アセンブリ](https://www.aliexpress.com/item/1005008210004927.html) |
| JDI 4.4インチ MIP | LPM044M141A | 640x480、2 / 8色 | [DigiKey](https://www.digikey.jp/ja/products/detail/azumo/12567-06-T3/10492348)（廃番） |
| AUO カラー MIP | U340QBN01 | 272x451、8 / 64色 | [Alibaba](https://www.alibaba.com/product-detail/3-4-Inch-TFT-LCD-Display_1601438380650.html)、[Youritech](https://youritech-online.com/products/u340qbn01-0-with-front-light-3-4-inch-sunlight-readable-lcd-display-272x451-reflective-spi-sunlight-readable-mip-display?variant=52037710479669)、Bryton Rider S800 |
| AUO 64色 MIP x2 | U340QBN01 x2 | 544x451、64色 | [Alibaba](https://www.alibaba.com/product-detail/3-4-Inch-TFT-LCD-Display_1601438380650.html) または [Youritech](https://youritech-online.com/products/u340qbn01-0-with-front-light-3-4-inch-sunlight-readable-lcd-display-272x451-reflective-spi-sunlight-readable-mip-display?variant=52037710479669) から U340QBN01 を2枚 |

パネルごとの設定例は[パネル別の設定](#パネル別の設定)を参照してください。

U340QBN01 をデュアルパネルモードで使うときは `colors=64` が強制されます。

### 実機表示

AUO U340QBN01 (Bryton Rider S800) の64色表示例。272x451のパネルで3D地図を表示しています。

<img src="docs/images/auo-u340qbn01-64color.jpg" alt="縦型ケースに組み込んだ AUO U340QBN01 の64色表示">

同じパネルを横に2枚並べた、544x451のデュアルパネル表示です。

<img src="docs/images/dual-panel-544x451.jpg" alt="AUO U340QBN01 を2枚並べた544x451の64色表示">

デュアルパネル基板の裏面。Raspberry Pi Zero W と2枚のパネルを接続した
組み立て例です。写真では個々の信号配線までは確認できません。

<img src="docs/images/hardware-overview.jpg" alt="Raspberry Pi Zero W と2枚の AUO パネルを接続した基板の裏面">

## 接続とピン配置の変更

既存製品のパネルや接続基板を使う場合は、まず型番と信号の接続先を確認し、
以下の標準配線との差分をオーバーレイに反映してください。古いドライバが
新しいカーネルで使えなくなった場合も、対応パネルであれば移行を検討できます。
製品ごとの電源・信号極性・バックライト駆動回路の適合は別途確認が必要です。

### 標準の GPIO 対応表

デフォルトの `rpi` オーバーレイは Raspberry Pi の SPI0 と BCM GPIO
番号を使います。MISO/GPIO9 はディスプレイでは使わないため、SPI の
pinctrl 設定から意図的に外してあります。以下のピン配置表は `rpi`
オーバーレイ向けです。

| ディスプレイ信号 | デフォルト BCM GPIO | 40ピンヘッダ | 備考 |
| --- | --- | --- | --- |
| SPI0 MOSI / SI | GPIO10 | 19 | Raspberry Pi からパネルへのピクセルデータ |
| SPI0 SCLK / SCK | GPIO11 | 23 | SPI クロック |
| SPI0 CE0 / SCS | GPIO8 | 24 | 1枚目のパネルのチップセレクト、アクティブハイ (`spi-cs-high`) |
| SPI0 CE1 / SCS | GPIO7 | 26 | デュアルパネルの2枚目のチップセレクト（`dual_panel=1`） |
| EXTCOMIN / VCOM | GPIO24 | 18 | ドライバがソフトウェアでトグル。`panel_type=jdi` は 128 Hz、`panel_type=sharp_mono` は 1 Hz |
| DISP | GPIO25 | 22 | 表示イネーブル / 電源制御 |
| バックライト PWM | GPIO18 | 12 | 任意の PWM0 チャネル0出力。`backlight_pwm` などの `backlight_*` オーバーレイオプションで有効化 |
| 3V3 / GND | ボード電源ピン | 1 または 17 / 任意の GND | 電源配線はボード依存で、オーバーレイでは制御しない |

`dtoverlay` パラメータには、物理ヘッダのピン番号ではなく BCM GPIO 番号を
使ってください。

### ピンアサインの変更箇所

編集するファイルは [`overlays/sharp-drm-rpi.dts`](overlays/sharp-drm-rpi.dts) です。
バックライトは `config.txt` のパラメータで変更できます。それ以外は現在、
オーバーレイのソースを編集します。

| 変更したい信号 | 編集箇所 | 揃えて変更する内容 |
| --- | --- | --- |
| EXTCOMIN / VCOM | `fragment@2` の `sharp_pins`、`fragment@4` の `vcom-gpios` | `brcm,pins` の先頭と `vcom-gpios` の GPIO 番号 |
| DISP | `fragment@2` の `sharp_pins`、`fragment@4` の `disp-gpios` | `brcm,pins` の2番目と `disp-gpios` の GPIO 番号 |
| SPI のチップセレクト | `fragment@4` の `sharp_drm` の `reg`、ノード名、SPI コントローラ側の CS 設定 | `reg` は GPIO 番号ではなく CS の番号。CE0 は `0`、CE1 は `1` |
| SPI バス・MOSI・SCLK | `fragment@0` と `fragment@4` の `target`、`fragment@1` のピン設定 | 対象コントローラと、それに対応するピン機能。任意の GPIO への番号変更だけでは移動できません |
| バックライト PWM | `config.txt` の `backlight_pin`、`backlight_pwm_func`、`backlight_pwm_channel` | 対応するハードウェア PWM のピン・機能・チャネルを組み合わせる |

例えば、VCOM を GPIO23、DISP を GPIO22 に接続する場合は、次の3箇所を変更します。
これは編集例です。使用する基板で両ピンが空いていることを確認してください。

```dts
/* fragment@2: sharp_pins */
brcm,pins = <23 22>;

/* fragment@4: sharp_drm */
vcom-gpios = <&gpio 23 0>;
disp-gpios = <&gpio 22 0>;
```

`brcm,function = <1 1>` は出力設定のままにします。上の例の末尾の `0` は
GPIO の極性フラグです。接続基板の反転回路などを確認せず変更しないでください。
SPI の CS を変更する場合は、同じ CS を使う `spidev` や補助パネルとの競合も
解消する必要があります。デュアルパネルは標準で CE0 と CE1 の両方を使います。

### 変更したオーバーレイの反映

ピン配置だけの変更なら、カーネルモジュールの再ビルドは不要です。
リポジトリのルートでオーバーレイを再生成し、使用中の起動パーティションへ
インストールして再起動します。以下は `/boot/firmware/overlays` を使う環境の例です。
古い環境ではインストール先を `/boot/overlays` に読み替えてください。

```bash
make -B overlays/sharp-drm-rpi.dtbo
sudo install -m 0644 overlays/sharp-drm-rpi.dtbo /boot/firmware/overlays/sharp-drm.dtbo
sudo reboot
```

設定行は引き続き `dtoverlay=sharp-drm,...` を使います。再インストール時に
オーバーレイが上書きされるため、変更した `.dts` は保存しておいてください。
バックライトのピン候補とブザーなどとの PWM 共存設定は
[バックライト PWM](#バックライト-pwm)を参照してください。

## 対応 OS とカーネル

本ドライバは Raspberry Pi OS Trixie 以降 (Debian Trixie ベース) と
Raspberry Pi の 6.18 系カーネルを対象とします。主な開発・テスト環境は
Raspberry Pi Zero 2 W と Raspberry Pi 5 です。初代 Raspberry Pi Zero
（ARMv6、32bit）でも、同梱 FFmpeg による動画再生を検証しています。
検証条件と結果は[FFmpeg の説明](contrib/ffmpeg-rpi-isp/README_ja.md)を参照してください。

今回の公開時に確認した実機環境、導入・削除、表示処理の結果は
[v2.0.0 の検証記録](docs/validation/v2.0.0.md) を参照してください。

## ユーザーガイド

これは Linux カーネルにディスプレイデバイスを提供するカーネルデバイス
ドライバです。

[サンプル画像](samples/README_ja.md) に、400x240・640x480の横向き8色用
テスト画像と、元の272x451 AUO 64色用テスト画像を収録しています。

### クイックインストール

Raspberry Pi 上でソースを取得し、以降のコマンドはこのディレクトリで実行します。

```bash
sudo apt-get install git build-essential device-tree-compiler dkms
git clone https://github.com/hishizuka/memory-lcd-drm.git
cd memory-lcd-drm
uname -r
```

実行中のカーネルに合う image と headers の両メタパッケージを導入します。
これにより、カーネル更新時に DKMS の再ビルドに必要なヘッダも更新されます。
`uname -r` の末尾に合う行を1つ選んでください。

| カーネル名の末尾 | インストールコマンド |
| --- | --- |
| `rpt-rpi-2712` | `sudo apt-get install linux-image-rpi-2712 linux-headers-rpi-2712` |
| `rpt-rpi-v8` | `sudo apt-get install linux-image-rpi-v8 linux-headers-rpi-v8` |
| `rpt-rpi-v6`（初代 Pi Zero など） | `sudo apt-get install linux-image-rpi-v6 linux-headers-rpi-v6` |
| `rpt-rpi-v7` | `sudo apt-get install linux-image-rpi-v7 linux-headers-rpi-v7` |
| `rpt-rpi-v7l` | `sudo apt-get install linux-image-rpi-v7l linux-headers-rpi-v7l` |

カーネルとヘッダについては
[Raspberry Pi 公式ドキュメント](https://www.raspberrypi.com/documentation/computers/linux_kernel.html)も参照してください。
カーネルが更新された場合は再起動し、実行中のカーネル用のヘッダがあることを確認します。

```bash
ls /lib/modules/$(uname -r)/build/Makefile
```

ファイルが見つからない場合は、カーネルとヘッダの対応を確認してから先へ進んでください。

モジュールを DKMS に登録し、デバイスツリーオーバーレイをインストールします:

```bash
sudo make install_dkms
```

新しいカーネルと対応ヘッダがインストールされるたびに、DKMS が
`sharp-drm` を自動的に再ビルドしてインストールします。`make install_dkms` は
対象カーネルの既存 initramfs がある場合、その内容も更新します。1回限りの手動
インストールを行う場合は、従来どおり次を実行します:

```bash
make
sudo make install
```

パネル設定を `/boot/firmware/config.txt` に追記します (最近の
Raspberry Pi OS の場合。古いリリースでは `/boot/config.txt`)。

```ini
[all]
dtparam=spi=on
dtoverlay=sharp-drm,width=272,height=451,colors=64
```

上の例は AUO U340QBN01 の64色表示用です。
[パネル別の設定](#パネル別の設定)から自分のパネルに合った `dtoverlay` 行を選び、
再起動します。

```bash
sudo reboot
```

### インストールの確認

再起動後、モジュールがロードされ DRM デバイスが存在することを確認します:

```bash
lsmod | grep sharp_drm
dmesg | grep -i sharp
ls /dev/dri/
```

fbcon が新しいフレームバッファにバインドされると、カーネルコンソールが
パネルに表示されます。コンソールフォントは `/boot/firmware/cmdline.txt`
で変更できます。小さいパネルでは `fbcon=font:VGA8x8` が便利です。
表示されない場合は[パネルに何も表示されない場合](#パネルに何も表示されない場合)を
確認してください。

### パラメーターの設定場所と反映タイミング

パラメーターによって、設定する場所と変更が反映されるタイミングが異なります。

| パラメーター | config.txt | modprobe.d | sysfs | 備考 |
| --- | --- | --- | --- | --- |
| `panel_type` | ○ | — | 確認のみ | パネルの種類（`jdi` / `sharp_mono`） |
| `width` | ○ | — | — | パネルの横幅（画素数） |
| `height` | ○ | — | — | パネルの高さ（画素数） |
| `colors` | ○ | ○ ※1 | ○ | 表示色数（2 / 8 / 64色） |
| `spi_speed` | ○ | — | — | 1枚目（CE0）のSPIクロック上限。単体・デュアルパネル共通 |
| `spi_speed_secondary` | ○ | — | — | デュアルパネルの2枚目（CE1）のSPIクロック上限。`dual_panel=1` 時のみ使用 |
| `dual_panel` | ○ | — | — | デュアルパネルを有効化（272×451を2枚、CE0＋CE1） |
| `backlight_pwm` | ○ | — | — | バックライト用PWMを有効化 |
| `backlight_pin` | ○ | — | — | バックライトPWMに使うGPIO番号 |
| `backlight_pwm_func` | ○ | — | — | PWMピンの機能選択 |
| `backlight_pwm_channel` | ○ | — | — | バックライトのPWMチャネル |
| `backlight_pwm_period_ns` | ○ | — | — | PWMの周期（ナノ秒） |
| `backlight_default_brightness` | ○ | — | — | 起動時のバックライト輝度 |
| `cacheable_buffers` | — | ○ | 確認のみ | CPU描画用フレームバッファのキャッシュを有効化 |
| `dither_algo` | — | ○ | ○ | ディザリング方式 |
| `blue_noise_2197` | — | ○ | ○ | 64色・2197色相当モードのブルーノイズを有効化 |
| `mono_cutoff` | — | ○ | ○ | モノクロの白黒判定・ディザの明るさ調整 |
| `mono_invert` | — | ○ | ○ | モノクロ変換後の白黒を反転 |
| `perf_stats` | — | ○ | ○ | 性能計測を有効化 |
| `display_clear` | — | — | ○ | 画面を黒でクリアする操作 |
| `display_blink` | — | — | ○ | 黒または白の画面で点滅 |
| `display_invert` | — | — | ○ | 表示色の反転・反転点滅 |
| `brightness` | — | — | ○ ※2 | バックライトの目標輝度 |
| `bl_power` | — | — | ○ ※2 | バックライトの点灯・消灯 |
| `actual_brightness` | — | — | 確認のみ ※2 | 現在のバックライト輝度 |
| `max_brightness` | — | — | 確認のみ ※2 | 指定できるバックライト輝度の上限 |

○：指定・変更可能、—：この場所では設定しません。「確認のみ」は読み取り専用です。

※1 `colors` はモジュール引数でも受け付けますが、パネル検出時にオーバーレイの値で
上書きされるため、起動時の色数は `config.txt` で指定してください。

※2 バックライトの4項目は `/sys/class/backlight/backlight/` にあります。
それ以外のsysfs項目は `/sys/module/sharp_drm/parameters/` にあります。
`display_clear`・`display_blink`・`display_invert` は、パネルの初期化後に行う表示操作です。

* [`config.txt`](#configtxt-で指定するパラメーター)：再起動後に反映します。
* [`modprobe.d`](#モジュール読み込み時に指定するパラメーター)：`/etc/modprobe.d/*.conf` の
  `options sharp_drm ...` で指定し、次回のモジュール読み込み時に反映します。
  `cacheable_buffers` の設定例では `sharp-drm-cacheable.conf` を使います。
* [sysfs](#sysfs-で変更できるパラメーター)：実行中に変更できます。
  表示への反映は次の更新時など、項目によって異なります。書き込みだけでは永続化されません。

### config.txt で指定するパラメーター

Raspberry Pi では `/boot/firmware/config.txt`（旧環境は `/boot/config.txt`）の
`dtoverlay=sharp-drm,...` と `dtparam=...` で指定します。変更後は再起動してください。

`make install` / `make install_dkms` はモジュールとオーバーレイをインストールしますが、
パネル用の `config.txt` 設定は書き換えません。適切な行を手動で追加します。

解像度と色深度は `dtoverlay` パラメータで選択します:

```ini
dtoverlay=sharp-drm,width=400,height=240,colors=8
```

デフォルトは `panel_type=jdi`、`width=400`、`height=240`、`colors=8` です。
対応解像度は `272x451`、`320x240`、`400x240`、`640x480` です。
モノクロ Sharp パネルでは `panel_type=sharp_mono` を指定してください。
この場合 `colors=2` が強制されます。

JDI LPM027M128B、LPM027M128C、LPM044M141A では `colors=2` または
`colors=8` を指定できます。U340QBN01 はシングルパネルモードで
`colors=8` と `colors=64` に対応し、`colors=64` で6ビットカラーモードが
有効になります。

#### パネル別の設定

各行の設定は候補です。使用するパネルに合う設定だけを `config.txt` に記載します。

| パネル | `config.txt` 行 |
| --- | --- |
| LS027B7DH01 | `dtoverlay=sharp-drm,panel_type=sharp_mono,width=400,height=240,colors=2` |
| LS044Q7DH01 | `dtoverlay=sharp-drm,panel_type=sharp_mono,width=320,height=240,colors=2` |
| LPM027M128B / LPM027M128C | `dtoverlay=sharp-drm,width=400,height=240,colors=2` または `dtoverlay=sharp-drm,width=400,height=240,colors=8` |
| LPM044M141A | `dtoverlay=sharp-drm,width=640,height=480,colors=2` または `dtoverlay=sharp-drm,width=640,height=480,colors=8` |
| U340QBN01 | `dtoverlay=sharp-drm,width=272,height=451,colors=8` または `dtoverlay=sharp-drm,width=272,height=451,colors=64` |
| U340QBN01 x2 | `dtoverlay=sharp-drm,width=272,height=451,colors=64,dual_panel=1` |

#### SPI 速度と対象プラットフォーム

`spi_speed` でプライマリパネルの SPI 最大周波数を Hz 単位で指定できます
（既定値は `4000000`）。7 MHz 対応の 272x451 U340QBN01 パネルでは、
例えば次のように指定します:

```ini
dtoverlay=sharp-drm,width=272,height=451,colors=64,spi_speed=7000000
```

`dual_panel=1` のデュアルパネル構成では、1枚目（CE0）を `spi_speed`、
2枚目（CE1）を `spi_speed_secondary` で個別に指定します。
`spi_speed_secondary` の既定値は `4000000` です。両方が 7 MHz 対応なら両方の
引数を指定してください。これらは SPI クロックの上限であり、実際のクロックは
コントローラ側で低くなる場合があります。
Raspberry Pi の `config.txt` は1行98文字を超えた部分を無視します。
引数が長い場合は、次の `dtoverlay=` 行より前に `dtparam=` 行を追加してください。

ビルド時のデフォルトは Raspberry Pi 用オーバーレイです:

```bash
make PLATFORM=rpi
```

オーバーレイは `sharp-drm.dtbo` としてインストールされます。

### バックライト PWM

デフォルトでは、`sharp-drm` オーバーレイは PWM チャネルを一切確保せず、
GPIO18 も設定しません。ディスプレイにバックライトがない場合は
`dtoverlay` 行をそのままにしてこの節を読み飛ばし、`backlight_*`
オプションを追加しないでください。

サポートする Raspberry Pi 構成は、PWM0 が GPIO18 のディスプレイ
バックライト専用であることを前提とします。アナログオーディオも PWM
ブロックを使うため、`dtparam=audio=off` で無効化してください。以下の
パターンから選びます。

**PWM0 のみ (ディスプレイバックライト)**

バックライトだけが PWM を使う場合は、`backlight_pwm` でドライバ管理の
バックライト PWM を有効にします。`sharp-drm` オーバーレイが PWM0 と
GPIO18 のピンマルチプレクサを設定するため、別途 `pwm` オーバーレイを
追加しないでください:

```ini
dtparam=spi=on
dtparam=audio=off

[all]
dtoverlay=sharp-drm,width=400,height=240,colors=8,backlight_pwm
```

**PWM0 (ディスプレイバックライト) + PWM1 (別のデバイス)**

PWM1 も必要な場合は `pwm-2chan` を使います。この2行のオーバーレイの
相対順序が重要です: `sharp-drm` を先、`pwm-2chan` を後にすることで、
最終的な PWM ピンマルチプレクサに GPIO18/PWM0 と GPIO13/PWM1 の両方が
含まれます。順序を逆にすると、GPIO13 の PWM1 ピンマルチプレクサが失われる
ことがあります:

```ini
dtparam=spi=on
dtparam=audio=off

[all]
dtoverlay=sharp-drm,width=400,height=240,colors=8,backlight_pwm
dtoverlay=pwm-2chan,pin=18,func=2,pin2=13,func2=4
```

`pwm` や `pwm-2chan` を有効にするだけでは、PWM0 は `sharp-drm` ドライバに
接続されません。ドライバにバックライトを制御させる場合は、以下の
`backlight_*` オプションのいずれかを `sharp-drm` 行に必ず指定して
ください。

これらのオプションはどれか1つでバックライト PWM フラグメントが有効になる
ため、`backlight_pin=18` だけでも `backlight_pwm` と同じ意味になります:

* `backlight_pwm`: GPIO18 / PWM0 チャネル0でドライバ管理の PWM
  バックライトを有効化
* `backlight_pin`: ハードウェア PWM に使う BCM GPIO 番号
* `backlight_pwm_func`: 選択した PWM ピンの Raspberry Pi
  ピンマルチプレクサ機能
* `backlight_pwm_channel`: `pwms` プロパティ内の PWM チャネル
* `backlight_pwm_period_ns`: PWM 周期 (ナノ秒)
* `backlight_default_brightness`: 起動時輝度 (`0`〜`255`、既定値 `255`)。
  起動時から消灯する場合は `0`

よく使う Raspberry Pi PWM ピンの例:

| バックライトピン | PWM マッピング | オーバーレイオプション |
| --- | --- | --- |
| GPIO18 | PWM0 チャネル0、ALT5 | `backlight_pwm` |
| GPIO19 | PWM1 チャネル1、ALT5 | `backlight_pin=19,backlight_pwm_channel=1` |
| GPIO12 | PWM0 チャネル0、ALT0 | `backlight_pin=12,backlight_pwm_func=4` |
| GPIO13 | PWM1 チャネル1、ALT0 | `backlight_pin=13,backlight_pwm_func=4,backlight_pwm_channel=1` |

たとえば、U340QBN01 のバックライトを GPIO18 で使用し、ユーザー空間が
輝度を設定するまで消灯したまま起動し、SPI の上限を 7 MHz にする場合は、
次のように指定します:

```ini
dtoverlay=sharp-drm,width=272,height=451,colors=64
dtparam=backlight_pwm,backlight_default_brightness=0,spi_speed=7000000
```

輝度は実行時に、Linux 標準のバックライトクラス
`/sys/class/backlight/backlight/` で制御します （[sysfs で変更できるパラメーター](#sysfs-で変更できるパラメーター)を参照）。

### デュアルパネルモード (CE0 + CE1、272x451 のみ)

272x451 パネル2枚を横に並べ、単一の論理 544x451 ディスプレイとして
使えます:

```ini
dtoverlay=sharp-drm,width=272,height=451,colors=64,dual_panel=1
dtparam=spi_speed=7000000,spi_speed_secondary=7000000
```

`dual_panel` は `colors=64` を強制し、64色 AUO パネル向けです。
デュアルパネルモードでは `panel_type=sharp_mono` はサポートされません。

デュアルパネルの1枚目は CE0/CS0、2枚目は CE1/CS1 を使います。
2枚目のSPIクロック上限は `spi_speed_secondary` で指定します。
2枚目を別のチップセレクトへ変更するオプションは、現在のオーバーレイにはありません。

### モジュール読み込み時に指定するパラメーター

`cacheable_buffers` は、ドライバが確保するフレームバッファをCPUキャッシュに
載せる設定です。既定値は `0`（無効）。CPUだけで描画する場合に、画像変換時の
読み取りを高速化できます。このバッファを外部へ共有してGPUなどからDMAで
書き込ませる場合は、無効のままにしてください。外部から取り込むDMA-BUFには
適用されず、共有元のマッピング属性を維持します。

有効にする場合は `/etc/modprobe.d/sharp-drm-cacheable.conf` に次を記載します。
ファイル名は一例です。同じ設定を複数の `.conf` ファイルに重複させないでください。

```conf
options sharp_drm cacheable_buffers=1
```

`make install` / `make install_dkms` はこの設定ファイルを作成しません。
**`config.txt` での指定と、sysfsからの変更には対応していません。**
変更は次回のモジュール読み込み時に反映されるため、通常は設定後に再起動します。
initramfsからモジュールを読み込む構成では、再起動前に設定をinitramfsにも反映します。
Raspberry Pi OSでは次のコマンドを使います。

```bash
sudo update-initramfs -u -k "$(uname -r)"
```

再起動後の確認（`Y` が有効、`N` が無効）:

```bash
cat /sys/module/sharp_drm/parameters/cacheable_buffers
```

### sysfs で変更できるパラメーター

以下の書き換え可能な項目は、モジュールを読み込み直さずに変更できます。
`/sys/module/sharp_drm/parameters/` 内の対象ファイルへ値を書き込みます。
バックライトの輝度は別の `/sys/class/backlight/backlight/` で制御します。
sysfsへの書き込みだけでは設定は永続化されません。

```bash
echo <setting> | sudo tee /sys/module/sharp_drm/parameters/<param>
```

#### 基本パラメーター

* `colors`: `2` モノクロ、`8` 3ビットカラー、`64` 6ビットカラー。
  `config.txt` で指定した起動時の色数を、パネルが対応する範囲で変更します
* `perf_stats`: `0` 性能計測を無効化（デフォルト）、`1` で有効化

`panel_type` は接続中のパネル種別を確認するための読み取り専用項目です。
変更する場合は `config.txt` の設定を変更して再起動します。

```bash
cat /sys/module/sharp_drm/parameters/panel_type
```

ドライバのアンロード時は常に画面をクリアして電源を切ります (DISP を
Low に)。`auto_clear` パラメータはありません。

#### 色数とディザリングの設定

`colors` はsysfsから動的に変更できます。変更後の最初の画面更新では、確保済みの
バッファを使って新しい色数で全行を変換し、SPIの転送サイズも切り替えます。
書き込みだけでは再描画を開始しないため、表示アプリからの次の更新を待つか、
再描画を要求してください。フレームバッファの解像度やメモリ確保量は変わりません。
起動時の色数にかかわらず、変換・比較・送信用のバッファは64色の行データも扱える
容量で確保するため、2色から8色、8色から64色への変更でも再確保は不要です。

**色数を下げるとSPI転送量を減らせます。** 例えば272画素幅の1行は、64色で208バイト、
8色で104バイト、2色で36バイトです（行ヘッダー込み、転送末尾のデータは別）。
同じ行数の更新なら、64色から8色で半分、2色で約17%になります。
SPI転送がボトルネックの場合に更新時間の短縮が期待できますが、変換処理や
差分行数も影響するため、表示速度がそのまま同じ倍率で上がるわけではありません。
ディザリングで見かけの色数を増やしても、同じ `colors` なら1行の転送量は変わりません。

使用するパネルが対応する色数を選んでください。`sharp_mono` は2色、
デュアルパネルは64色に固定され、他の色数への変更は拒否されます。

![3D地図を使った、原画像・2色・8色・64色とディザリングの比較](docs/images/rendering-modes.png)

同じ272x451の原画像を、本ドライバの変換処理で描画した比較です。
上から「原画像とモノクロ」「8色」「64色」の行に分け、同じ色数での方式の違いを横に並べています。
単純二値化のモノクロ画像では、この地図で地形と道路が残るように調整した
`mono_cutoff=188` を使用しています。モノクロの順序ディザ画像は
`dither_algo=1`、中立の `mono_cutoff=128` です（ドライバの既定値は `32`）。
27・125・343・2197色はディザリングによる見かけの色数で、画素ごとの色数は
8色または64色です。実機写真ではなく、変換結果を RGB 画像として示しています。
[原寸画像](docs/images/rendering-modes.png)を開くとディザの違いを確認できます。
地図画像: Pi Zero Bikecomputer、© [OpenStreetMap contributors](https://www.openstreetmap.org/copyright)。
標高データ: 国土地理院（加工して使用）。

`dither_algo` は、パネルの色数に収まらない中間色をどう表現するかを選ぶ設定です。

| 値 | 方式 | 表示の特徴 |
| --- | --- | --- |
| `0` | ディザリングなし | 各画素をパネルが出せる色へ変換。中間色の境界が段階的になります |
| `1` | モノクロ用の順序ディザリング | 4×4の Bayer パターンで、白黒の画素から中間の灰色を表現 |
| `2` / `3` | 8色用の順序ディザリング | 規則的な画素の並びで27色 / 125色相当の中間色を表現 |
| `4` / `5` | 64色用の順序ディザリング | 規則的な画素の並びで343色 / 2197色相当の中間色を表現 |
| `6`（`errdiff2`） | 誤差拡散 | 色変換で生じた誤差を周辺の未処理画素へ配り、中間色を表現。8色・64色の両方に対応 |

ブルーノイズは別の `dither_algo` 番号ではありません。`colors=64`、
`dither_algo=5` に加えて `blue_noise_2197=1` を指定すると、規則的な並びの
代わりに、点が不規則に分散したブルーノイズのパターンを使い、規則的な縞や
格子模様を目立ちにくくします。8色モードでは使いません。
実装では固定の32×32のしきい値表を画素の座標で参照します。パターンは二次元ですが、
周囲の画素の計算結果には依存せず、1行ずつ独立して変換できます。

誤差拡散は、各画素をパネルで出せる色に変えたときの誤差を、これから処理する
画素に加えて補う方法です。本実装の `errdiff2` は Floyd–Steinberg 型で、
行ごとに走査方向を反転します。同じ行の進行方向側の1画素へ誤差の7/16、
次の行の3画素へ3/16・5/16・1/16を配ります。
保持する誤差は現在行と次行の2行分ですが、前の行から受け取った誤差が
さらに次の行へ伝わります。上下を含む3行だけで完結する方式ではありません。

誤差拡散の考え方は8色・64色で共通ですが、8色では RGB 各成分が2段階、
64色では4段階になるため、分配する誤差と画素の並びが変わります。
64色の方が元の色に近い値を選べますが、見え方は画像によっても異なります。
誤差拡散を「何色相当」と固定した色数で表すことはできません。
比較画像には、64色ブルーノイズと8色・64色それぞれの誤差拡散も収録しています。

番号変更前の設定を使っている場合は、モノクロ順序ディザの旧 `6` を `1` に、
旧 `1〜5` をそれぞれ `2〜6` に変更してください（`0` は変更なし）。
起動設定やアプリから数値を指定している場合も更新が必要です。

利用できる組み合わせと `dither_algo` の設定値は次のとおりです。

| `colors` | `0` off | `1` mono ordered | `2` 27colors | `3` 125colors | `4` 343colors | `5` 2197colors | `6` errdiff2 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `2` (モノクロ) | ✅* | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ |
| `8` (3ビット) | ✅ | ❌ | ✅* | ✅ | ❌ | ❌ | ✅ |
| `64` (6ビット) | ✅ | ❌ | ❌ | ❌ | ✅* | ✅ | ✅ |

✅\* はその色数の既定値です。`colors` に書き込むと `dither_algo` は
その色数の既定値へ戻るため、`colors` → `dither_algo` の順で設定します。
未対応の組み合わせ（❌）はディザなしで変換します。8色・64色モードで
`dither_algo=1` を指定した場合も同様です。

* `dither_algo`: 表からディザリングアルゴリズムを選択
* `blue_noise_2197`: `0` 無効 (デフォルト)、`1` 有効。
  `colors=64` かつ `dither_algo=5` のときに使用します。

計算負荷と部分更新への影響は次のとおりです。

| 方式 | 計算負荷と更新範囲 |
| --- | --- |
| 順序ディザ | 小さなしきい値表を参照して色を選びます。変更された行だけを変換できます |
| モノクロ順序ディザ | 1行ごとに4個の繰り返ししきい値を求め、RGB の加重輝度または NV12 の輝度と比較します。各行は独立し、XRGB8888 は単純二値化と同じ条件で NEON を使えます |
| ブルーノイズ | 画素ごとに32×32の表を参照します。誤差の蓄積・分配は不要で、順序ディザと同様に変更された行だけを変換できます |
| 誤差拡散 | RGB 各成分の誤差を蓄積・分配する計算が加わります。画素を順番に処理する必要があり、本実装では NEON を使いません。部分更新でも対象パネルの全行を変換します |

いずれも処理する画素数に比例した計算量ですが、1画素あたりの処理と部分更新時の
変換範囲が異なります。速度差は環境と画像に依存するため、一定の倍率では示していません。
全行を変換した場合も、SPI へ送るのは比較の結果、更新が必要な行です。

設定例（対応するパネルで実行）:

```bash
# 8-color error diffusion
echo 8 | sudo tee /sys/module/sharp_drm/parameters/colors
echo 6 | sudo tee /sys/module/sharp_drm/parameters/dither_algo

# 64-color error diffusion
echo 64 | sudo tee /sys/module/sharp_drm/parameters/colors
echo 6 | sudo tee /sys/module/sharp_drm/parameters/dither_algo

# 64-color blue noise
echo 64 | sudo tee /sys/module/sharp_drm/parameters/colors
echo 5 | sudo tee /sys/module/sharp_drm/parameters/dither_algo
echo 1 | sudo tee /sys/module/sharp_drm/parameters/blue_noise_2197
```

2197色の順序ディザに戻す場合は `blue_noise_2197` を `0` にします。

#### モノクロ表示

* `mono_cutoff`: `0`〜`255` のグレースケール閾値（デフォルト `32`）。
  `dither_algo=1` ではディザのしきい値の中心を移動します
* `mono_invert`: `0` 反転なし、`1` 変換後の白黒画素を反転

`colors=2` の既定は `dither_algo=0` で、従来の単純二値化を維持します。
モノクロの順序ディザは次のように有効化します。

```bash
# Monochrome ordered dithering, neutral brightness
echo 2 | sudo tee /sys/module/sharp_drm/parameters/colors
echo 128 | sudo tee /sys/module/sharp_drm/parameters/mono_cutoff
echo 1 | sudo tee /sys/module/sharp_drm/parameters/dither_algo
```

`mono_cutoff=128` では、4×4の Bayer パターンの16画素に8・24・…・248の
しきい値を割り当てます。均一な灰色は16画素中の白の数を0〜16個に変えて表し、
灰度128では8個が白になります。`mono_cutoff` を上げると暗く、下げると明るくなります。
各しきい値を `mono_cutoff - 128` だけ移動し、1〜255に収めるため、
`mono_invert` 適用前の純黒・純白は維持します。単純二値化とは異なり、
ディザ有効時は cutoff 0でも純黒は黒のままです。
XRGB8888 は従来の加重輝度を使い、リミテッドレンジの NV12 は輝度を直接使って、
しきい値を16〜235の範囲へ換算します。

パターンは画素の座標に固定され、部分更新でも位置がずれず、変更された行だけを
変換します。規則的な模様が現れ、細い文字の読みやすさが下がる場合があります。
単純二値化に戻す場合は `dither_algo=0` を選びます。
`blue_noise_2197` はモノクロでは使いません。モノクロの誤差拡散は未実装で、
`colors=2` での値2〜6は単純二値化として動作します。

#### 画面のクリア・点滅・反転

* `display_clear`: `1` を書き込むと、アクティブなフレームバッファを黒で
  塗りつぶして再描画（XRGB8888 とリミテッドレンジの NV12 に対応）
* `display_blink`: `0` 無効、`1` 黒ブリンク、`2` 白ブリンク
* `display_invert`: `0` 通常、`1` パネルの表示色を反転

表示パイプが無効の間、表示コマンドは `ESHUTDOWN` を返します。

#### バックライトの明るさ

`backlight_*` オーバーレイオプションで PWM を有効にすると、ドライバは
`backlight` という名前の Linux 標準バックライトデバイスを登録します。

ユーザー空間が明示的に輝度を設定するまで起動時から消灯するには、次のように
指定します:

```ini
dtoverlay=sharp-drm,width=272,height=451,colors=64,backlight_pwm,backlight_default_brightness=0
```
sysfs バックライトクラス ABI に従い、有効な輝度値は `0` から
`max_brightness` が報告する値までです。本ドライバは `255` を報告し、
デフォルトは最大輝度です。

実行中の輝度変更は、約10ms周期で PWM を更新しながら300msかけて
smoothstep（`3t^2 - 2t^3`）で遷移します。遷移中に別の輝度が指定された
場合は、現在適用中の明るさから新しい遷移を始めます。起動時の最初の100%
設定と、安全のための電源オフ処理だけは即時に反映します。

例:

```bash
echo 128 | sudo tee /sys/class/backlight/backlight/brightness
echo 4 | sudo tee /sys/class/backlight/backlight/bl_power  # off
echo 0 | sudo tee /sys/class/backlight/backlight/bl_power  # on
cat /sys/class/backlight/backlight/actual_brightness
cat /sys/class/backlight/backlight/max_brightness
```

#### 一般ユーザーからの設定変更

`sudo make install` は、モジュールパラメータとバックライト制御用の udev
ルールもインストールします。root 以外のユーザーから更新できるようにするには、
ユーザーを設定済みグループに追加します。再起動せずにルールを反映する場合は、
ルールの再読み込みと適用も行います。グループへの追加後はログインし直してください:

```bash
sudo usermod -aG video <user>
sudo udevadm control --reload-rules
sudo udevadm trigger --action=add --subsystem-match=module --sysname-match=sharp_drm
sudo udevadm trigger --action=add --subsystem-match=backlight --sysname-match=backlight
```

### 動画再生

再生アプリ側で、画像を fbdev に書き込む処理を、
ハードウェアデコーダーの出力バッファを DRM へ直接渡す処理に変更すると、
表示用フレームバッファへの全画面コピーを省けます。デコード済み画像の
メモリをそのままフレームバッファとして使うために、Linux のメモリ共有機能
（DMA-BUF）を利用します。同梱のパッチを適用した FFmpeg では、
ハードウェアデコードと DRM 出力（`-f vout_drm -drm_module sharp_drm`）を
組み合わせて利用します。
[FFmpeg パッチとビルド・再生手順](contrib/ffmpeg-rpi-isp/README_ja.md)を用意しています。
RGB 画像（XRGB8888）と動画で使われる NV12 形式に対応し、パネル用の色変換と
SPI 送信はドライバが行います。
この FFmpeg を使った Raspberry Pi Zero での検証では、動画再生の CPU 負荷を
大幅に削減できることを確認しています。

### 任意の初期化サービス・困ったとき

#### 起動時の初期化

パネル設定と再起動を済ませ、DRM デバイスが認識された後に実行してください。
インストール時にもサービスが起動するため、パネルが未設定だと初期化待ちになります。

起動時に fbcon と Sharp framebuffer を自動初期化する場合は、任意の
初期化サービスをインストールします:

```bash
sudo make install_redraw_service
systemctl status mip-redraw.service
```

このサービスは fbcon をバインドし、Sharp framebuffer を赤で塗りつぶし、
標準 DRM dirty 通知を1回発行して終了します。初期化状態はプロセス終了後も
有効なので、`systemctl status` は `active (exited)` と表示されます。

初期化後も mmap ベースの fbdev 書込みが途切れる環境では、同じ補助
プログラムを30 Hzの行変更監視ブリッジとして実行できます:

```bash
sudo /usr/local/libexec/sharp-drm/mip-redraw auto 30 --initialize-red
```

#### パネルに何も表示されない場合

* 有効な `config.txt` に `dtparam=spi=on` と `dtoverlay=sharp-drm,...`
  行があることを確認する。
* オーバーレイがインストールされていることを確認する:
  `ls /boot/firmware/overlays/sharp-drm.dtbo` (古いリリースでは
  `/boot/overlays/`)。
* モジュールが実行中のカーネル向けにビルドされていることを確認する:
  `modinfo sharp-drm | head`。
* [標準の GPIO 対応表](#標準の-gpio-対応表)と配線を見直す。特に SCS (チップセレクト、
  アクティブハイ) と DISP。

### アンインストール

初期化サービスを導入している場合は、先に削除します。

```bash
sudo make uninstall_redraw_service
```

DKMS で導入した場合は登録とモジュールを削除し、続けて共通の設定を削除します。

```bash
sudo make uninstall_dkms
sudo make uninstall
```

手動で `make install` した場合は `sudo make uninstall` を実行します。
`make uninstall_dkms` は対象カーネルの既存 initramfs も更新します。
このターゲットは設定とオーバーレイを削除しますが、手動インストールした
カーネルモジュールのファイル自体は削除しません。

これにより、オーバーレイディレクトリから `sharp-drm.dtbo` が削除され、
`/etc/modules` から `sharp-drm` 行が、`config.txt` から
`dtoverlay=sharp-drm` 行が取り除かれます。編集されたファイルは `.save`
サフィックス付きでバックアップされます。再起動するとディスプレイが
解放されます。分割して追加した `dtparam=backlight_*` や
`dtparam=spi_speed*` などは自動削除されないため、`config.txt` を確認して
本ドライバ専用の設定を取り除いてください。

## 開発者向けリファレンス

### ソース構成

ドライバは役割ごとに小さなモジュールへ分割されています:

* `src/main.c`: SPI ドライバ登録と、プライマリ/補助デバイスの
  probe・remove・shutdown の接続
* `src/sharp_drm.h`: 内部デバイス構造体、ロック規則、ファイル間 API
* `src/kms.c`: DRM/KMS セットアップ、コネクタ/パイプのコールバック、
  probe/remove/shutdown、パネルの電源シーケンス
* `src/registry.c`: デバイス登録管理、パネルの色数制約の適用と公開
* `src/fb_update.c`: damage 処理、変換結果の送信パイプラインへの投入、
  フレームバッファのクリア、プライマリ plane の再描画
* `src/control.c`: 表示コマンドと反転点滅、work の受付・キャンセル
* `src/backlight.c`: 任意の PWM バックライト登録と輝度の遷移
* `src/render.c`: 2/8/64色モードのフレームバッファクリッピング、色変換、
  ディザリング
* `src/render_internal.h`: scalar/NEON 間で共有するレンダリング定数と内部 API
* `src/render_neon.c`: 対応モード向けの AArch64 NEON 実装
* `src/spi_io.c`: controller 上限を考慮したタグ付き完全行の SPI
  メッセージ構築
* `src/tx.c`: desired/displayed 行状態、世代番号、2個の TX slot、ordered
  SPI worker
* `src/perf.c`: debugfs の性能・パイプラインカウンタ
* `src/params.c`: `/sys/module/sharp_drm/parameters/` の sysfs
  モジュールパラメータ
* `src/ioctl_iface.h`: 再描画 ioctl 番号のマクロ

デバイスツリーオーバーレイのソースは `overlays/` に、プラットフォーム
ごとに1ファイル (`overlays/sharp-drm-<platform>.dts`) で置かれています。
ビルド対象は make 変数 `PLATFORM` で選択し、デフォルトは `rpi` です。
インストールされるオーバーレイは常に `sharp-drm.dtbo` という名前なので、
`dtoverlay=sharp-drm` はそのまま使えます。対応する `PLATFORM` の値は
現在は `rpi` のみです。

### ビルドとテスト

よく使う開発コマンド:

```bash
make               # Default: PLATFORM=rpi
sudo make install
sudo make uninstall
make clean
```

初回インストールは、上の「クイックインストール」節を参照してください。

実機を使わない変換・送信管理などのテストは、次で実行できます。
ARM64 環境では NEON の変換結果も検証します。

```bash
make -C tests check
```

必要な環境、各テストの目的、個別実行、生成物の削除、`golden.txt` の更新手順は
[テストガイド](tests/README_ja.md)を参照してください。

実機で DRM の有効化・再描画・DMA-BUF 入力などを確認する補助プログラムは
`scripts/drm_smoke.c` にあります。

### 更新パイプライン

SPI で行データを送っている間に、CPU は次の更新の画像変換を進められます。

更新パスは `sharp_drm_pipe_update()` -> `sharp_drm_fb_dirty()` ->
`sharp_render_clip()` -> `sharp_tx_store_converted_locked()` -> ordered TX
worker -> `sharp_spi_write_tagged_batch()` です。これらのパネルは任意の
X 座標範囲や個別画素だけを更新できません。変更通知（damage）が1行の一部だけでも、
そのパネル行の全幅を変換し、差分があれば完全な1行として送信します。

デュアルパネルでは、damage の X 座標から対象パネルを選んでから完全行へ
展開します。shadow が無効な場合や変換設定を変更した場合は両パネルを
全面更新します。ARM64 の対応環境では NV12 の64色変換にも NEON の量子化・
パッキング処理を使い、誤差拡散は scalar 処理を使います。

`desired` は最新の変換済み行を保持し、`displayed` は SPI 送信が成功した後に
だけ更新します。送信中に新しく更新された行や、送信に失敗した行は、行ごとの
世代番号によって dirty のまま残ります。2個の固定 TX slot は
`spi_max_transfer_size()` と `spi_max_message_size()` から大きさを決め
（取得できない場合は保守的な 65535 byte を使用）、すべての message 境界を
タグ付き完全行へ揃えます。SPI I/O 中は `fb_lock` を保持しないため、次の更新の
変換と SPI コントローラの転送を並行できます。表示制御コマンドは、
キューからの新たな行送信を止め、進行中の転送が完了してから送信します。

送信開始行をバッチごとに巡回させ、送信より速い連続更新でも下側の行が
取り残されないようにします。

外部から取り込んだ DMA-BUF の更新では、TX slot 2個が使用中なら DRM 更新を
待機させます。コミット済みの更新を捨てず、フレームバッファを必要な期間保持し、
DRM のアトミック更新の規則を保ちます。同梱パッチを適用した FFmpeg は、
送信待ちの画像を最大1枚だけ保持します。ドライバ側の空きを待つ間に新しい画像が
届けば、まだ DRM へ渡していない古い画像を置き換え、待ち行列の増大を防ぎます。
パネルへの送信単位は、この経路でも完全な1行です。

初代 Raspberry Pi Zero 向け FFmpeg では、デコーダー側で BGR4 形式に変換した
DMA-BUF を渡す経路を推奨しています。経路と性能の検証結果は
[FFmpeg の説明](contrib/ffmpeg-rpi-isp/README_ja.md)を参照してください。

### 性能カウンタ

debugfs がマウント済みなら、累積カウンタを
`/sys/kernel/debug/sharp_drm/stats` で確認できます:

```bash
sudo mount -t debugfs none /sys/kernel/debug  # Only if not mounted
echo enable | sudo tee /sys/kernel/debug/sharp_drm/stats
cat /sys/kernel/debug/sharp_drm/stats
echo reset | sudo tee /sys/kernel/debug/sharp_drm/stats
```

通常の更新処理への負荷を抑えるため、計測はデフォルトで無効です。再び無効に
するには `disable` を書き込みます。次の情報を出力します。

* 更新遅延の p50/p95/p99（対数ヒストグラムによる概算）
* 変換・比較・送信データの組み立て・待ち行列・SPI 転送にかかった時間
* 更新候補・送信済み・送信省略の行数、送信バイト数とメッセージ数
* 送信バッファの使用状況と空き不足、更新の集約、取り込んだ DMA-BUF の待機
* 全画面の再生成と SPI エラー

2つのビルドを比較するときは、それぞれ同じ処理を開始する直前に `reset` して
計測します。カウンタに加え、Pi の機種、カーネル、CPU の周波数制御方式、温度、
速度制限の状態、モジュールパラメータ、デバイスツリーの SPI 周波数を
保存するには、次を実行します。

```bash
scripts/collect_perf.sh result.txt -- your-workload-command
```

同じ行更新を再現できるフレームバッファのテストには、次を使います:

```bash
cc -O2 -Wall -Wextra -o /tmp/drm_dirty scripts/drm_dirty.c
scripts/collect_perf.sh full.txt -- \
  scripts/fb_stress.py full --frames 20
scripts/collect_perf.sh sparse.txt -- \
  scripts/fb_stress.py sparse --frames 20 \
  --dirty-helper /tmp/drm_dirty --card /dev/dri/card1
```

テストモードは `full`、`same`、`stripe`、`row`、`sparse`、`left`、`right` です。
後の2つは論理画面の片側を対象とし、デュアルパネル構成で各パネルを確認できます。
fbcon が対象のフレームバッファに接続されていると、コンソールの再描画も行数や
SPI のカウンタに含まれます。計測中だけ接続を解除してください。
全画面への書き込みは fbdev が自動通知します。部分行や半幅のテストでは、
標準 DRM の変更通知を送る補助プログラム（`drm_dirty`）を使用します。

### Qt Quick 連携（実験的）

[`contrib/qml-dmabuf`](contrib/qml-dmabuf/README_ja.md)には、VC4 OpenGL で
描画した画像を DRM へ渡すライブラリ（presenter）があります。ユーザー空間で
XRGB8888 形式の GBM 入力バッファ2枚と、DRM への送信を担当するワーカーを管理します。
ドライバ側の2個の TX slot は、これとは別の、変換済み行を保持する送信バッファです。

新しい画像で置き換えられるのは、DRM へ渡す前の `READY` 状態の画像だけです。
送信済みのフレームバッファは DRM が必要とする期間保持します。ワーカーが
描画済み画像と表示中の画像を比較して変更行を求めるため、Qt の描画スレッドは
この比較や DRM への送信完了を待つ必要がありません。

このライブラリは任意機能で、通常の `make`、`make install`、DKMS の導入手順には
含まれません。リポジトリのルートから次を実行します。

```bash
sudo apt install pkg-config libdrm-dev libgbm-dev libegl-dev libgles-dev
make presenter
sudo make install_presenter
```

`libsharp_presenter.so` は `/usr/local/lib`、公開ヘッダは `/usr/local/include` に
インストールされます。OpenGL 経路では VC4/V3D ドライバも有効にする必要があります。
ソフトウェア描画や fbdev 出力では、このライブラリは不要です。
削除する場合は `sudo make uninstall_presenter` を実行します。

### README の図の再生成

Pillow と C コンパイラが必要です。リポジトリのルートで実行します。

```bash
python3 scripts/generate_rendering_comparison.py
```

比較画像は `docs/images/rendering-source.png` を原画像にして、
`src/render.c` の実際の変換処理から生成します。パネル固有の発色や反射は
再現していません。実機写真はこの生成処理には含まれません。

## 参考資料

* [ardangelo による元の Sharp Memory LCD DRM ドライバ](https://github.com/ardangelo/sharp-drm-driver) —
  このリポジトリの fork 元である DRM ドライバ。元の著作権表記はソース内に保持しています。
* [w4ilun による元の SPI/GPIO カーネルドライバ](https://github.com/w4ilun/Sharp-Memory-LCD-Kernel-Driver) —
  本ドライバの源流である fbdev モジュール。過去のピン配置とビルド手順の
  記録あり
* [Sharp Memory LCD プログラミング・アプリケーションノート (PDF)](https://www.sharpsde.com/fileadmin/products/Displays/2016_SDE_App_Note_for_Memory_LCD_programming_V1.3.pdf)

## ライセンス

GPL-2.0-or-later — [LICENSE](LICENSE) を参照。

本リポジトリで作成したコード、ドキュメント、画像に適用します。
写真とイラストの出典は [画像の出典](docs/ASSETS.md) を参照してください。
任意機能の [FFmpeg ビルド](contrib/ffmpeg-rpi-isp/README_ja.md#ライセンスと出典) が
取得する外部コンポーネントには、それぞれの上流ライセンスが適用されます。
