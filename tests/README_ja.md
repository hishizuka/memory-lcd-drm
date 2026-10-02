# ドライバの単体テスト

[English](README.md) | 日本語

ドライバの C コードをユーザー空間で実行するテストです。`stubs/` が Linux
カーネルと DRM の API を代替するため、Raspberry Pi やパネル、カーネルヘッダ、
root 権限は不要です。カーネルモジュールのビルド・ロードは行いません。

## 必要な環境

* Linux または macOS
* C11 対応の C コンパイラ（GCC または Clang）、Make、POSIX スレッドの開発環境
* テスト対象の `src/`、このディレクトリのソース・`stubs/`・`golden.txt`

Debian / Raspberry Pi OS では `build-essential`、macOS では Xcode Command
Line Tools が基本のビルドツールを提供します。Pillow や Python は不要です。
コンパイラは `CC=clang` などで選択できます。アサーションを使うため、
`CFLAGS` に `-DNDEBUG` を指定しないでください。

## 実行方法

以下のコマンドは、リポジトリのルートで実行します。

```bash
make -C tests check
```

7種類のテストをビルドして実行します。ネイティブ ARM64 環境
（`uname -m` が `aarch64` または `arm64`）では、NEON 版の描画テストも
自動的に実行します。他のアーキテクチャでは通常の C 実装を検証します。

全テストが成功すると、各テストが `all ... cases passed` と表示し、
`make` は終了コード `0` で終了します。描画結果の不一致はケース名と期待値・実測値、
その他の検証失敗はアサーションなどの診断を出力し、終了コードは非ゼロになります。
ビルドに失敗した場合も非ゼロです。並列実行では出力順が変わることがあります。

| ソース / ビルド対象 | 検証する内容 |
| --- | --- |
| `golden_render.c` / `golden_render` | 2/8/64色変換の出力ハッシュ、量子化、NV12 変換、右パネルの部分更新、モノクロ順序ディザの白の密度・閾値の単調性・反転・スカラー/NEON の一致 |
| `tx_pipeline.c` / `tx_pipeline` | 行の世代管理、送信順序、複数バッチの進行、再試行と上限、停止・再開、パネル間の送信順 |
| `spi_io.c` / `spi_io` | コントローラの転送制限、タグ付き行バッチ、不正なバッチ、パネルコマンド |
| `perf_stats.c` / `perf_stats` | 計測の無効化・リセット、遅延のパーセンタイル、debugfs 制御 |
| `params_parse.c` / `params_parse` | 秒からミリ秒への変換、不正入力、境界値、オーバーフロー |
| `fb_damage.c` / `fb_damage` | 更新範囲からの対象パネル選択、完全行への展開、誤差拡散・全面再生成時の範囲 |
| `registry.c` / `registry` | 実際の POSIX スレッドとミューテックスを使ったデバイス情報の公開と色数制約の競合検証 |

個別のテストも実行できます。ビルド対象だけを指定した場合は、実行までは行いません。

```bash
make -C tests tx_pipeline
./tests/.build/tx_pipeline

make -C tests golden_render
./tests/.build/golden_render --check tests/golden.txt
```

NEON 版だけをビルド・実行する場合:

```bash
make -C tests check-neon
```

非 ARM64 環境では、このターゲットは実行できない旨を表示して正常終了します。
この終了は NEON テストの成功を意味しません。

## 生成物と削除

実行ファイルと基準値の更新候補は `tests/.build/` に生成します。このディレクトリは
Git の除外対象です。テストのソース、`Makefile`、`stubs/`、比較基準の `golden.txt`
は公開対象として保持します。

```bash
make -C tests clean
```

生成された実行ファイルと更新候補を削除します。`.build/` ディレクトリ自体と
比較基準の `tests/golden.txt` は残ります。コンパイラやコンパイルオプションを
変更した場合も、いったん `clean` してから再ビルドしてください。

`BUILD_DIR` で出力先を変更できますが、その場合の Git 除外設定は別途必要です。
削除時も同じ `BUILD_DIR` を指定してください。

## golden.txt の更新

`golden.txt` は、描画ケースごとの期待する出力ハッシュを記録した比較基準です。
通常の `check` では変更しません。**テストの失敗を消す目的で再生成しないでください。**
まず変換処理の不具合か、意図した仕様変更かを調べます。更新するのは、意図した
変換結果の変更やテストケースの追加をレビューし、期待する結果を別途確認した場合です。
ハッシュの一致だけでは、色やディザリングが正しいことを保証できません。

更新候補は次のように作ります。

```bash
make -C tests golden
diff -u tests/golden.txt tests/.build/golden.txt
```

`make golden` は候補を `.build/golden.txt` に出力し、追跡中の `golden.txt` を
上書きしません。`diff` の終了コード `1` は差分があることを示します。
変更したケース・理由・出力内容を確認してから、明示的に採用します。

```bash
cp tests/.build/golden.txt tests/golden.txt
make -C tests check
git diff -- tests/golden.txt
```

基準値の変更は、対応するコード・テストの変更と理由を一緒にレビューしてください。
NEON の対象経路を変更した場合は、ARM64 上でも検証します。

## 実機で別途確認すること

これらのテストでは、本物の SPI 転送、GPIO/PWM、パネルの発色、電気的な配線、
実カーネルでのスケジューリングや DRM の動作までは検証できません。
`stubs/` の成功を実機での正常動作と同一視しないでください。

Raspberry Pi 上でのカーネルビルド、表示・電源制御、再有効化、DMA-BUF 入力などは
別途確認します。補助プログラムは [`scripts/drm_smoke.c`](../scripts/drm_smoke.c)、
導入と性能計測は[日本語 README](../README_ja.md)を参照してください。
