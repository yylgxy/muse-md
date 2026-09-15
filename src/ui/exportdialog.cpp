#include "exportdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

ExportDialog::ExportDialog(Format format, const QString &suggestedPath, QWidget *parent)
    : QDialog(parent), m_title(Exporter::defaultTitleFor(suggestedPath))
{
    setWindowTitle(QStringLiteral("导出文档"));
    setModal(true);

    // ---------------- 格式 ----------------
    m_formatBox = new QComboBox(this);
    m_formatBox->addItem(QStringLiteral("HTML（网页，浏览器直接打开 / 拷给别人）"));
    m_formatBox->addItem(QStringLiteral("PDF（打印稿，排版固定）"));
    m_formatBox->setCurrentIndex(format == Format::Pdf ? 1 : 0);

    // ---------------- 保存路径 ----------------
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setText(QDir::toNativeSeparators(suggestedPath));
    m_pathEdit->setMinimumWidth(360);
    m_pathEdit->setPlaceholderText(QStringLiteral("导出到哪里"));

    auto *browseButton = new QPushButton(QStringLiteral("浏览…"), this);

    auto *pathRow = new QHBoxLayout;
    pathRow->setContentsMargins(0, 0, 0, 0);
    pathRow->addWidget(m_pathEdit, 1);
    pathRow->addWidget(browseButton);

    // ---------------- HTML 选项 ----------------
    auto *htmlGroup = new QGroupBox(QStringLiteral("HTML 选项"), this);
    m_inlineImages = new QCheckBox(QStringLiteral("把图片一起打包进文件（推荐）"), htmlGroup);
    m_inlineImages->setChecked(true);
    m_inlineImages->setToolTip(QStringLiteral("勾上：图片变成文件里的一段数据，拷到任何地方都能看。\n"
                                             "不勾：图片保持相对路径，只有和原文放在一起才看得到。"));
    auto *htmlLayout = new QVBoxLayout(htmlGroup);
    htmlLayout->addWidget(m_inlineImages);

    // ---------------- PDF 选项 ----------------
    auto *pdfGroup = new QGroupBox(QStringLiteral("PDF 选项"), this);
    m_pageSize = new QComboBox(pdfGroup);
    m_pageSize->addItem(QStringLiteral("A4（210 × 297 mm）"), int(QPageSize::A4));
    m_pageSize->addItem(QStringLiteral("A3（297 × 420 mm）"), int(QPageSize::A3));
    m_pageSize->addItem(QStringLiteral("Letter（8.5 × 11 in）"), int(QPageSize::Letter));
    m_pageSize->addItem(QStringLiteral("Legal（8.5 × 14 in）"), int(QPageSize::Legal));

    m_orientation = new QComboBox(pdfGroup);
    m_orientation->addItem(QStringLiteral("纵向"));
    m_orientation->addItem(QStringLiteral("横向"));

    m_margin = new QDoubleSpinBox(pdfGroup);
    m_margin->setRange(0.0, 50.0);
    m_margin->setSingleStep(1.0);
    m_margin->setDecimals(0);
    m_margin->setValue(12.0);
    m_margin->setSuffix(QStringLiteral(" mm"));

    auto *pdfForm = new QFormLayout(pdfGroup);
    pdfForm->addRow(QStringLiteral("纸张"), m_pageSize);
    pdfForm->addRow(QStringLiteral("方向"), m_orientation);
    pdfForm->addRow(QStringLiteral("页边距"), m_margin);

    // ---------------- 汇总说明 + 按钮 ----------------
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    if (QPushButton *saveButton = m_buttons->button(QDialogButtonBox::Save)) {
        saveButton->setText(QStringLiteral("导出"));
    }

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("格式"), m_formatBox);
    form->addRow(QStringLiteral("保存到"), pathRow);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(htmlGroup);
    layout->addWidget(pdfGroup);
    layout->addWidget(m_summary);
    layout->addWidget(m_buttons);

    // ---------------- 接线 ----------------
    connect(m_formatBox, &QComboBox::currentIndexChanged, this, &ExportDialog::onFormatChanged);
    connect(browseButton, &QPushButton::clicked, this, &ExportDialog::onBrowseClicked);
    connect(m_inlineImages, &QCheckBox::toggled, this, &ExportDialog::updateSummary);
    connect(m_pageSize, &QComboBox::currentIndexChanged, this, &ExportDialog::updateSummary);
    connect(m_orientation, &QComboBox::currentIndexChanged, this, &ExportDialog::updateSummary);
    connect(m_margin, &QDoubleSpinBox::valueChanged, this, &ExportDialog::updateSummary);
    connect(m_pathEdit, &QLineEdit::textChanged, this, &ExportDialog::updateSummary);
    // 确定前挡一道：路径为空就别往下走（否则导出必然失败，还得多弹一次错误框）
    connect(m_buttons, &QDialogButtonBox::accepted, this, [this] {
        if (m_pathEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("还差一步"), QStringLiteral("请先选一个保存位置。"));
            return;
        }
        accept();
    });
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    onFormatChanged();  // 初始化两个选项组的可用状态 + 汇总文字
}

// ============================ 请求 ============================

ExportDialog::Request ExportDialog::request() const
{
    Request result;
    result.format = (m_formatBox->currentIndex() == 1) ? Format::Pdf : Format::Html;
    result.targetPath = m_pathEdit->text().trimmed();
    result.title = m_title;

    result.html.inlineImages = m_inlineImages->isChecked();

    result.pdf.pageSize = static_cast<QPageSize::PageSizeId>(m_pageSize->currentData().toInt());
    result.pdf.orientation = (m_orientation->currentIndex() == 1) ? QPageLayout::Landscape : QPageLayout::Portrait;
    result.pdf.marginMm = m_margin->value();

    return result;
}

