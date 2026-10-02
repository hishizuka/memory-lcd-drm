# Sharp DRM DMA-BUF presenter

[English](README.md) | 日本語

## 概要

VC4 OpenGLで描画するlinear XRGB8888 GBM bufferを2枚管理し、Sharp DRMへ
PRIME importしてatomic commitするuserspaceライブラリです。kernel driverの
DRM atomic semanticsは維持し、古いframeはcommit前のREADY状態でだけ置換します。

## 更新構造

```text
OpenGL render
  -> GBM buffer A/B
  -> READYは最大1枚（新しい描画で置換可能）
  -> submit worker（atomic commitでblock可能）
  -> sharp-drm desired/displayed rows
  -> immutable TX slot A/B
  -> SPI
```

buffer状態は`FREE -> RENDERING -> READY -> SUBMITTING -> ACTIVE`と遷移します。
`SUBMITTING`と`ACTIVE`は変更しません。空きbufferがなく、まだworkerが取得して
いない`READY`がある場合は、その古いframeを破棄して同じbufferへ最新sceneを
描画できます。DRM commit済みのframeは破棄しません。

完了通知には`eventfd`を使用します。Qt側は`QSocketNotifier`などで監視し、bufferが
空いた時点で最新sceneだけを描画できます。固定周期timerは不要です。

`sharp_presenter_submit_auto_damage()`はsubmit workerで新しく描画したlinear bufferを
前面bufferと比較し、変更行からdamage clipを作ります。render threadはGPU完了とqueue後に
戻るため、CPU比較やDRM commitを待ちません。無変更frameはDRM commit前に破棄しますが、
commit済みframeの通常のDRM lifetime semanticsは維持します。producerがdamage範囲を
把握している場合は`sharp_presenter_submit()`を使用できます。

## ビルドとインストール

```sh
sudo apt install pkg-config libdrm-dev libgbm-dev libegl-dev libgles-dev
make
sudo make install
```

ライブラリ生成時とbuffer破棄時は、呼び出しthreadにcurrentなEGL/OpenGL contextが
必要です。`sharp_presenter_submit()`は正しさを優先して`glFinish()`を実行します。
将来は明示fenceへ置き換え可能です。

同梱demoは自動行damageが既定です。`--full-damage`を指定すると、producerが指定する
全画面damageとの比較ができます。
