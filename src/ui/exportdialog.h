#ifndef EXPORTDIALOG_H
#define EXPORTDIALOG_H

#include <QDialog>
#include <QString>

#include "exporter.h"  // 选项结构体是本对话框的返回值的一部分，需要完整定义

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;

// 导出对话框（5.6）：选格式、选保存路径、配置几项参数。
//
// 它只做一件事：**把用户的意图收集成一个 Exporter 能直接用的请求**。
// 真正的导出在 Exporter 里（business 层，不弹窗、能单独测），
// 这里不碰文件内容，也不调用 WebEngine —— 所以这个对话框可以脱离 Chromium 单独构造和验证
// （测试就是直接 `ExportDialog dialog(...); dialog.request();` 这样读它的返回值）。
//
// 界面上刻意保持简单（需求原话是"简单配置"）：格式 + 路径 + 每种格式各一两项。
// 高级选项（页边距、纸张、图片内联）都给了能用的默认值，不改也能导出好结果。
class ExportDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Format {
        Html,
        Pdf,
    };

    // 一次导出的完整请求（对话框的"输出"）
    struct Request
    {
        Format format = Format::Html;
        QString targetPath;
        Exporter::HtmlOptions html;
        Exporter::PdfOptions pdf;
        QString title;  // 文档标题（进 HTML 的 <title>；PDF 也用它做文档名）
    };

    // format：初始格式；suggestedPath：建议的保存路径（一般是"文档路径换个后缀"）
    ExportDialog(Format format, const QString &suggestedPath, QWidget *parent = nullptr);

    // 当前设置。**没点确定也能读** —— 测试和"预览即将导出的设置"都靠它。
    Request request() const;
    void setRequest(const Request &request);

    // 文档路径 → 建议的导出路径（换后缀）。抽成 static 纯函数便于单独测。
    // 传空路径时只返回文件名（目录由调用方决定）。
    static QString suggestedPathFor(const QString &documentPath, Format format);

    // 这种格式默认用什么文件过滤器（"另存为"对话框要用）
    static QString fileFilterFor(Format format);

private slots:
    void onFormatChanged();
    void onBrowseClicked();
    void updateSummary();

private:
    void applyFormatToPath();

    // 文档标题（进 HTML 的 <title>）。构造时从建议路径推出来，setRequest() 可以覆盖。
    QString m_title;

    QComboBox *m_formatBox = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QLabel *m_summary = nullptr;

    // HTML 选项
    QCheckBox *m_inlineImages = nullptr;

    // PDF 选项
    QComboBox *m_pageSize = nullptr;
    QComboBox *m_orientation = nullptr;
    QDoubleSpinBox *m_margin = nullptr;

    QDialogButtonBox *m_buttons = nullptr;
};

#endif // EXPORTDIALOG_H
