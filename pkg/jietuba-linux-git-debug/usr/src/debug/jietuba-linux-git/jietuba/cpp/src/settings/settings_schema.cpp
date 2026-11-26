#include "settings/tool_settings.h"

namespace jietuba {
const std::vector<AppSetting> &APP_SETTINGS() {
    static const std::vector<AppSetting> settings = {
#define SETTING_AS(accessor, name, path, Type, initial, scope, getter, setter) {#name, path, SettingScope::scope},
#define SETTING(name, path, Type, initial, scope, getter, setter) SETTING_AS(name, name, path, Type, initial, scope, getter, setter)
#include "settings/app_settings.inc"
#undef SETTING
#undef SETTING_AS
    };
    return settings;
}

const QSet<QString> &LOCAL_SETTING_PATHS() {
    static const QSet<QString> paths = [] {
        QSet<QString> result;
        for (const auto &setting : APP_SETTINGS())
            if (setting.scope == SettingScope::Local) result.insert(setting.path);
        return result;
    }();
    return paths;
}

}
