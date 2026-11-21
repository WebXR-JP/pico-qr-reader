# PICO QR Reader

PICO 4 Ultra（PICO OS 5.15.0+）の OpenXR 拡張 **`XR_PICO_camera_image`** を使用し、外向きRGBカメラからリアルタイムにQRコードを読み取るネイティブXRアプリです。

## 特徴

- **PICO 4 Ultra 外向きカラーカメラ対応**: `XR_PICO_camera_image` + `XR_EXT_future`（PICO OS 5.15.7 の OpenXR 1.1 ローダー）により、`config.disable_cameraservice=true` の環境でもリアルタイムに `1280x960` RGBA8888 映像を取得
- **システムパススルー表示**: 背景の実世界は `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` でランタイム標準のパススルー（PICO本体のMRと同じ経路）に任せ、アプリは透明レイヤーに照準とHUDだけを描画。カメラ映像は自前で描画しないため遅延はシステムと同等。カメラ（左RGB, id=1）はQRデコード専用
- **フローティングHUD**: 読み取り状態と結果のパネルはワールド空間に浮遊し、視界中央下を目標に少し遅れて滑らかに追従
- **オフライン高速QRデコード**: 軽量Cライブラリ [`quirc`](https://github.com/dlbeer/quirc) をバックグラウンドスレッドで実行（描画ループをブロックしない）
- **自動クリップボードコピー & URLオープン**:
  - QRコードを検出すると自動で Android クリップボードにコピー
  - URL（`http://` / `https://`）の場合、右コントローラートリガー（Select）でブラウザで開く（テキストの場合は再コピー）
  - デコード結果は `logcat`（タグ `PicoQrReader`）にも出力

## ビルド・インストール

```bash
# ビルドのみ
./build.sh

# ビルド + USB接続中の PICO 4 Ultra へインストール + CAMERA権限付与
./build.sh install
```

生成物: `build/pico-qr-reader.apk`

## 初回起動時の注意（PICO OS の仕様）

PICO OS の `ConfigurationService` の仕様上、`adb install` 直後に `adb shell am start` だけで起動すると未認知パッケージ扱いになり `xrCreateInstance` でクラッシュする場合があります。**初回は PICO Home UI（ライブラリ → 提供元不明のアプリ → PICO QR Reader）から起動してください。**

## ログ確認

```bash
adb logcat -s PicoQrReader PicoQrCamera
```
