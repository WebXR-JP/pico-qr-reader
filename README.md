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

Gradle（Android Gradle Plugin）+ CMake 構成です。

### 必要なもの

- JDK 17 以上
- Android SDK（`ANDROID_HOME` 環境変数、または `local.properties` の `sdk.dir` で指定）
  - NDK と CMake は `app/build.gradle.kts` で固定したバージョンが初回ビルド時に自動ダウンロードされます（SDK ライセンスへの同意が必要: `sdkmanager --licenses`）

### コマンド

```bash
./gradlew assembleDebug
```

```bash
./gradlew installDebug
```

```bash
adb shell pm grant dev.pico.qrreader android.permission.CAMERA
```

生成物: `app/build/outputs/apk/debug/pico-qr-reader-debug.apk`

### 構成

| パス | 内容 |
| --- | --- |
| `app/src/main/java/` | `MainActivity`（JNI ホスト、クリップボード / URL / HUD ビットマップ描画） |
| `app/src/main/cpp/` | ネイティブ本体（OpenXR ループ、`XR_PICO_camera_image`）と `CMakeLists.txt` |
| `third_party/quirc/` | QR デコーダ（静的ライブラリとしてビルド） |
| `third_party/openxr/`, `third_party/lib/` | **PICO OpenXR SDK** のヘッダーとローダー。PICO 独自拡張を含むため Khronos 版 AAR では置き換え不可 |

### CI / リリース

GitHub Actions（`.github/workflows/build.yml`）で push / PR ごとに debug・release ビルドと lint を実行し、debug APK を artifact としてアップロードします。

`v*` タグを push すると署名済み release APK をビルドして GitHub Release に添付します。リポジトリに次の Secrets が必要です。

| Secret | 内容 |
| --- | --- |
| `RELEASE_KEYSTORE_BASE64` | keystore ファイルを `base64 -w0` したもの |
| `RELEASE_KEYSTORE_PASSWORD` | keystore のパスワード |
| `RELEASE_KEY_ALIAS` | 鍵のエイリアス |
| `RELEASE_KEY_PASSWORD` | 鍵のパスワード |

ローカルでも同名の環境変数（`RELEASE_KEYSTORE_PATH` に keystore のパス）を設定すれば `./gradlew assembleRelease` で署名済み APK を作れます。未設定の場合は未署名 APK になります。

## 初回起動時の注意（PICO OS の仕様）

PICO OS の `ConfigurationService` の仕様上、`adb install` 直後に `adb shell am start` だけで起動すると未認知パッケージ扱いになり `xrCreateInstance` でクラッシュする場合があります。**初回は PICO Home UI（ライブラリ → 提供元不明のアプリ → PICO QR Reader）から起動してください。**

## ログ確認

```bash
adb logcat -s PicoQrReader PicoQrCamera
```
