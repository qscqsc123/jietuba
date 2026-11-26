#pragma once

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QPointer>
#include <QString>

class QNetworkReply;

struct LinuxTranslationConfig {
    QString provider;
    QString apiKey;
    QString endpoint;
    QString target;
    QString googleApiKey;
    QString youdaoAppId;
    QString youdaoAppSecret;
};

struct LinuxTranslationResult {
    QString text;
    QString detectedLanguage;
    QString error;
};

struct LinuxTranslationPreparation {
    QNetworkRequest request;
    QByteArray body;
    QString error;
};

/**
 * Linux 翻译 HTTP 适配器。支持 DeepL、Google、有道和自建 LibreTranslate。
 * 请求与解析独立于网络，便于用本地样例验证。只有明确点击翻译后才会发送 OCR 文字。
 *
 * Asynchronous HTTP adapter for DeepL, Google, Youdao and LibreTranslate.
 * Request creation and response parsing are testable without network access.
 */
class LinuxTranslationClient final : public QObject {
    Q_OBJECT
public:
    explicit LinuxTranslationClient(QObject *parent = nullptr);
    static LinuxTranslationPreparation prepare(const QString &text, const LinuxTranslationConfig &config);
    static LinuxTranslationResult parse(const QString &provider, int httpStatus, const QByteArray &response);
    void translate(const QString &text, const LinuxTranslationConfig &config);
    bool isWorking() const { return !current_.isNull(); }
signals:
    void finished(const LinuxTranslationResult &result);
private:
    QNetworkAccessManager manager_;
    QPointer<QNetworkReply> current_;
};
