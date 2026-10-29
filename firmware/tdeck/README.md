# LILYGO T-Deck コンパニオン — v14「無線番長」

物理キーボード + トラックボール搭載の**無線専用端末**。
ESP-NOW / LoRa / MeshCore 監視 / FM ラジオ / GPS(Plus)を
1 台に集約し、無線と関係ないもの(時計 UI・音楽・センサー)は持たない。

ホーム画面はチャットターミナルがほぼ全画面。
時刻は隅に小さく表示され、GPS 測位時に自動で合う(RTC 代わり)。

## 機能

- チャット: ESP-NOW(CYD / M5Core2 と互換)+ LoRa(920.8 MHz、STD-T108 準拠。
  設定で変調プロファイル Std / Narrow / MeshCore(実験用リスニング、
  生パケット RAW ログ付き)を切替可能。MeshCore モードでは RAW 画面で
  ADVERT パケットの解読表示(公開鍵・時刻・ノード名、署名検証は未実装)。
  ホームのターミナルからは両方に同時送信
- 日本語入力: **Alt+C** で英数/日本語を切替(Alt+Space はキーボード
  ファーム側で検出できないため)。SKK 変換は Space、候補送りは n / N
- FM ラジオ: Si4703 モジュール(I2C 0x10、Grove / 拡張端子に接続)を
  自動検出。選局シーク、音量、RDS の局名・ラジオテキスト表示。
  音声はモジュールの 3.5 mm ジャックから出す(イヤホンがアンテナ兼用)
- GPS(T-Deck Plus 内蔵): UART1 で NMEA(GGA/RMC)を自動検出
  (通常版では無効化されたまま)。位置ビーコンを ESP-NOW type 4 /
  LoRa type 3 で送信でき、受信した相手とはピア一覧で距離・方角表示。
  GPS 時刻でシステム時計を自動設定する
- 地図: SD にキャッシュした国土地理院タイル(JPG)を表示。
  自分と相手(ビーコン受信済み)のマーカー付き。
  タイルは PC で `tools/gsi_cache.py` を使って事前に用意する(出典: 国土地理院)
- トラックボールカーソル: タッチ不良時の保険。Settings → Cursor で
  Auto(最初のタッチまで)/ ON / OFF。押下 = クリック

## 画面とキー

- ホーム: CHAT(ピア一覧)/ MAP / FM / SET ボタン、ターミナルで入力 → Enter で送信
- ピア一覧: Enter = 会話、R = RAW ログ、F = FM、G = GPS、M = 地図
- FM 画面: `,` / `.` = シーク、- / = = 音量
- GPS 画面: Enter / B = 位置ビーコン送信
- 地図画面: + / - = ズーム、C = GPS 位置にセンタリング、
  トラックボール = パン、タップ = その地点を中心に

## ビルド

- FQBN: `esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB`
- ボード設定: USB CDC On Boot = Enabled(シリアル用)
- ライブラリ: LovyanGFX 1.2.35、LilyGo-display-library(公式プロファイル
  `LilyGo_T_Deck` を使うこと。自前で画面設定を書くと真っ黒になる)、
  RadioLib 7.8.1、TJpg_Decoder 1.1.0
- T-Deck Plus も同じ .ino がそのまま使える(GPS は自動検出)

## 備考

- FM モジュール未接続でも全機能が動作する(ラジオ画面に案内が出るだけ)
- GPS の RX/TX は基板リビジョンで入れ替わることがあるため、
  起動後 20 秒データが来なければ自動で入れ替えて試す
