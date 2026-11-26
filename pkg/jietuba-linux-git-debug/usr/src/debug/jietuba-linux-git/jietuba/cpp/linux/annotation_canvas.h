#pragma once
#include <QImage>
#include <QWidget>
#include <QColor>
#include <optional>
#include <functional>
class QUndoStack;

class LinuxPinToolbar;
class LinuxPinWindow;
class LinuxSettings;
namespace jietuba {class Toolbar;}

/**
 * 截图、贴图和录制共享的标注画布。画布拥有图层和撤销栈；工具栏只承载按钮和面板。
 * 收起工具栏时结束当前编辑，保留图层，复制和保存始终按原图像素导出。
 * Shared annotation surface for captures, pins and recording. The canvas owns its document while
 * its separate toolbar hosts the controls. Ending an edit keeps the layers and
 * undo history alive; export always uses the document's native pixel dimensions.
 */
class LinuxAnnotationCanvas : public QWidget {
public:
    using QWidget::QWidget;
    static LinuxAnnotationCanvas *createForPin(LinuxPinWindow *pin, const QImage &image,
                                               LinuxSettings &settings, LinuxPinToolbar *host);
    // 录制使用透明的原版画布，设置面板归属录制表面；导出只包含标注而非控制点和工具栏。
    // Recording reuses the transparent canvas; panels belong to the recording surface and exports contain annotations only.
    static LinuxAnnotationCanvas *createForRecording(QWidget *surface,QWidget *controls,const QSize &pixels,LinuxSettings &settings);
    // 独立屏幕标注沿用同一画布，但采用 Windows ANNOTATION 工具栏和可撤销清空。
    // Screen annotation shares the canvas, with the original ANNOTATION profile and undoable clear.
    static LinuxAnnotationCanvas *createForScreenAnnotation(QWidget *surface,jietuba::Toolbar *toolbar,const QSize &pixels,LinuxSettings &settings);
    virtual void clearAnnotationsUndoable() = 0;
    // 只影响屏幕显示，白板和光标光圈不进入图层或撤销栈，也不污染录制标注导出。
    // View-only backdrop/halo never become document layers or recording-annotation exports.
    virtual void setAnnotationAppearance(bool whiteboard,std::optional<QPointF> pointer) = 0;
    virtual void activateRecordingTool(const QString &tool) = 0;
    virtual QString recordingTool() const = 0;
    virtual QUndoStack *recordingUndoStack() const = 0;
    virtual QImage recordingImage() = 0;
    virtual void layoutRecordingPanels() = 0;
    virtual void clearRecording() = 0;
    std::function<void()> recordingChanged;
    virtual QImage result() = 0;
    virtual bool pinToolActive() const = 0;
    virtual void endPinEditing() = 0;
    virtual bool editingCanvasText() const = 0;
};
