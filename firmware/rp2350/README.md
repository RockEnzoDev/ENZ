# RP2350-USB ボタン (Tenstar RP2350-USB)

USB HID デバイスの基盤スケッチ。ジョイスティックやドングルキーへの派生元。

## ENZ_USB_BUTTON

- ボタン: GP2 と GND の間に接続(内部プルアップ、20 ms デバウンス)
- 長押ししても連射しない(押した瞬間に1回だけ)
- 既定のキーは **F13**(単体では無害。OBS や Discord 側で割り当てて使う)
  キーを変える場合はスケッチ先頭の `BUTTON_KEY` を変更

## ビルド

- FQBN: `rp2040:rp2040:generic_rp2350:flash=16777216_0,usbstack=tinyusb`
  (arduino-pico core 6.3.1)
- 注意: TinyUSB スタックでは古典的な `Keyboard.h` が使えない。
  HID は同梱の `Adafruit_USBD_HID` を使う

## 書き込み

BOOTSEL(基板の BOOT ボタンを押しながら USB 接続)でマスストレージとして
現れるので、生成された .uf2 をコピーする。
