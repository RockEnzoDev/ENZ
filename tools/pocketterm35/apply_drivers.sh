#!/bin/bash
# PocketTerm35 driver recipe -- harvested from the manufacturer image
# on 2026-10-30 (see harvest analysis). Run on a FRESH Raspberry Pi OS
# (trixie, 64-bit) install, as your normal user. Re-runnable.
#
# What makes the PocketTerm35 work (all confirmed from the harvest):
#   display : Waveshare 3.5" DPI LCD
#             -> overlays waveshare-35dpi-4b + waveshare-35dpi-5b
#   touch   : Goodix capacitive, I2C 0x5d (in-kernel driver)
#             -> dtparam=i2c_arm=on
#   keyboard: RP2040 USB HID (1209:0001) on the Pi 4 OTG port
#             -> dtoverlay=dwc2,dr_mode=host
# Everything else is stock Raspberry Pi OS.
set -e
cd "$(dirname "$0")"
BOOT=/boot/firmware
[ -d "$BOOT" ] || BOOT=/boot

echo "== install Waveshare DPI overlays =="
sudo install -m 644 waveshare-35dpi-4b.dtbo \
                    waveshare-35dpi-5b.dtbo "$BOOT/overlays/"
# Verify: stock RPi OS ships its own 5b WITHOUT the ft6236@5d node,
# which leaves touch dead. The harvested files must match these md5s:
md5sum "$BOOT/overlays/waveshare-35dpi-4b.dtbo" "$BOOT/overlays/waveshare-35dpi-5b.dtbo"
echo "  expected 4b: 8fba10fbec8b95fc6c1aea8bae56592a"
echo "  expected 5b: ec0dae463e40becc7e9124b97e7537fb"

echo "== patch config.txt =="
CFG="$BOOT/config.txt"
for line in "dtparam=i2c_arm=on" \
            "dtoverlay=waveshare-35dpi-4b" \
            "dtoverlay=waveshare-35dpi-5b" \
            "dtoverlay=dwc2,dr_mode=host"; do
  grep -qx "$line" "$CFG" || echo "$line" | sudo tee -a "$CFG" >/dev/null
done

echo "== WiFi regdom -> JP =="
if grep -q "cfg80211.ieee80211_regdom=" "$BOOT/cmdline.txt"; then
  sudo sed -i 's/cfg80211.ieee80211_regdom=[A-Z][A-Z]/cfg80211.ieee80211_regdom=JP/' "$BOOT/cmdline.txt"
else
  sudo sed -i 's/$/ cfg80211.ieee80211_regdom=JP/' "$BOOT/cmdline.txt"
fi

echo
echo "DONE. Reboot, then verify:"
echo "  evtest   -> 'Goodix Capacitive TouchScreen' and"
echo "              'My Custom Pico Keyboard' should be listed."
