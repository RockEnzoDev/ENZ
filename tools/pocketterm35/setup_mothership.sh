#!/usr/bin/env bash
# ============================================================
# ENZ mothership setup for PocketTerm35 (Raspberry Pi OS, user: enz)
# Run AFTER the fresh install + driver restore, as the enz user:
#   bash setup_mothership.sh
# Optional: WITH_CORES=1 bash setup_mothership.sh
#   -> also installs arduino-cli ESP32/RP2350 cores (big download,
#      lets the mothership BUILD firmware, not just flash it)
# Safe to re-run: every step skips what is already in place.
# ============================================================
set -euo pipefail

MS="$HOME/mothership"
mkdir -p "$MS" "$HOME/bin"

echo "== [0/6] Japanese fonts/IME + keyboard layout =="
sudo apt-get update
sudo apt-get install -y fonts-noto-cjk ibus-mozc
# The PocketTerm35 Pico keyboard is US layout. The labwc desktop takes
# its layout from XKB_DEFAULT_LAYOUT in the labwc environment files
# (Imager/cloud-init writes gb then jp there; /etc/default/keyboard
# does NOT drive the desktop). Set both files to us.
sudo sed -i 's/^XKB_DEFAULT_LAYOUT=.*/XKB_DEFAULT_LAYOUT=us/' /etc/xdg/labwc/environment || true
mkdir -p "$HOME/.config/labwc"
sed -i 's/^XKB_DEFAULT_LAYOUT=.*/XKB_DEFAULT_LAYOUT=us/' "$HOME/.config/labwc/environment" 2>/dev/null || true
grep -q '^XKB_DEFAULT_LAYOUT=' "$HOME/.config/labwc/environment" 2>/dev/null || \
  echo 'XKB_DEFAULT_LAYOUT=us' >> "$HOME/.config/labwc/environment"
sudo sed -i 's/XKBLAYOUT="jp"/XKBLAYOUT="us"/' /etc/default/keyboard || true

echo "== [1/6] system packages =="
sudo apt-get update
sudo apt-get install -y git python3-pip python3-venv screen minicom \
  i2c-tools usbutils curl unzip

echo "== [2/6] esptool (flashing) =="
if ! command -v esptool >/dev/null 2>&1 && ! python3 -m esptool version >/dev/null 2>&1; then
  python3 -m pip install --user --break-system-packages esptool || \
  python3 -m pip install --user esptool
fi
python3 -m esptool version || true

echo "== [3/6] arduino-cli =="
if [ ! -x "$HOME/bin/arduino-cli" ]; then
  curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh \
    | BINDIR="$HOME/bin" sh
fi
export PATH="$HOME/bin:$PATH"
arduino-cli version || true

if [ "${WITH_CORES:-0}" = "1" ]; then
  echo "== [3b] arduino cores (esp32 + rp2040) =="
  arduino-cli config init --overwrite >/dev/null 2>&1 || true
  arduino-cli config add board_manager.additional_urls \
    https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json || true
  arduino-cli core update-index
  arduino-cli core install esp32:esp32@3.3.12 || arduino-cli core install esp32:esp32
  arduino-cli core install rp2040:rp2040 || true
fi

echo "== [4/6] ENZ repo =="
if [ ! -d "$MS/ENZ/.git" ]; then
  git clone https://github.com/RockEnzoDev/ENZ.git "$MS/ENZ"
else
  git -C "$MS/ENZ" pull --ff-only || true
fi

echo "== [5/6] Tailscale =="
if ! command -v tailscale >/dev/null 2>&1; then
  curl -fsSL https://tailscale.com/install.sh | sh
fi
echo "   -> after this script finishes, run:  sudo tailscale up"
echo "   -> and enable SSH:                   sudo tailscale set --ssh"

echo "== [6/6] helper: cot-flash =="
cat > "$HOME/bin/cot-flash" <<'EOF'
#!/usr/bin/env bash
# usage: cot-flash <serial-port> <bin-file> [offset] [chip]
#   default offset 0x10000 (app-only images), default chip auto
#   examples:
#     cot-flash /dev/ttyACM0 firmware-m5stack-unitc6l-....bin 0x10000 esp32c6
#     cot-flash /dev/ttyUSB0 ENZ_TDECK_v16-merged.bin 0x0 esp32s3
set -euo pipefail
PORT="${1:?port required}"; BIN="${2:?bin required}"
OFF="${3:-0x10000}"; CHIP="${4:-auto}"
python3 -m esptool --chip "$CHIP" --port "$PORT" write-flash "$OFF" "$BIN"
EOF
chmod +x "$HOME/bin/cot-flash"

echo
echo "DONE. Mothership home: $MS"
echo "Next: sudo tailscale up  (authenticate in the browser)"
echo "Then tell Muse the tailscale hostname so it can connect."
