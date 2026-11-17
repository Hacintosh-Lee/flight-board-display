#!/bin/bash
# 编译"航班牌"桌面应用, 装到 ~/Applications/航班牌.app (可以拖进程序坞)。需要装了 Xcode 命令行工具。
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$HOME/Applications/航班牌.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
swiftc -O -o "$APP/Contents/MacOS/FlightBoard" "$HERE/app/main.swift" -framework Cocoa -framework WebKit
[ -f "$HERE/app/AppIcon.icns" ] && cp "$HERE/app/AppIcon.icns" "$APP/Contents/Resources/"
cat > "$APP/Contents/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>航班牌</string>
    <key>CFBundleDisplayName</key><string>航班牌</string>
    <key>CFBundleIdentifier</key><string>com.flightboard.app</string>
    <key>CFBundleExecutable</key><string>FlightBoard</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>LSMinimumSystemVersion</key><string>11.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <!-- 页面在本机的 http://127.0.0.1:8377, 要允许访问本机的明文地址 -->
    <key>NSAppTransportSecurity</key>
    <dict><key>NSAllowsLocalNetworking</key><true/></dict>
</dict>
</plist>
PL
codesign --force --deep -s - "$APP" >/dev/null 2>&1 || true
echo "已装好: $APP"
