#include "capture/portal_frame.h"
#include <QTest>
/**
 * 合成帧验证分数缩放、负屏幕坐标与多屏身份校验，不访问真实桌面。
 * Synthetic frames test fractional DPI, negative origins and monitor validation without desktop capture.
 */
class PortalFrameTest final : public QObject {
    Q_OBJECT
private slots:
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
