#ifndef SEARCHRESULTDELEGATE_H
#define SEARCHRESULTDELEGATE_H

#include <QColor>
#include <QStyledItemDelegate>

// 搜索结果列表里"命中词高亮"的绘制（C1）。
//
// ---- 为什么必须有这个类，而不是一句 setForeground ----
// 路线图原来的写法是"给命中的那一段加前景/背景色（QTreeWidgetItem::setForeground）"，
// 但 QTreeWidgetItem 的 setForeground/setBackground 作用范围是**整个单元格**，
// 它没有办法只给"内容"这一列里的某几个字换色。用它的结果是整列文字都被染色，
// 用户反而看不出命中的是哪几个字 —— 那就等于没做。
//
// 想只染一段，只有两条路：
//   ① 自己画（本类）：把文字的分段位置算出来，逐段 drawText；
//   ② 塞一个 QLabel 当 itemWidget，靠富文本染色。
// 选 ① 的理由：② 在 500 条结果（kMaxResults）时会造 500 个 QLabel 控件，
// 而且选中态的行底色/文字色会和 QLabel 打架（QLabel 自己那层文字不会跟着选中变色）。
// ① 只在绘制事件里干活，不额外造控件，选中态也天然正确。
//
// ---- 分工：算位置 vs 画颜色 ----
// "命中词在显示文本里的哪个位置"是纯算术，放在 SearchPanel::displaySpanFor() 里，
// 有单独的单元测试（那才是最容易写错的部分：截断平移、trim 平移）。
// 本类只做"已知区间、把颜色画上去"，是薄薄一层：
// 它负责的是**绘制**，不是**规则**。这样"高亮位置对不对"能在无 GUI 的测试里验，
// 不需要为了测一个偏移去截图比像素。
class SearchResultDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    // 命中区间存在条目数据里的角色：value 是 QPoint(start, length)。
    // 放在 item 上而不是 delegate 内部，是因为 delegate 是列表共享的，
    // 而每一行的命中位置各不相同（**状态跟着数据走，工具跟着视图走**）。
    // 用 Qt::UserRole 之后的角色，避免和 SearchPanel 存在第 0 列的"结果下标"撞车。
    static constexpr int kMatchSpanRole = Qt::UserRole + 1;

    explicit SearchResultDelegate(QObject *parent = nullptr);

    // 命中的那一段用什么方式强调：加一层半透明底色 + 加粗。
    // 为什么两层：只换前景色的话，在"整行被选中"（底色是强调色）时命中词会糊进去看不见；
    // 只在选中态用反白底色。两个分支都保留可见性，是这里唯一的"视觉规则"。
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

    // 命中区间对应的底色/前景色（从视图调色板推导，所以亮暗主题都跟着变）。
    // 抽成 public static 是为了让它能被单独断言（"亮暗两套下都算得出有效颜色"）。
    static QColor matchBackground(const QPalette &palette, bool selected);
    static QColor matchForeground(const QPalette &palette, bool selected);
};

#endif // SEARCHRESULTDELEGATE_H
