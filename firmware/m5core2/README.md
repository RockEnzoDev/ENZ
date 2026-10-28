# M5Core2 コンパニオン

CYD と ESP-NOW チャット互換の相棒機。アプリは時計・チャット・音楽のみ
(WiFi・天気・センサー画面は意図的に無し)。

## 機能

- 時計: RTC 使用、7セグメント表示。時刻は手動設定
- チャット: ESP-NOW、起動時から ON、固定チャンネル(既定 Ch1、Settings で変更)
  日本語入力(かな変換 + SKK)は CYD と共通仕様
- 音楽: MP3 / WAV プレーヤー(選曲・音量・再生/一時停止・次/前)
  Bluetooth A2DP ソース出力対応(Settings → Bluetooth、出力先 SPK/BT 切替)

## ビルド(重要)

- FQBN: `esp32:esp32:m5stack_core2:PSRAM=disabled`
  **PSRAM は必ず Disabled**。ON だと WiFi + Bluetooth リンク時に IRAM が溢れる
- ライブラリ: M5Unified 0.2.28、M5GFX 0.2.35、ESP8266Audio 2.4.1、
  ESP32-A2DP 1.8.11(GitHub の ZIP から導入。ライブラリマネージャには無い)

## 書き込み

- Core2 に BOOT ボタンは無い(自動で書き込みモードに入る)
- 物理ボタンは左側面の電源のみ(短押し ON / 長押し OFF)。
  画面下の丸3つはタッチボタン
- COM ポートが出ない場合は CH9102 ドライバ(M5Stack 公式)が必要

## SD カード

直下に `jpfont16.bin` と `skk.dic`(リポジトリの assets/sd)、フォルダ `/music`。
