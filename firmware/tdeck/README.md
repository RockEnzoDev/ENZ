# LILYGO T-Deck コンパニオン

物理キーボード + トラックボール搭載のチャット端末。DOS グリーンの UI。
ESP-NOW と LoRa の両対応で、ホーム画面は CYD 風
(7セグ時計 + センサーカード + ターミナル)。

## 機能

- チャット: ESP-NOW(CYD / M5Core2 と互換)+ LoRa(920.8 MHz、STD-T108 準拠。
  設定で変調プロファイル Std / Narrow / MeshCore(実験用リスニング、
  生パケット RAW ログ付き)を切替可能。MeshCore モードでは RAW 画面で
  ADVERT パケットの解読表示(公開鍵・時刻・ノード名、署名検証は未実装)
  ホームのターミナルからは両方に同時送信
- 日本語入力: **Alt+C** で英数/日本語を切替(Alt+Space はキーボード
  ファーム側で検出できないため)。SKK 変換は Space、候補送りは n / N
- センサー: I2C に AHT20 / BME280 / BMP280 を挿せば自動表示
  (気圧は 24 時間グラフ付き)
- 音楽: MP3 / WAV 再生(I2S スピーカー)
- トラックボールカーソル: タッチ不良時の保険。Settings → Cursor で
  Auto(最初のタッチまで)/ ON / OFF。押下 = クリック

## ビルド

- FQBN: `esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB`
- ボード設定: USB CDC On Boot = Enabled(シリアル用)
- ライブラリ: LovyanGFX 1.2.35、LilyGo-display-library(公式プロファイル
  `LilyGo_T_Deck` を使うこと。自前で画面設定を書くと真っ黒になる)、
  RadioLib 7.8.1、ESP8266Audio 2.4.1

## 備考

- ディスプレイは色反転タイプ(invertDisplay(true) 済み)
- キーボードは I2C 0x55(ESP32-C3)。GT911 タッチ・センサーとバス共有
- SD 直下に `jpfont16.bin` と `skk.dic`(リポジトリの assets/sd)が
  無いと日本語が豆腐表示になる
