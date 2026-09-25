#include "diffview.h"

#include <QHBoxLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QStringList>

#include "linediff.h"

using markdown_editor::core::document::LineDiff;

namespace {

// 三个样式：Delete / Insert / Context。用 int 枚举，方便 addRow 里 switch。
enum Style
{
    StyleContext = 0,
    StyleDelete = 1,
    StyleInsert = 2,
};

// 行号固定 6 个字符宽：差异很少超过 99 万行，6 位足够右对齐。
constexpr int kLineNoWidth = 6;

}  // namespace

DiffView::DiffView(QWidget *parent) : QWidget(parent)
{
    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setWordWrap(false);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    // 等宽字体：行号对齐和 diff 的可读性都靠它。
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(10);
    m_list->setFont(mono);

    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(m_list);

    connect(m_list, &QListWidget::itemDoubleClicked, this, &DiffView::onItemDoubleClicked);
}

void DiffView::setDiff(const LineDiff::Result &result,
                       const QStringList &oldLines,
                       const QStringList &newLines)
{
    m_list->clear();

    for (const LineDiff::Hunk &h : result.hunks) {
        switch (h.kind) {
        case LineDiff::Kind::Equal:
            for (int i = 0; i < h.oldCount; ++i) {
                const int oldLine = h.oldStart + i;
                const int newLine = h.newStart + i;
                // 上下文行取旧行内容（两侧相等，取哪侧都一样）；行号两侧都标。
                addRow(oldLines[oldLine - 1], StyleContext, oldLine, newLine);
            }
            break;
        case LineDiff::Kind::Delete:
            for (int i = 0; i < h.oldCount; ++i) {
                const int oldLine = h.oldStart + i;
                addRow(oldLines[oldLine - 1], StyleDelete, oldLine, -1);
            }
            break;
        case LineDiff::Kind::Insert:
            for (int i = 0; i < h.newCount; ++i) {
                const int newLine = h.newStart + i;
                addRow(newLines[newLine - 1], StyleInsert, -1, newLine);
            }
            break;
        }
    }
}

int DiffView::rowCount() const
{
    return m_list->count();
}

void DiffView::addRow(const QString &text, int style, int oldNo, int newNo)
{
    // 行号前缀："  oldNo│  newNo│ "，两侧都是右对齐、固定宽度，缺失一侧填空格。
    const QString oldStr = (oldNo > 0) ? QString::number(oldNo).rightJustified(kLineNoWidth) : QString(kLineNoWidth, QLatin1Char(' '));
    const QString newStr = (newNo > 0) ? QString::number(newNo).rightJustified(kLineNoWidth) : QString(kLineNoWidth, QLatin1Char(' '));
    // 用 '│'（竖线，全角）分隔，肉眼能区分"行号"和"内容"。
    const QString prefix = oldStr + QStringLiteral(" \u2502 ") + newStr + QStringLiteral(" \u2502 ");

    auto *item = new QListWidgetItem(prefix + text);
    // 把"新行号"存进 item 的数据里，双击时读出来跳转；没有对应新行存 -1。
    item->setData(Qt::UserRole, newNo);

    // 前景/背景着色：只在 Delete / Insert 两种样式里设，Context 用默认（跟随主题）。
    switch (style) {
    case StyleDelete:
        item->setBackground(QColor(0xff, 0xd7, 0xd5));
        item->setForeground(QColor(0xb3, 0x00, 0x00));
        break;
    case StyleInsert:
        item->setBackground(QColor(0xd4, 0xfc, 0xd4));
        item->setForeground(QColor(0x00, 0x6d, 0x00));
        break;
    default:
        break;
    }

    m_list->addItem(item);
}

void DiffView::onItemDoubleClicked()
{
    QListWidgetItem *item = m_list->currentItem();
    if (item == nullptr) {
        return;
    }
    const int newLine = item->data(Qt::UserRole).toInt();
    emit lineActivated(newLine);
}
