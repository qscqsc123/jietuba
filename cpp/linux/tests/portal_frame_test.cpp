#include "capture/portal_frame.h"
#include "capture/fullscreen_frame.h"
#include <QTest>
/**
 * 合成帧验证分数缩放、负屏幕坐标与多屏身份校验，不访问真实桌面。
 * Synthetic frames test fractional DPI, negative origins and monitor validation without desktop capture.
 */
class PortalFrameTest final : public QObject {
    Q_OBJECT
private slots:
    void fullscreenNativePixels() {
        QImage native(2560,1600,QImage::Format_RGB32);native.fill(QColor("#27648a"));
        native.setPixelColor(2559,1599,Qt::yellow);
        for(double tag:{1.,1.5,2.}) {
            native.setDevicePixelRatio(tag);
            const auto pixels=linuxFullscreenFrame(native,QSize(1707,1067));
            QCOMPARE(pixels.size(),QSize(2560,1600)); QCOMPARE(pixels.devicePixelRatio(),1.);
            QCOMPARE(pixels.pixelColor(2559,1599),QColor(Qt::yellow));
            QCOMPARE(pixels.pixelColor(0,0),QColor("#27648a"));
        }
    }
    void mixedOutputScales() {
        const QList<QRect> outputs{QRect(-1707,0,1707,1067),QRect(0,0,1920,1080),QRect(1920,-100,1280,800)};
        const QList<QSize> nativeSizes{QSize(2560,1600),QSize(1920,1080),QSize(2560,1600)};
        for(int i=0;i<outputs.size();++i) {
            QImage frame(nativeSizes[i],QImage::Format_RGB32);frame.fill(Qt::blue);
            QCOMPARE(linuxFullscreenFrame(frame,outputs[i].size()).size(),nativeSizes[i]);
        }
        // 一个轴恰好吻合也不能放过旋转/非等比的错误帧。
        // Reject a mismatched aspect even when one dimension happens to match.
        QVERIFY(linuxFullscreenFrame(QImage(2560,1067,QImage::Format_RGB32),QSize(1707,1067)).isNull());
        QVERIFY(linuxFullscreenFrame(QImage(1600,2560,QImage::Format_RGB32),QSize(1707,1067)).isNull());
        QVERIFY(linuxFullscreenFrame({},QSize(1707,1067)).isNull());
        QVERIFY(linuxFullscreenFrame(QImage(10,10,QImage::Format_RGB32),{}).isNull());
    }
    void fractionalCrop() {
        QImage image(200,100,QImage::Format_RGB32);
        for(int y=0;y<100;++y) for(int x=0;x<200;++x) image.setPixelColor(x,y,QColor(x,y,40));
        const QRect output(-160,0,160,80),region(-159,1,81,41);
        QString error; const auto crop=linuxPortalFrame(image,{{"position",QPoint(-160,0)},{"size",QSize(160,80)}},output,region,true,&error);
        QCOMPARE(crop.size(),QSize(102,52)); QCOMPARE(crop.devicePixelRatio(),1.25); QCOMPARE(crop.pixelColor(0,0),QColor(1,1,40)); QVERIFY(error.isEmpty());
    }
    void wrongOrAmbiguousMonitor() {
        QImage image(200,100,QImage::Format_RGB32); QString error; const QRect output(-160,0,160,80);
        QVERIFY(linuxPortalFrame(image,{},output,{},true,&error).isNull()); QVERIFY(error.contains(QStringLiteral("多屏")));
        QVERIFY(linuxPortalFrame(image,{{"position",QPoint(0,0)},{"size",QSize(160,80)}},output,{},true,&error).isNull()); QVERIFY(error.contains(QStringLiteral("不同")));
        QVERIFY(!linuxPortalFrame(image,{},output,{},false,&error).isNull());
        QVERIFY(linuxPortalFrame(image,{},output,QRect(-161,0,30,20),false,&error).isNull());
    }
};
QTEST_GUILESS_MAIN(PortalFrameTest)
#include "portal_frame_test.moc"
