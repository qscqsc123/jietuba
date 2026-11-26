#pragma once

#include <QString>

// 用 XDG 登录启动项保存状态，桌面设置与应用开关读取的是同一份配置。
// The XDG login entry is the source of truth for both the desktop and the app.
namespace LinuxAutostart {
bool isEnabled();
QString setEnabled(bool enabled);
}