void ExportDialog::setRequest(const Request &request)
{
    m_formatBox->setCurrentIndex(request.format == Format::Pdf ? 1 : 0);

    if (!request.targetPath.isEmpty()) {
        m_pathEdit->setText(QDir::toNativeSeparators(request.targetPath));
    }
    if (!request.title.isEmpty()) {
        m_title = request.title;
    }

    m_inlineImages->setChecked(request.html.inlineImages);

    const int pageIndex = m_pageSize->findData(int(request.pdf.pageSize));
    if (pageIndex >= 0) {
        m_pageSize->setCurrentIndex(pageIndex);
    }
    m_orientation->setCurrentIndex(request.pdf.orientation == QPageLayout::Landscape ? 1 : 0);
    m_margin->setValue(request.pdf.marginMm);

    onFormatChanged();
}

// ============================ 纯函数 ============================

QString ExportDialog::suggestedPathFor(const QString &documentPath, Format format)
{
    const QString suffix = (format == Format::Pdf) ? QStringLiteral("pdf") : QStringLiteral("html");
    if (documentPath.trimmed().isEmpty()) {
        return QStringLiteral("未命名.") + suffix;
    }

    const QFileInfo info(documentPath);
    const QString baseName = info.completeBaseName().isEmpty() ? QStringLiteral("未命名") : info.completeBaseName();
    const QString name = baseName + QLatin1Char('.') + suffix;

    const QString dirPart = info.path();
    if (dirPart.isEmpty() || dirPart == QLatin1String(".")) {
        return name;  // 只给了文件名：目录由调用方决定
    }
    return QDir(dirPart).filePath(name);
}

QString ExportDialog::fileFilterFor(Format format)
{
    return (format == Format::Pdf) ? QStringLiteral("PDF 文件 (*.pdf);;所有文件 (*)")
                                   : QStringLiteral("网页文件 (*.html *.htm);;所有文件 (*)");
}

// ============================ 内部 ============================

void ExportDialog::onFormatChanged()
{
    const bool isPdf = (m_formatBox->currentIndex() == 1);

    // 只让当前格式相关的选项可用：另一个格式的设置先"冻"着，不改它的值
    m_pageSize->setEnabled(isPdf);
    m_orientation->setEnabled(isPdf);
    m_margin->setEnabled(isPdf);
    m_inlineImages->setEnabled(!isPdf);
    m_inlineImages->setToolTip(isPdf ? QStringLiteral("导出 PDF 时图片总会一起打包（临时文件在别处，相对路径会失效）")
                                     : QStringLiteral("勾上：图片变成文件里的一段数据，拷到任何地方都能看。\n"
                                                       "不勾：图片保持相对路径，只有和原文放在一起才看得到。"));

    applyFormatToPath();  // 换格式时把后缀跟着换掉（a.md → a.html / a.pdf）
    updateSummary();
}

void ExportDialog::applyFormatToPath()
{
    QString path = m_pathEdit->text().trimmed();
    if (path.isEmpty()) {
        return;
    }

    const QFileInfo info(path);
    const QString suffix = (m_formatBox->currentIndex() == 1) ? QStringLiteral("pdf") : QStringLiteral("html");
    if (info.suffix().compare(suffix, Qt::CaseInsensitive) == 0) {
        return;  // 后缀已经对了，不动用户可能手改过的名字
    }

    const QString baseName = info.completeBaseName().isEmpty() ? QStringLiteral("未命名") : info.completeBaseName();
    const QString name = baseName + QLatin1Char('.') + suffix;
    const QString dirPart = info.path();
    path = (dirPart.isEmpty() || dirPart == QLatin1String(".")) ? name : QDir(dirPart).filePath(name);
    m_pathEdit->setText(QDir::toNativeSeparators(path));
}

void ExportDialog::onBrowseClicked()
{
    const Format format = (m_formatBox->currentIndex() == 1) ? Format::Pdf : Format::Html;
    const QString current = m_pathEdit->text().trimmed();
    const QString chosen = QFileDialog::getSaveFileName(this,
                                                        QStringLiteral("导出到"),
                                                        current.isEmpty() ? QDir::homePath() : current,
                                                        fileFilterFor(format));
    if (!chosen.isEmpty()) {
        m_pathEdit->setText(QDir::toNativeSeparators(chosen));
    }
}

void ExportDialog::updateSummary()
{
    const Request current = request();
    const QString where = current.targetPath.isEmpty() ? QStringLiteral("（还没选保存位置）")
                                                       : QDir::toNativeSeparators(current.targetPath);

    if (current.format == Format::Html) {
        m_summary->setText(QStringLiteral("会把当前文档导出成一份独立的网页：样式内联，%1。\n保存到：%2")
                               .arg(current.html.inlineImages ? QStringLiteral("图片一起打包进文件，拷到别处也能看")
                                                             : QStringLiteral("图片保持相对路径（要和原文放在一起才看得到）"),
                                    where));
    } else {
        m_summary->setText(QStringLiteral("会生成 %1、%2、页边距 %3 mm 的 PDF。\n保存到：%4")
                               .arg(m_pageSize->currentText().section(QStringLiteral("（"), 0, 0),
                                    current.pdf.orientation == QPageLayout::Landscape ? QStringLiteral("横向")
                                                                                      : QStringLiteral("纵向"),
                                    QString::number(current.pdf.marginMm, 'f', 0),
                                    where));
    }
}
