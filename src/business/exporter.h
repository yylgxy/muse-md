#ifndef EXPORTER_H
#define EXPORTER_H

#include <QObject>
#include <QPageLayout>
#include <QPageSize>
#include <QString>
#include <QTimer>

class QWebEnginePage;
class QTemporaryFile;

// 导出系统（5.6）：把当前文档导出成**独立**的 HTML 或 PDF。
//
// 两种导出的共同点：都先用同一条渲染管线把 Markdown 变成 HTML，再套上同一份样式。
// 所以"导出的 HTML 在浏览器里看到的"和"导出成 PDF 的"是同一份东西 —— 不是两套排版。
//
// 样式从哪来：直接从预览模板（:/html/preview_template.html）里抽那段 <style>。
// 这样预览和导出**永远一致**（改配色只改一处），而且不用把 CSS 抄第二份。
// 抽出来的那份是给屏幕上看的，导出时要再补几条覆盖规则（见 exportOverrideStyleSheet()）：
// 预览的 body 有个 60vh 的大下边距（为了让最后一段也能滚到顶部），导出成文件时
// 它只会变成一整页空白。
//
// ★ 为什么 PDF 不直接对着预览页 printToPdf，而是另起一个页面：
//   1. 预览页的内容可能正处在 300ms 防抖的等待里，直接打会打出上一版内容；
//   2. 用户可能正处在"仅编辑"模式（预览被隐藏），甚至根本没开预览；
//   3. 另起页面能保证"导出的 HTML"和"导出的 PDF"逐字节同源。
//   代价是多起一次 Chromium 页面（几百毫秒），换来的确定性很值。
//
// ★ 为什么先把 HTML 写成临时文件再 setUrl()，而不是 setHtml()：
//   QWebEnginePage::setHtml() 是把内容塞进 data: URL 加载的，**大约 2 MB 就到顶**
//   （Qt 文档明说的限制）。笔记里带两张图就超了。写文件再 setUrl() 没有这个限制。
//
// 线程/异步：exportHtml() 是同步的（纯拼字符串 + 写文件）；exportPdf() **是异步的**
//   （printToPdf 走 Chromium 的打印管线），结果通过 pdfExported() 信号回来，
//   并且带一个超时看门狗 —— Chromium 起不来时不会让界面永远等下去。
//
// PDF 的图片一律内联（HtmlExportOptions::inlineImages 对它无效）：临时文件在系统临时目录，
//   相对路径的图片在那儿必然找不到。
class Exporter : public QObject
{
    Q_OBJECT

public:
    // 单个图片内联进 HTML 的上限：超过就保持原来的相对路径（本来就是为了"独立文件"，
    // 但把一张 20MB 的图 base64 塞进 HTML 也不合理 —— 文件会大到打不开）。
    static constexpr qint64 kMaxInlineImageBytes = 4 * 1024 * 1024;

    // 生成 PDF 的超时（毫秒）。超时按失败处理：与其让用户对着"正在导出…"发呆，
    // 不如告诉他"Chromium 没能启动/页面卡住了"。
    static constexpr int kPdfTimeoutMs = 60000;

    // HTML 导出选项
    struct HtmlOptions
    {
        // 把本地图片打包成 data: URI。开着才是真正的"独立文件"（拷到别处也能看）；
        // 关掉则保留相对路径（只有把导出的 HTML 和原文放在一起才看得到图）。
        bool inlineImages = true;
        // 文档标题（进 <title>）。留空则用文件名。
        QString title;
    };

    // PDF 导出选项
    struct PdfOptions
    {
        QPageSize::PageSizeId pageSize = QPageSize::A4;
        QPageLayout::Orientation orientation = QPageLayout::Portrait;
        qreal marginMm = 12.0;  // 四边统一的边距（毫米）
    };

    // HTML 导出的统计（给界面报一句"内联了几张图"）
    struct HtmlResult
    {
        qint64 bytes = 0;        // 写出去的文件大小
        int imagesInlined = 0;   // 内联成功的图片数
        int imagesSkipped = 0;   // 没能内联（文件不存在/读不了/太大）的图片数
        bool isEmpty = false;    // 文档本身没有内容
    };

