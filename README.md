# PICO QR Reader

PICO 4 Ultra（PICO OS 5.15.0+）の OpenXR 拡張 **`XR_PICO_camera_image`** を使用し、外向きRGBパススルーカメラからリアルタイムにQRコードを読み取るネイティブXRアプリです。

## 特徴

- **PICO 4 Ultra 外向きカラーカメラ対応**: `XR_PICO_camera_image` + `XR_EXT_future`（PICO OS 5.15.7 の OpenXR 1.1 ローダー）により、`config.disable_cameraservice=true` の環境でもリアルタイムに `1280x960` RGBA8888 映像を取得
- **視界全体のカメラビュー + 両眼視差**: 左右RGBカメラ（id=1/2）を同時キャプチャできる環境では、左眼に左カメラ・右眼に右カメラの映像を割り当ててステレオ視差を再現。ピンホール内部パラメータ（焦点距離・主点・FOV）で表示FOVへ再投影する。2台目が使えない場合は自動でモノラル表示にフォールバック
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
