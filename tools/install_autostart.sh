#!/bin/bash
# 把电脑端推送程序装成开机自启(登录后自动运行, 挂了自动拉起)。
# 改了 flightboard_host/host.py 之后重新跑一遍本脚本即可。卸载: tools/install_autostart.sh remove
#
# 程序会被拷到 ~/Library/Application Support/FlightBoard/ 再运行:
# macOS 不允许后台服务直接读"文稿"文件夹里的文件, 放在这里才不会被拦。
set -e
SRC="$(cd "$(dirname "$0")/.." && pwd)/flightboard_host"
DST="$HOME/Library/Application Support/FlightBoard"
LABEL=com.flightboard.host
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
if [ "$1" = "remove" ]; then
  rm -f "$PLIST"
  echo "已卸载自启 (程序文件还留在 $DST)"
  exit 0
fi

pkill -f "flightboard_host/host.py" 2>/dev/null || true   # 手动起的那个占着串口
mkdir -p "$DST"
cp "$SRC/host.py" "$SRC/ui.html" "$SRC/airports.json" "$SRC/countries.json" "$DST/"   # 程序 + 控制页面 + 世界机场清单
for k in airlabs_key.txt; do
  [ -f "$SRC/$k" ] && cp "$SRC/$k" "$DST/" && chmod 600 "$DST/$k"
done
[ -f "$DST/airport.txt" ] || cp "$SRC/airport.txt" "$DST/" 2>/dev/null || true
[ -x "$DST/venv/bin/python" ] || python3 -m venv "$DST/venv"
"$DST/venv/bin/python" -c "import serial" 2>/dev/null || "$DST/venv/bin/pip" install -q pyserial

cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$DST/venv/bin/python</string>
        <string>$DST/host.py</string>
        <string>run</string>
    </array>
    <key>WorkingDirectory</key>
    <string>$DST</string>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>ProcessType</key>
    <string>Standard</string>
    <key>ThrottleInterval</key>
    <integer>10</integer>
    <key>StandardOutPath</key>
    <string>$HOME/Library/Logs/FlightBoard.log</string>
    <key>StandardErrorPath</key>
    <string>$HOME/Library/Logs/FlightBoard.log</string>
</dict>
</plist>
PL
launchctl bootstrap "gui/$(id -u)" "$PLIST"
echo "已安装并启动。日志: $HOME/Library/Logs/FlightBoard.log"
