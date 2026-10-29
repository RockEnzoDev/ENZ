# CYD (ESP32-2432S028) ランチャー

> 開発は完了(2026-10-29)。標準は部屋用(室内版 INDOOR_CLEAN)で、
> .bin リリースは ENZ_CYD_ROOM_v1 のみ。外出用(OUTDOOR_CLEAN)は
> .ino ソースのみの配布とする。

縦画面(240×320)の DOS 風ランチャー。大きな時計、気圧グラフ(24時間)を
常時表示するセンサーカード、天気、ホーム画面のチャットターミナルが主役。

## バリエーション

| スケッチ | 内容 |
|---|---|
| ENZ_LAUNCHER_OUTDOOR | 屋外版(実機検証済みの基準版)。WiFi は天気取得時のみ手動 |
| ENZ_LAUNCHER_OUTDOOR_CLEAN | 屋外版の軽量化版(未使用アプリを除去)。実機確認済み |
| ENZ_LAUNCHER_INDOOR_CLEAN | 屋内版。起動時 WiFi ON、天気を 30 分ごとに自動取得 |

## 操作の要点

- Photo ボタン: `/picture` のスライドショーを即開始(15 秒間隔、タップで終了)
- Music ボタン: `/music` を全自動再生するトグル(音量 100%、画面遷移なし)
- チャットのチャンネル変更: Settings → ESP-NOW → Chat channel(Auto / Ch1–13)
- ESP-NOW はチャット使用中のみ ON

## ビルド

- ボード: ESP32 Dev Module(esp32:esp32:esp32)
- ライブラリ: TFT_eSPI 2.5.43(Setup42 を選択)、XPT2046_Touchscreen、
  ArduinoJson 7.4.3、TJpg_Decoder 1.1.0
- センサー: AHT10/AHT20(I2C SDA27/SCL22)、BMP280/BME280
  (気圧は設定の標高で補正)

## SD カード

直下に `jpfont16.bin` と `skk.dic`(リポジトリの assets/sd)、
フォルダ `/music`、`/picture`、`/chats`(自動作成)。
`presshist.bin` は気圧の 24 時間履歴(CLEAN 版が自動で読み書き。
Launcher 等でファームを入れ替えてもグラフが残るように SD に保存)
