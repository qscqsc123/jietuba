#include "autostart.h"
#include "global_shortcut.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

namespace {
QString entryPath(const QString &configDirectory) {
    return QDir(configDirectory).filePath("autostart/" + GlobalShortcut::applicationId() + ".desktop");
}

bool entryEnabled(const QString &path) {
    QSettings entry(path, QSettings::IniFormat);
    return entry.value("Desktop Entry/Type").toString() == "Application"
        && !entry.value("Desktop Entry/Exec").toString().isEmpty()
        && !entry.value("Desktop Entry/Hidden", false).toBool()
        && entry.value("Desktop Entry/X-GNOME-Autostart-enabled", true).toBool();
}

QString desktopExec(QString program) {
    // Exec 先解码 desktop-entry 的反斜杠，再拆分参数并展开 % 字段。
    // Desktop entries decode backslashes before parsing Exec arguments and percent codes.
    program.replace('\\', QStringLiteral("\\\\"));
    for (const QChar character : {QChar('"'), QChar('`'), QChar('$')})
        program.replace(character, QStringLiteral("\\") + character);
    program.replace('%', QStringLiteral("%%"));
    program.replace('\\', QStringLiteral("\\\\"));
    program.replace('\n', QStringLiteral("\\n")).replace('\r', QStringLiteral("\\r"));
    return '"' + program + '"';
}
}

bool LinuxAutostart::isEnabled() {
    for (const auto &directory : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation)) {
        const auto path = entryPath(directory);
        if (QFileInfo::exists(path)) return entryEnabled(path);
    }
    return false;
}

QString LinuxAutostart::setEnabled(bool enabled) {
    const auto userDirectory = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    const auto path = entryPath(userDirectory);
    if (!enabled) {
        bool hasSystemEntry = false;
        for (const auto &directory : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))
            if (directory != userDirectory && QFileInfo::exists(entryPath(directory))) hasSystemEntry = true;
        if (!hasSystemEntry) {
            QFile entry(path);
            if (QFileInfo::exists(path) && !entry.remove()) return entry.errorString();
            return {};
        }
        // 系统级启动项不能删除；由同名用户条目覆盖。
        // A same-named user entry masks an administrator-provided entry.
    }

    if (userDirectory.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) return path;
    QSaveFile entry(path);
    if (!entry.open(QIODevice::WriteOnly)) return entry.errorString();
    const auto contents = QStringLiteral("[Desktop Entry]\nType=Application\nName=Jietuba\nName[zh_CN]=截图吧\n"
                                         "Exec=%1\nIcon=%2\nTerminal=false\nHidden=%3\n")
                              .arg(desktopExec(QCoreApplication::applicationFilePath()), GlobalShortcut::applicationId(),
                                   enabled ? QStringLiteral("false") : QStringLiteral("true")).toUtf8();
    if (entry.write(contents) != contents.size() || !entry.commit()) return entry.errorString();
    return {};
}
