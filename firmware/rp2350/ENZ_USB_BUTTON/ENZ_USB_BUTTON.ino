// ENZ USB Button — RP2350 (Tenstar RP2350-USB) — step 1 of the RP2350/C3 plan.
// The smallest HID foundation: one button, one keypress. The joystick and
// dongle-key builds derive from this file.
//
// WIRING (only 2 wires):
//   Button between GP2 and GND. No resistor needed (internal pull-up).
//   Press = GP2 LOW. While held, the key is sent ONCE (no auto-repeat),
//   so an accidental long press cannot flood the PC with keystrokes.
//
// KEY (change this one line to change the function):
//   Default is F13: an inert key that does nothing by itself, meant to be
//   mapped in an app (OBS, Discord, AutoHotkey, etc.) to mic-mute,
//   record-start, or anything else. It is inert on purpose — a stray press
//   must never type into a document.
//   For a quick visible test, temporarily set it to HID_KEY_SPACE and press
//   the button in a text editor; a space appears. Then set it back.
//   Other useful values: HID_KEY_M, HID_KEY_ENTER, HID_KEY_F20.
//   Modifier example: to send Ctrl+M, set BUTTON_MODIFIER to
//   KEYBOARD_MODIFIER_LEFTCTRL and BUTTON_KEY to HID_KEY_M.
//
// BUILD (arduino-pico core 6.3.1, same setup as the HID test):
//   Board: Generic RP2350, Flash: 16MB, USB Stack: TinyUSB
//   FQBN: rp2040:rp2040:generic_rp2350:flash=16777216_0,usbstack=tinyusb
//   NOTE: the core's classic Keyboard.h hard-errors with the TinyUSB stack,
//   so this uses Adafruit_USBD_HID directly (same as the test sketch).
// FLASHING: hold BOOTSEL while plugging the stick in (or double-tap reset
//   on boards that expose it), then copy the generated .uf2 onto the
//   RPI-RP2 drive. The stick reappears as a USB keyboard.
// TEST: open a keyboard tester page (it shows F13) or remap F13 in your app.
#include <Adafruit_TinyUSB.h>

Adafruit_USBD_HID usb_hid;
uint8_t const desc_hid_report[] = {TUD_HID_REPORT_DESC_KEYBOARD()};

// ---- user settings (edit these two lines only) ----
const int     BTN_PIN         = 2;              // button: GP2 -> GND
const uint8_t BUTTON_KEY      = HID_KEY_F13;    // key sent on press
const uint8_t BUTTON_MODIFIER = 0;              // e.g. KEYBOARD_MODIFIER_LEFTCTRL
const unsigned long DEBOUNCE_MS = 20;           // press debounce

// ---- state ----
bool     prevStable = HIGH;   // last debounced level (HIGH = released)
bool     lastRaw    = HIGH;
unsigned long lastChangeMs = 0;

void setup() {
  pinMode(BTN_PIN, INPUT_PULLUP);
  usb_hid.setReportDescriptor(desc_hid_report, sizeof(desc_hid_report));
  usb_hid.begin();
}

void loop() {
  bool raw = digitalRead(BTN_PIN);

  // Debounce: only accept a level that has been stable for DEBOUNCE_MS.
  if (raw != lastRaw) {
    lastRaw = raw;
    lastChangeMs = millis();
  }
  if (millis() - lastChangeMs < DEBOUNCE_MS) return;

  // Edge: HIGH -> LOW = press. Send one tap, then wait for release.
  if (prevStable == HIGH && raw == LOW) {
    if (usb_hid.ready()) {
      uint8_t keycode[6] = {BUTTON_KEY, 0, 0, 0, 0, 0};
      usb_hid.keyboardReport(0, BUTTON_MODIFIER, keycode);
      delay(10);
      usb_hid.keyboardRelease(0);
    }
  }
  prevStable = raw;
}
