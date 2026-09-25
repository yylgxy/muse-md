#ifndef DIFFVIEW_H
#define DIFFVIEW_H

#include <QList>
#include <QStringList>
#include <QWidget>

#include "linediff.h"  // 接口里出现 LineDiff::Result，需要完整定义

class QListWidget;

// 版本对比视图（#9）：把 LineDiff 的结构化结果渲染成"行级红绿着色"的只读视图。
//
// 为什么是独立的控件而不是塞进 MainWindow：
//   1. 历史面板要的是"按行显示 + 点一行跳到编辑器那一行"，这是界面层的事，
//      而 LineDiff 已经给出了结构（每个 Hunk 的 kind + oldStart/newStart + 行数），
//      这里只是把结构画出来 —— 算法层不用再动。
//   2. MainWindow 没法在测试里实例化（构造要拉起 Chromium），而这个视图
//      不依赖任何 Chromium / WebChannel，可以脱离主窗口单独测"着色对不对"。
//
// 实现取舍：用 QListWidget（每行一个 item，天然支持滚动），行号放进每行文本的前缀，
// 而不是单独的 delegate —— 差异视图是只读的，前缀对齐（等宽字体 + 固定列宽）已经足够，
// 引入自定义 delegate 反而要为"对齐/选中/复制"重写一堆 Qt 内部行为。
//
// 颜色约定（和 GitHub 的 diff 一致，也符合本项目红=增/绿=删…… 注意这里语义相反）：
//   * 新增行（Insert）：浅绿底 + 深绿字（<span 风格> #d4fcd4 / #006d00）
//   * 删除行（Delete）：浅红底 + 深红字（#ffd7d5 / #b30000）
//   * 上下文行（Equal）：无底色
// 行号用"旧行号 / 新行号"两列并排（Delete 行只有旧行号，Insert 行只有新行号），
// 1 起算，与全项目一致。
class DiffView : public QWidget
{
    Q_OBJECT

public:
    explicit DiffView(QWidget *parent = nullptr);

    // 把一份差异结果 + 两侧的原文渲染出来。oldLines / newLines 是 splitLines() 之后的
    // 行数组（和 LineDiff::compute 用的同一份），行号 1 起算。
    void setDiff(const markdown_editor::core::document::LineDiff::Result &result,
                 const QStringList &oldLines,
                 const QStringList &newLines);

    // 当前渲染出来的行数（测试用：断言"N 个 hunk 展开成 M 行"）。
    int rowCount() const;

signals:
    // 双击某一"新"行 → 要求编辑器跳到新文档的这一行（1 起算）。
    // Delete/上下文行也可能有对应的新行号；没有对应新行的（纯删除）发 -1。
    void lineActivated(int newLine);

private:
    // 渲染一行。style 决定底色，oldNo/newNo 是两侧行号（-1 = 这一侧没有）。
    void addRow(const QString &text, int style, int oldNo, int newNo);

    // 双击处理：把 item 里存的新行号发出去
    void onItemDoubleClicked();

    QListWidget *m_list = nullptr;
};

#endif // DIFFVIEW_H
