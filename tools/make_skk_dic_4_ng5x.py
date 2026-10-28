#!/usr/bin/env python3
"""
Make skk.dic (kanji conversion dictionary) for the ENZ Launcher from an SKK dictionary.

  python3 make_skk_dic.py                       # downloads SKK-JISYO.M (medium size) and converts it
  python3 make_skk_dic.py L                     # S / M / L = small / medium / large dictionary
  python3 make_skk_dic.py path/to/SKK-JISYO.L   # convert a dictionary you already have

Copy the resulting skk.dic to the ROOT of the SD card.
The dictionary comes from the SKK project (https://github.com/skk-dev/dict, GPL / CC-BY-SA style
licences - check the licence of the file you use). This script only converts the file:
 - keeps normal entries only ("okuri-nasi"), reading in hiragana
 - removes annotations, converts EUC-JP to UTF-8
 - sorts by UTF-8 bytes (the launcher does a binary search on the file)
"""
import sys, os, re, urllib.request

BASE = "https://raw.githubusercontent.com/skk-dev/dict/master/SKK-JISYO."
arg = sys.argv[1] if len(sys.argv) > 1 else "M"
if os.path.exists(arg):
    raw = open(arg, "rb").read()
else:
    url = BASE + arg.upper()
    print("downloading", url)
    raw = urllib.request.urlopen(url, timeout=60).read()

try:
    text = raw.decode("utf-8")
except UnicodeDecodeError:
    text = raw.decode("euc_jp", errors="ignore")

entries = {}
section_nasi = True            # entries before the ';; okuri-ari' marker are okuri-nasi in some files; handle both
HIRA = re.compile(r"^[ぁ-ゖー]+$")
for line in text.splitlines():
    if line.startswith(";;"):
        if "okuri-ari" in line: section_nasi = False
        elif "okuri-nasi" in line: section_nasi = True
        continue
    if not section_nasi or " /" not in line:
        continue
    key, rest = line.split(" /", 1)
    if not HIRA.match(key):
        continue
    cands = []
    for c in rest.strip().rstrip("/").split("/"):
        c = c.split(";")[0].strip()
        if c and not c.startswith("(") and c not in cands:
            cands.append(c)
    if cands:
        if key in entries:
            for c in cands:
                if c not in entries[key]: entries[key].append(c)
        else:
            entries[key] = cands

out = "skk.dic"
with open(out, "w", encoding="utf-8", newline="\n") as f:
    for key in sorted(entries, key=lambda k: k.encode("utf-8")):
        f.write("%s /%s/\n" % (key, "/".join(entries[key][:20])))
print("wrote %s: %d entries, %.1f KB" % (out, len(entries), os.path.getsize(out) / 1024))
