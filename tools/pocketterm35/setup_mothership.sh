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

echo "== [0/7] Japanese fonts/IME + keyboard layout =="
sudo apt-get update
# IME is fcitx5 + Mozc, NOT IBus: on trixie/labwc, IBus switched engines
# and ran mozc_server yet never delivered conversion to any app
# (isolation-tested in Mousepad, 2026-10-31). fcitx5 works everywhere,
# including the Chromium kiosk via text-input-v3.
sudo apt-get install -y fonts-noto-cjk fcitx5 fcitx5-mozc
mkdir -p "$HOME/.config/fcitx5"
if [ ! -f "$HOME/.config/fcitx5/profile" ]; then
  cat > "$HOME/.config/fcitx5/profile" <<'EOF'
[Groups/0]
Name=Default
Default Layout=us
DefaultIM=mozc

[Groups/0/Items/0]
Name=keyboard-us
Layout=

[Groups/0/Items/1]
Name=mozc
Layout=

[GroupOrder]
0=Default
EOF
fi
# IM modules for the whole labwc session (toggle: Ctrl+Space).
mkdir -p "$HOME/.config/labwc"
touch "$HOME/.config/labwc/environment"
for kv in GTK_IM_MODULE=fcitx QT_IM_MODULE=fcitx 'XMODIFIERS=@im=fcitx'; do
  k="${kv%%=*}"
  if grep -q "^$k=" "$HOME/.config/labwc/environment"; then
    sed -i "s|^$k=.*|$kv|" "$HOME/.config/labwc/environment"
  else
    echo "$kv" >> "$HOME/.config/labwc/environment"
  fi
done
# No on-screen keyboard: the device has a physical one, and squeekboard
# pops up over the Muse login screen.
mkdir -p "$HOME/.config/autostart"
printf '[Desktop Entry]\nType=Application\nName=Squeekboard\nHidden=true\nX-GNOME-Autostart-enabled=false\n' \
  > "$HOME/.config/autostart/squeekboard.desktop"
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

echo "== [1/7] system packages =="
sudo apt-get update
sudo apt-get install -y git python3-pip python3-venv screen minicom \
  i2c-tools usbutils curl unzip

echo "== [2/7] esptool (flashing) =="
if ! command -v esptool >/dev/null 2>&1 && ! python3 -m esptool version >/dev/null 2>&1; then
  python3 -m pip install --user --break-system-packages esptool || \
  python3 -m pip install --user esptool
fi
python3 -m esptool version || true

echo "== [3/7] arduino-cli =="
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

echo "== [4/7] ENZ repo =="
if [ ! -d "$MS/ENZ/.git" ]; then
  git clone https://github.com/RockEnzoDev/ENZ.git "$MS/ENZ"
else
  git -C "$MS/ENZ" pull --ff-only || true
fi

echo "== [5/7] Tailscale =="
if ! command -v tailscale >/dev/null 2>&1; then
  curl -fsSL https://tailscale.com/install.sh | sh
fi
echo "   -> after this script finishes, run:  sudo tailscale up"
echo "   -> and enable SSH:                   sudo tailscale set --ssh"

echo "== [6/7] helper: cot-flash =="
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

echo "== [7/7] Muse terminal face: IM daemon, kiosk, screen blanking =="
# Power on -> desktop -> fcitx5 up -> Chromium fullscreen on muse.ai,
# screen blanks after 10 min idle (any key/touch wakes it).
mkdir -p "$HOME/.config/labwc" "$HOME/.config/swayidle"
AUTOSTART="$HOME/.config/labwc/autostart"
touch "$AUTOSTART"
grep -q '^fcitx5 -d' "$AUTOSTART" || echo 'fcitx5 -d &' >> "$AUTOSTART"
grep -q '^swayidle' "$AUTOSTART" || echo 'swayidle &' >> "$AUTOSTART"
grep -q 'muse.ai' "$AUTOSTART" || \
  echo 'env LANG=ja_JP.UTF-8 GTK_IM_MODULE=fcitx QT_IM_MODULE=fcitx XMODIFIERS=@im=fcitx chromium --kiosk --enable-wayland-ime --wayland-text-input-version=3 --noerrdialogs --disable-session-crashed-bubble --disable-infobars https://muse.ai &' >> "$AUTOSTART"
cat > "$HOME/.config/swayidle/config" <<'EOF'
timeout 600 'wlopm --off HDMI-A-1' resume 'wlopm --on HDMI-A-1'
EOF

echo
echo "DONE. Mothership home: $MS"
echo "Next: sudo tailscale up  (authenticate in the browser)"
echo "Then tell Muse the tailscale hostname so it can connect."
