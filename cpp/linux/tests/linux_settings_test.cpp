#include "linux_settings.h"
#include "autostart.h"
#include <QFile>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cstdio>
/**
 * 使用独立 INI 检查设置写入、重启恢复和异常配置回退，不碰用户配置。
 * Exercises persistence, restart and invalid values in an isolated INI file.
 */
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) return 1;
    const auto file = dir.filePath("settings.ini");
    {
        LinuxSettings settings(file);
        settings.setSaveDirectory(dir.path()); settings.setImageFormat("jpg");
        settings.setCaptureAllScreens(false); settings.setTheme("dark"); settings.setBorderWidth(8);
        settings.setGifFps(24); settings.setGifDelay(3);
        if (!settings.saveToolStyle("pen",{{"width",9},{"color","#ff123456"},{"opacity",128}})) return 13;
        if (settings.saveToolStyle("unknown",{{"width",9}})) return 14;
        settings.setMagnifierOption("grid",true); settings.setMagnifierOption("selection",true);
        settings.setMagnifierZoom(6.25); settings.setMagnifierFormats({"HEX","CSS rgb()"});
        QString error;
        if (settings.setShortcut("pen","Ctrl+C",&error) || error.isEmpty() || settings.setShortcut("pen","Escape")) return 15;
        if (!settings.setShortcut("pen","F2") || !settings.setShortcut("pin_copy","F2") || !settings.setShortcut("barcode","")) return 16;
        settings.setLastRegion(QRect(10,20,300,200),QSize(800,600),"test-output");
        if (!settings.saveScreenshotToolbarLayout(QJsonArray{QJsonArray{"pen","show"},QJsonArray{"save","more"}})) return 10;
        settings.setPinBorderEnabled(false); settings.setPinZoomStep(25);
        if (settings.pinAutoToolbar() || settings.pinHoverButtons()) return 23;
        if(settings.globalShortcutSlots("translate").value(0)!="Alt+3" || settings.globalShortcutSlots("screen_annotation").value(0)!="Alt+4" || !settings.globalShortcutSlots("ocr").value(0).isEmpty()) return 28;
        settings.setGlobalShortcutSlots("translate",QStringList{QString(),"F11",QString(),"Ctrl+F12"});
        settings.setPinAutoToolbar(true);
        settings.setPinHoverButtons(true);
        if (!settings.savePinToolbarLayout(QJsonArray{QJsonArray{"copy", "more"}, QJsonArray{"save", "show"}})) return 24;
        settings.setWatermarkText(QStringLiteral("测试水印")); settings.setWatermarkStyle({{QStringLiteral("layout"), QStringLiteral("center")}, {QStringLiteral("opacity"), .5}});
        settings.setTranslationProvider(QStringLiteral("libretranslate"));
        settings.setTranslationEndpoint(QStringLiteral("http://127.0.0.1:5000/translate"));
        settings.setTranslationApiKey(QStringLiteral("secret")); settings.setTranslationTarget(QStringLiteral("en"));
        settings.setGoogleApiKey(QStringLiteral("google-test"));
        auto video=settings.videoOptions(); video.fps=48; video.bitrate=8000000; video.system_audio=false; video.hardware=false; video.cursor=false;
        settings.setVideoOptions(video); settings.setRecordingMode("mp4");
        settings.setYoudaoAppId(QStringLiteral("youdao-id")); settings.setYoudaoAppSecret(QStringLiteral("youdao-secret"));
        if (settings.sync() != QSettings::NoError) return 2;
        if (QFileInfo(file).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) return 5;
    }
    {
        LinuxSettings settings(file);
        if(settings.globalShortcutSlots("translate")!=QStringList{QString(),"F11",QString(),"Ctrl+F12"}) return 29;
        if (settings.gifFps()!=24 || settings.gifDelay()!=3) return 8;
        const auto video=settings.videoOptions();
        if(settings.recordingMode()!="mp4" || video.fps!=48 || video.bitrate!=8000000 || video.system_audio || video.hardware || video.cursor) return 27;
        if (settings.toolStyle("pen").value("width").toInt()!=9 || settings.toolStyle("pen").value("color")!="#ff123456") return 17;
        if (!settings.magnifierOption("grid") || !settings.magnifierOption("selection") || settings.magnifierZoom()!=6.25
            || settings.magnifierFormats()!=QStringList{"HEX","CSS rgb()"}) return 18;
        if (settings.shortcut("pen")!="F2" || settings.shortcut("pin_copy")!="F2" || !settings.shortcut("barcode").isEmpty()) return 19;
        if (settings.lastRegion(QSize(800,600),"test-output")!=QRect(10,20,300,200)
            || !settings.lastRegion(QSize(600,800),"test-output").isEmpty() || !settings.lastRegion(QSize(800,600),"other").isEmpty()) return 20;
        settings.resetShortcuts(); if (settings.shortcut("pen")!="P" || settings.shortcut("copy")!="Ctrl+C") return 21;
        if (settings.screenshotToolbarLayout()!=QJsonArray{QJsonArray{"pen","show"},QJsonArray{"save","more"}}) return 11;
        if (!settings.pinAutoToolbar() || !settings.pinHoverButtons()
            || settings.pinToolbarLayout()!=QJsonArray{QJsonArray{"copy", "more"}, QJsonArray{"save", "show"}}) return 25;
        if (settings.saveDirectory() != dir.path() || settings.imageFormat() != "jpg" || settings.captureAllScreens() || settings.theme() != "dark" || settings.borderWidth() != 8 || settings.pinBorderEnabled() || settings.pinZoomStep() != 25 || settings.watermarkText()!=QStringLiteral("测试水印") || settings.watermarkStyle().value("layout").toString()!="center" || settings.watermarkStyle().value("opacity").toDouble()!=.5) return 3;
        if (settings.translationProvider() != "libretranslate" || settings.translationEndpoint() != "http://127.0.0.1:5000/translate"
            || settings.translationApiKey() != "secret" || settings.translationTarget() != "en") return 6;
        if (settings.googleApiKey() != "google-test" || settings.youdaoAppId() != "youdao-id"
            || settings.youdaoAppSecret() != "youdao-secret") return 7;
    }
    {
        QSettings broken(file, QSettings::IniFormat);
        broken.setValue("capture/image_format", "invalid"); broken.setValue("appearance/theme", "invalid"); broken.setValue("appearance/border_width", 999);
        broken.setValue("translation/provider", "invalid"); broken.setValue("translation/target", "invalid"); broken.setValue("pin/zoom_step", 999); broken.sync();
        // 手改配置文件也不能产生除零帧间隔或无限倒计时。
        // Hand-edited settings must not produce invalid frame intervals or countdowns.
        broken.setValue("recording/gif_fps",0); broken.setValue("recording/delay",-1); broken.sync();
        broken.setValue("toolbar/screenshot_layout",QByteArray("not-json")); broken.sync();
        broken.setValue("toolbar/pin_layout", QByteArray("not-json")); broken.sync();
        broken.setValue("tools/pen",QByteArray("not-json")); broken.setValue("magnifier/zoom","nan");
        broken.setValue("magnifier/formats",QStringList{"unknown"}); broken.setValue("shortcuts/pen","Escape"); broken.sync();
    }
    LinuxSettings restored(file);
    if (restored.gifFps()!=16 || restored.gifDelay()!=0) return 9;
    if (!restored.screenshotToolbarLayout().isEmpty()) return 12;
    if (!restored.pinToolbarLayout().isEmpty()) return 26;
    if (!restored.toolStyle("pen").isEmpty() || restored.magnifierZoom()!=4 || restored.magnifierFormats()!=QStringList{"RGB","HEX"} || restored.shortcut("pen")!="P") return 22;
    if (restored.imageFormat() != "png" || restored.theme() != "system" || restored.borderWidth() != 4 || restored.pinZoomStep() != 10
        || restored.translationProvider() != "deepl_free" || restored.translationTarget() != "zh") return 4;

    // 迁移使用原版键名；本机目录和桌面已授权的组合键不随文件覆盖。
    // Windows preferences travel; machine-local directories and desktop grants stay put.
    const auto windowsFile = dir.filePath("windows.ini");
    {
        QSettings windows(windowsFile, QSettings::IniFormat);
        windows.setValue("app/language", "ja");
        windows.setValue("app/show_main_window", false);
        windows.setValue("app/screenshot_format", "JPG");
        windows.setValue("app/screenshot_save_path", "D:/captures");
        windows.setValue("app/translation_hotkey", "ctrl+9");
        windows.setValue("inapp/inapp_tool_pen", "f3");
        windows.setValue("app/screenshot_toolbar_layout", R"([["pen","show"]])");
    }
    if (!restored.importPreferences(windowsFile).isEmpty()) return 30;
    LinuxSettings migrated(file);
    if (migrated.language() != "ja" || migrated.showMainWindowOnStartup() || migrated.imageFormat() != "jpg"
        || migrated.saveDirectory() != dir.path() || migrated.globalShortcutSlots("translate").value(1) != "F11"
        || migrated.shortcut("pen") != "F3" || migrated.screenshotToolbarLayout() != QJsonArray{QJsonArray{"pen", "show"}}) return 31;

    QFile legacy(dir.filePath("legacy.json"));
    if (!legacy.open(QIODevice::WriteOnly)) return 32;
    legacy.write(R"({"format":"jietuba-settings","version":1,"settings":{"app/ui_theme_mode":"dark","app/language":"ko"}})");
    legacy.close();
    if (!migrated.importPreferences(legacy.fileName()).isEmpty() || migrated.theme() != "dark" || migrated.language() != "ko") return 33;

    const auto nativeFile = dir.filePath("backup.ini");
    LinuxSettings portable(nativeFile);
    portable.setGifDelay(5);
    portable.setSaveDirectory(dir.filePath("other"));
    if (!portable.saveToolStyle("pen", {{"width", 7}}) || !migrated.importPreferences(nativeFile).isEmpty()
        || migrated.gifDelay() != 5 || migrated.toolStyle("pen").value("width").toInt() != 7 || migrated.saveDirectory() != dir.path()) return 34;

    QFile corrupt(dir.filePath("broken.ini"));
    if (!corrupt.open(QIODevice::WriteOnly)) return 35;
    corrupt.write("[app]\nlanguage=zh\n[missing-close\n");
    corrupt.close();
    if (migrated.importPreferences(corrupt.fileName()).isEmpty() || migrated.language() != "ko") return 36;

    qputenv("XDG_CONFIG_HOME", dir.filePath("config").toUtf8());
    qputenv("XDG_CONFIG_DIRS", dir.filePath("global").toUtf8());
    if (LinuxAutostart::isEnabled() || !LinuxAutostart::setEnabled(true).isEmpty() || !LinuxAutostart::isEnabled()) return 37;
    const auto entryPath = dir.filePath("config/autostart/io.github.jietuba.LinuxPreview.desktop");
    {
        QSettings entry(entryPath, QSettings::IniFormat);
        if (!entry.value("Desktop Entry/Exec").toString().contains(QCoreApplication::applicationFilePath())) return 38;
        entry.setValue("Desktop Entry/Hidden", true);
    }
    if (LinuxAutostart::isEnabled() || !LinuxAutostart::setEnabled(false).isEmpty() || QFileInfo::exists(entryPath)) return 39;
    std::fprintf(stderr, "Linux settings persistence and validation passed\n");
    return 0;
}
