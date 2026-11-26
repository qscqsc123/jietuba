#pragma once
#include <QDBusArgument>
#include <QDBusVariant>
#include <QImage>
#include <QRect>
#include <QVariantMap>
#include <cmath>

/**
 * 门户整屏帧到目标选区的纯转换：核对授权显示器元数据，再按实际像素外扩裁剪。
 * 多屏缺少位置/尺寸时明确拒绝，不能以相同分辨率猜测用户选中了哪块显示器。
 * Pure portal-frame validation/cropping. Verify monitor identity before converting logical bounds.
 * Reject ambiguous multi-monitor metadata rather than guessing from equal display resolutions.
 */
inline bool linuxPortalPair(QVariant value,int *x,int *y) {
    while(value.metaType()==QMetaType::fromType<QDBusVariant>()) value=qvariant_cast<QDBusVariant>(value).variant();
    if(value.metaType()==QMetaType::fromType<QDBusArgument>()) {const auto a=qvariant_cast<QDBusArgument>(value); if(a.currentSignature()!="(ii)") return false; a.beginStructure(); a>>*x>>*y; a.endStructure(); return true;}
    if(value.canConvert<QPoint>()) {*x=value.toPoint().x(); *y=value.toPoint().y(); return true;}
    if(value.canConvert<QSize>()) {*x=value.toSize().width(); *y=value.toSize().height(); return true;}
    return false;
}
inline QImage linuxPortalFrame(const QImage &image,const QVariantMap &metadata,const QRect &output,const QRect &region,bool multiple,QString *error) {
    const auto reject=[error](const QString &reason) {if(error) *error=reason; return QImage();};
    if(image.isNull() || output.isEmpty()) return reject(QStringLiteral("无法核对门户返回的目标显示器。"));
    int x=0,y=0,w=0,h=0;
    const bool position=linuxPortalPair(metadata.value("position"),&x,&y),size=linuxPortalPair(metadata.value("size"),&w,&h);
    if(multiple && (!position || !size)) return reject(QStringLiteral("桌面未提供共享屏幕的位置和尺寸，当前无法安全匹配多屏选区。请使用单显示器或支持的原生后端。"));
    if((position && QPoint(x,y)!=output.topLeft()) || (size && QSize(w,h)!=output.size()))
        return reject(QStringLiteral("共享的显示器与框选显示器不同，请重新共享框选所在的显示器。"));
    const double sx=double(image.width())/output.width(),sy=double(image.height())/output.height();
    if(std::abs(sx-sy)>.02*std::max(sx,sy)) return reject(QStringLiteral("共享画面的宽高比例与目标显示器不符。"));
    QImage result=image;
    if(!region.isEmpty()) {
        if(!output.contains(region)) return reject(QStringLiteral("门户区域坐标超出共享屏幕。"));
        const auto local=region.translated(-output.topLeft());
        const int left=int(std::floor(local.x()*sx+1e-7)),top=int(std::floor(local.y()*sy+1e-7));
        const int right=int(std::ceil((local.x()+local.width())*sx-1e-7)),bottom=int(std::ceil((local.y()+local.height())*sy-1e-7));
        result=image.copy(QRect(left,top,right-left,bottom-top));
    }
    result.setDevicePixelRatio(sx); return result;
}
