#!/usr/bin/env bash
# 在私有 KWin、会话总线和 XDG 目录中验证区域并发采集，不读写当前 Plasma 桌面。
# Run the capture case on a private KWin display, session bus and XDG tree, never the active desktop.
set -eu
test_binary=$(realpath "${1:?usage: run_virtual_capture_reentry.sh build/linux-ui/mosaic_ui_test}")
test_root=$(mktemp -d)
trap 'rm -rf -- "$test_root"' EXIT
unset DISPLAY WAYLAND_DISPLAY WAYLAND_SOCKET DBUS_SESSION_BUS_ADDRESS XDG_SESSION_ID QT_QPA_PLATFORM SESSION_MANAGER
export XDG_RUNTIME_DIR="$test_root/run" XDG_CONFIG_HOME="$test_root/config" XDG_DATA_HOME="$test_root/data"
export XDG_CACHE_HOME="$test_root/cache" XDG_STATE_HOME="$test_root/state" XDG_CURRENT_DESKTOP=KDE
export QT_ACCESSIBILITY=0 NO_AT_BRIDGE=1 JIETUBA_REENTRY_BINARY="$test_binary" JIETUBA_REENTRY_RESULT="$test_root/result"
mkdir -m 700 -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME" "$XDG_DATA_HOME/applications"
cat >"$XDG_DATA_HOME/applications/jietuba-capture-reentry-test.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Jietuba isolated capture test
Exec="$test_binary"
NoDisplay=true
X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2
EOF
# libei 仅用于该虚拟桌面测试，不是应用运行依赖。 / Test-only compositor input driver.
export JIETUBA_TEST_CLICK="$test_root/pin-click"
c++ -std=c++20 -fPIC -pie "$(dirname "$0")/virtual_pin_click.cpp" -o "$JIETUBA_TEST_CLICK" $(pkg-config --cflags --libs Qt6Core Qt6DBus libei-1.0)
cat >"$test_root/client" <<'EOF'
#!/bin/sh
# 启动的桌面服务也只连接虚拟输出和私有总线。 / Keep activated desktop helpers on the private display, too.
dbus-update-activation-environment WAYLAND_DISPLAY XDG_CURRENT_DESKTOP XDG_RUNTIME_DIR
QT_QPA_PLATFORM=wayland "$JIETUBA_REENTRY_BINARY" --capture-reentry --virtual-kwin
echo "$?" >"$JIETUBA_REENTRY_RESULT"
EOF
chmod 700 "$test_root/client"
timeout -k 3 45 dbus-run-session -- kwin_wayland --virtual --width 900 --height 600 --socket wayland-jietuba-test \
    --no-lockscreen --no-global-shortcuts --no-kactivities --exit-with-session "$test_root/client"
test -s "$JIETUBA_REENTRY_RESULT" && exit "$(cat "$JIETUBA_REENTRY_RESULT")"
exit 1
