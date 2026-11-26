#include "settings/settings_transfer.h"
#include "settings/tool_settings.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace jietuba::settings_transfer {
namespace {
QStringList paths_in(SettingScope scope) {
    QStringList paths;
    for (const auto &setting : APP_SETTINGS())
        if (setting.scope == scope) paths.append(setting.path);
    std::sort(paths.begin(), paths.end());
    return paths;
}

// A settings.ini holds every setting, so only the ones an import carries are taken.
QVariantMap read_ini(const QString &path, const QStringList &additional_keys) {
    QSettings ini(path, QSettings::IniFormat);
    QVariantMap result;
    for (const auto &key : IMPORTED_KEYS() + additional_keys)
        if (ini.contains(key)) result[key] = ini.value(key);
    if (ini.status() != QSettings::NoError) throw SettingsFileError("malformed settings file");
    if (result.isEmpty()) throw SettingsFileError("not a Jietuba settings file");
    return result;
}

QVariantMap read_json(const QByteArray &data) {
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError) throw SettingsFileError(error.errorString().toStdString());
    const auto payload = document.object();
    if (!document.isObject() || payload.value("format") != FILE_TAG || !payload.value("settings").isObject()) throw SettingsFileError("not a Jietuba settings file");
    const auto version = payload.value("version");
    const double number = version.toDouble();
    if (!version.isDouble() || number < 0 || number > FORMAT_VERSION || number != std::floor(number))
        throw SettingsFileError(QString("unsupported format version: %1").arg(QString::fromUtf8(QJsonDocument(QJsonArray{version}).toJson(QJsonDocument::Compact))).toStdString());
    QVariantMap result;
    const auto settings = payload.value("settings").toObject();
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        const auto value = it.value();
        if (it.key().isEmpty() || LOCAL_SETTING_PATHS().contains(it.key())) continue;
        if (value.isString() || value.isDouble() || value.isBool() || value.isArray()) result[it.key()] = value.toVariant();
    }
    return result;
}
}

const QStringList &IMPORTED_KEYS() {
    static const QStringList keys = paths_in(SettingScope::Import);
    return keys;
}

QVariantMap read_settings_file(const QString &path, const QStringList &additional_ini_keys) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw SettingsFileError(file.errorString().toStdString());
    QByteArray data = file.readAll();
    file.close();
    if (data.startsWith("\xEF\xBB\xBF")) data.remove(0, 3);
    return data.trimmed().startsWith('{') ? read_json(data) : read_ini(path, additional_ini_keys);
}

}
