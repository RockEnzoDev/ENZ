# ENZ Series

通信互換性を持つ自作デバイスのシリーズです。どの機種も同じチャットプロトコル
([docs/PROTOCOL.md](docs/PROTOCOL.md))で会話でき、家族のように増やしていけます。

## 機種一覧

| 機種 | フォルダ | 役割 | 状態 |
|---|---|---|---|
| CYD (ESP32-2432S028) | [firmware/cyd](firmware/cyd) | 屋外用ランチャー / 屋内用ランチャー。時計・センサー・天気・チャット端末 | 実機確認済み |
| M5Core2 | [firmware/m5core2](firmware/m5core2) | コンパニオン。時計(RTC)・チャット・音楽プレーヤー・Bluetooth音声出力 | 実機確認済み |
| LILYGO T-Deck | [firmware/tdeck](firmware/tdeck) | 物理キーボード端末。チャット(ESP-NOW + LoRa)・センサー・音楽 | 実機確認済み(Phase 1) |
| RP2350-USB (Tenstar) | [firmware/rp2350](firmware/rp2350) | USBボタン(F13)。シリーズの入力デバイス基盤 | コンパイル検証済み |

今後の予定: Stamp C3 無線コールボタン、RP2350 + ESP32-C3 ペア(ENZ BASE)、
音声PTT(プロトコルの type 3 を予約済み)。

## 共通資産

- [assets/sd](assets/sd) — 全機種共通のSDカード用ファイル
  - `jpfont16.bin` — 日本語フォント(16px)。SD直下に置く
  - `skk.dic` — SKK変換辞書。SD直下に置く(無くてもかな入力のみ可能)
- [tools](tools) — `make_skk_dic_4_ng5x.py`(skk.dic 生成スクリプト)

## ビルド環境(共通)

- Arduino IDE 2.3.10 / arduino-cli
- ESP32 core 3.3.12、RP2040 core (arduino-pico) 6.3.1
- 機種ごとのボード設定・必要ライブラリは各フォルダの README を参照

## 設計方針

- 工場出荷時の初期値に個人情報は入れない(位置情報は東京都庁、表示名は JohnDoe)
- 新しい機種は、まず既存機種とチャット互換であることから作る
- 未知のパケットタイプは無視する(将来拡張で旧ファームを壊さない)
