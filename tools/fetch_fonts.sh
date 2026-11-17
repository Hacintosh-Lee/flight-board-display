#!/bin/bash
# 下载生成字库要用的开源字体(都是 SIL Open Font License, 来自 Google Fonts 官方仓库)到 assets/fonts/。
# 只有想改字体、重新生成 include/fb_font.h 时才需要; 仓库里已经带了生成好的 fb_font.h。
set -euo pipefail
DIR="$(cd "$(dirname "$0")/.." && pwd)/assets/fonts"
mkdir -p "$DIR"
BASE=https://raw.githubusercontent.com/google/fonts/main/ofl
for f in "notosanssc/NotoSansSC[wght].ttf" \
         "notosansmono/NotoSansMono[wdth,wght].ttf" \
         "barlowcondensed/BarlowCondensed-SemiBold.ttf" \
         "barlowcondensed/BarlowCondensed-Bold.ttf" \
         "notosansthailooped/NotoSansThaiLooped[wdth,wght].ttf" \
         "notosanssc/OFL.txt"; do
  out="$DIR/$(basename "$f")"
  [ -s "$out" ] && continue
  echo "下载 $f"
  curl -fsSL "$BASE/$(echo "$f" | sed 's/\[/%5B/g; s/\]/%5D/g')" -o "$out"
done
echo "字体在 $DIR"
