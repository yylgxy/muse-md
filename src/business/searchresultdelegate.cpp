#include "searchresultdelegate.h"

#include <QApplication>
#include <QFontMetrics>
#include <QPainter>
#include <QPalette>
#include <QPoint>
#include <QStyle>
#include <QStyleOptionViewItem>

SearchResultDelegate::SearchResultDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

QColor SearchResultDelegate::matchBackground(const QPalette &palette, bool selected)
{
    // 强调色取自视图调色板 —— 主题换色时这里自动跟着走，
    // 不用让面板去认识 ThemeManager（面板至今没引用过它，这个分工不该破）。
    QColor accent = palette.color(QPalette::Highlight);
    if (!accent.isValid()) {
        accent = QColor(0x09, 0x69, 0xda);  // 兜底：和亮色 QSS 里的强调色一致
    }

    if (selected) {
        // 整行已经铺了实心强调色，命中处再用强调色就看不见了 → 换成"提亮"。
        QColor lightened = accent.lighter(160);
        lightened.setAlpha(180);
        return lightened;
    }

    // 未选中：半透明强调色。**必须半透明** —— 实心色会盖住交替行的底色，
    // 视觉上整行像被选中了，用户会以为鼠标点错了行。
    accent.setAlpha(64);
    return accent;
}

QColor SearchResultDelegate::matchForeground(const QPalette &palette, bool selected)
{
    if (selected) {
        return palette.color(QPalette::HighlightedText);
    }
    // 未选中时用正常文字色（只是加粗），不强改色相：
    // 内容列本来就不该在颜色上抢注意，底色已经足够指出"是哪几个字"了。
    return palette.color(QPalette::Text);
}

void SearchResultDelegate::paint(QPainter *painter,
                                const QStyleOptionViewItem &option,
                                const QModelIndex &index) const
{
    const QPoint span = index.data(kMatchSpanRole).toPoint();

    // 没有命中区间（文件分组行、或者这条结果没定位到命中）→ 完全交给基类。
    if (span.y() <= 0) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    const QString text = opt.text;
    opt.text.clear();  // 先让基类只画"底子"：背景、选中块、焦点框

    const QWidget *widget = opt.widget;
    QStyle *style = (widget != nullptr) ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    // ---- 到这里背景已经画好了，下面自己把文字画上去 ----
    const bool selected = opt.state & QStyle::State_Selected;
    const int start = qBound(0, span.x(), text.size());
    const int length = qBound(0, span.y(), text.size() - start);

    // 文字要画在"标准文本区"里：这样对齐、缩进、边距和基类完全一致，
    // 不会因为自绘就让这一列的文字比别的列偏几像素。
    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);

    QFont font = opt.font;
    painter->save();
    painter->setClipRect(textRect);

    const QFontMetrics metrics(font);
    const int baselineY = textRect.top() + (textRect.height() + metrics.ascent() - metrics.descent()) / 2;

    // 命中那一段的底色方块：宽度只按那几个字算，高度给足一点（像"记号笔划一道"）。
    if (length > 0) {
        const int matchX = textRect.left() + metrics.horizontalAdvance(text.left(start));
        const int matchWidth = metrics.horizontalAdvance(text.mid(start, length));
        const int pad = qMax(1, metrics.height() / 8);
        const QRect matchRect(matchX, textRect.top() + qMax(0, (textRect.height() - metrics.height()) / 2),
                              matchWidth, qMin(textRect.height(), metrics.height()));
        painter->fillRect(matchRect.adjusted(-pad, 0, pad, 0),
                          matchBackground(opt.palette, selected));
    }

    painter->setPen(opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));

    if (length > 0) {
        // 三段：命中前 / 命中（加粗）/ 命中后。
        // 分开画是为了让命中段能单独换字体，而 x 位置全部用 QFontMetrics 累加得出，
        // 所以"前一段有多宽"这件事不需要自己去猜。
        const int x0 = textRect.left();
        painter->setFont(font);
        painter->drawText(x0, baselineY, text.left(start));

        const int x1 = x0 + metrics.horizontalAdvance(text.left(start));
        QFont boldFont = font;
        boldFont.setBold(true);
        painter->setFont(boldFont);
        painter->setPen(matchForeground(opt.palette, selected));

        const QFontMetrics boldMetrics(boldFont);
        painter->drawText(x1, baselineY, text.mid(start, length));

        const int x2 = x1 + boldMetrics.horizontalAdvance(text.mid(start, length));
        painter->setFont(font);
        painter->setPen(opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
        painter->drawText(x2, baselineY, text.mid(start + length));
    } else {
        painter->setFont(font);
        painter->drawText(textRect.left(), baselineY, text);
    }

    painter->restore();
}
