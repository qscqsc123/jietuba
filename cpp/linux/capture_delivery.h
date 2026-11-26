#pragma once
#include "linux_settings.h"
#include "core/save.h"
#include "core/file_names.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QMimeData>
#include <QSaveFile>
#include <QUrl>

// 截图、长图和贴图共用交付路径；先独占文件名，再写入编码后的图片。
// Share file reservation and clipboard formats across stills, stitched captures and pins.
namespace LinuxCaptureDelivery {
inline QString stem(const LinuxSettings &s, const QString &prefix = {}) {
    return prefix + jietuba::file_names::render(s.captureNamePattern());
}
inline QString save(const LinuxSettings &s, const QImage &image, const QString &prefix = {}) {
    if (image.isNull() || !QDir().mkpath(s.saveDirectory())) return {};
    QString path;
    try { path = jietuba::SaveService::claim_first_free(QDir(s.saveDirectory()).filePath(stem(s,prefix)), "."+s.imageFormat()); }
    catch (const std::exception &) { return {}; }
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) && image.save(&file,s.imageFormat().toLatin1().constData()) && file.commit()) return path;
    file.cancelWriting(); jietuba::SaveService::cleanup_failed_placeholder(path); return {};
}
inline bool copy(const QImage &image, const QString &path = {}) {
    QByteArray png; QBuffer b(&png);
    if (!b.open(QIODevice::WriteOnly) || !image.save(&b,"PNG")) return false;
    auto *mime = new QMimeData;
    mime->setImageData(image); mime->setData("image/png",png);
    if (!path.isEmpty()) mime->setUrls({QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath())});
    QApplication::clipboard()->setMimeData(mime); return true;
}
inline void saveOriginal(const QString &path, const QImage &base, const QImage &result, QWidget *parent) {
    if (path.isEmpty() || base.isNull() || base.convertToFormat(QImage::Format_ARGB32)==result.convertToFormat(QImage::Format_ARGB32)) return;
    try { if (jietuba::SaveService::save_beside(base,path,"original")) return; } catch (const std::exception &) {}
    QMessageBox::warning(parent,QStringLiteral("保存原图失败"),QStringLiteral("标注图片已保存，未能写入原图副本。"));
}
}