    explicit Exporter(QObject *parent = nullptr);

    // ============================ 拼 HTML（纯函数，能脱离 WebEngine 和磁盘测）============================

    // 预览模板里那段 CSS（导出的样式来源）
    static QString previewStyleSheet();

    // 导出/打印专用的覆盖规则（去掉预览的滚动留白、补上分页规则）
    static QString exportOverrideStyleSheet();

    // 把 Markdown 渲染成一份**完整、独立**的 HTML 文档：
    // <!DOCTYPE html> + meta charset + <title> + 内联样式 + 正文。
    // 注意：**不含**预览模板里的 WebChannel 脚本 —— 导出的文件不需要和 C++ 通话。
    static QString standaloneHtml(const QString &markdown, const QString &title);

    // 标题转义（文件名叫 <未命名> 也不该把 HTML 弄坏）
    static QString escapedTitle(const QString &raw);

    // 从文件路径推标题：取文件名（去掉扩展名）；空路径 → "未命名"
    static QString defaultTitleFor(const QString &filePath);

    // 把本地图片替换成 data: URI。baseDir 是文档所在目录（相对路径的基准）。
    // 已经是 data:/http(s):/qrc: 的一律不动；找不到、读不了、超过上限的也保持原样
    //（宁可在别处显示不出来，也不要导出一个坏文件或悄悄丢内容）。
    static QString inlineLocalImages(const QString &html, const QString &baseDir, HtmlResult *result = nullptr);

    static QString encodeDataUri(const QByteArray &bytes, const QString &mimeType);
    static QString mimeTypeForSuffix(const QString &suffix);

    // 导出选项 → QPageLayout（纸张/方向/边距）。抽出来是为了能单独测"选项真的生效了"。
    static QPageLayout buildPageLayout(const PdfOptions &options);

    // ============================ 导出 ============================

    // 导出 HTML（同步）。成功：true；失败：false + *error（可直接展示）。
    // 目标目录不存在会自动建（用户手打一个不存在的路径是很常见的事）。
    bool exportHtml(const QString &markdown,
                    const QString &baseDir,
                    const QString &targetPath,
                    const HtmlOptions &options = HtmlOptions(),
                    HtmlResult *result = nullptr,
                    QString *error = nullptr);

    // 导出 PDF（**异步**）：立刻返回，结果通过 pdfExported() 回来。
    // 正在导出时再调一次会直接以失败信号回来（不会排队，也不会打出两个文件互相覆盖）。
    void exportPdf(const QString &markdown,
                   const QString &baseDir,
                   const QString &targetPath,
                   const PdfOptions &options = PdfOptions(),
                   const QString &title = QString());

    bool isPdfRunning() const;

signals:
    // PDF 导出结束。ok=false 时 error 是可以直接展示给用户的原因。
    void pdfExported(const QString &path, bool ok, const QString &error);

private slots:
    void onPdfLoadFinished(bool ok);
    void onPdfPrintingFinished(const QString &path, bool ok);
    void onPdfTimeout();

private:
    // 两种导出共用的准备步骤：Markdown → 完整 HTML（图片该内联就内联）
    QString buildDocumentHtml(const QString &markdown,
                              const QString &baseDir,
                              const QString &title,
                              bool inlineImages,
                              HtmlResult *result) const;

    void finishPdf(bool ok, const QString &error);

    // PDF 用完的页面/临时文件都挂在 this 上，这里只是"借来看"，不拥有
    QWebEnginePage *m_pdfPage = nullptr;
    QTemporaryFile *m_pdfTempFile = nullptr;
    QString m_pdfTargetPath;
    QPageLayout m_pdfLayout;
    QTimer m_pdfWatchdog;  // QTimer 是值成员：生命期跟着本对象，不用 new/delete
};

#endif // EXPORTER_H
