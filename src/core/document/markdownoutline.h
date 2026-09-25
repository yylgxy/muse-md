#ifndef MARKDOWNOUTLINE_H
#define MARKDOWNOUTLINE_H

#include <QList>
#include <QString>

namespace markdown_editor::core::document {

// 文档大纲：从 Markdown **源码**里抽出标题列表。★ 零 Widgets 依赖，能单独测。
//
// ---- 为什么是"扫源码行"，而不是让 MarkdownParser 顺手把标题给我 ----
//   ① parser 的公开接口只有 parseToHtml()，返回一个 HTML 字符串；要从字符串里反解析出
//      "标题 + 行号"等于绕远路（还得处理实体、转义、嵌套标签）。
//   ② 就算去接 md4c 的 MD_BLOCK_H 回调也白搭：**回调不提供行号**
//      （MD_PARSER 的 detail 里只有语义信息，没有位置）。想要"第几行"就只能自己扫源码。
//      这和 SyncBridge::buildLineMap() 遇到的是**同一个约束** —— 两处独立证据指向它，
//      这是全项目最值钱的一句总结。
//
// ---- 已知边界（写出来是为了不让它变成"bug"）----
//   * 只认 ATX 标题（`# 标题`）。下划线式（Setext，`标题\n====`）不识别。
//   * 4 空格缩进的**无围栏**代码块不排除：要正确判它得把 md4c 的块级语义引进来，
//     代价不值得（围栏代码块是主流写法，已排除）。
//   * 围栏内的 `#` 会被排除，而且判据与 MarkdownHighlighter **共用同一个函数**
//     （isFenceLine），不是各写一份 —— 同一条规则抄两份，迟早改一处忘一处。
//   * 围栏的**关闭不要求和开启同符号**（`~~~` 能关掉 ``` 开的块）。这是高亮器原本就有的
//     语义，这里刻意保持一致：两份判据要在同一份文档上给出同一个结论。混用符号的写法
//     很罕见，而"两边打架"（代码被当代码画、却被大纲认成标题）是每次都会出问题的。
struct OutlineItem
{
    int level = 1;     // 1–6（就是 # 的个数）
    QString title;     // 已剥掉结尾的闭合 #，已 trimmed，**保证非空**
    int line = 1;      // **从 1 起算**（和 goToLine / 状态栏同一套约定）
};

class MarkdownOutline
{
public:
    // 单遍扫描。O(行数)，**不用正则**：10 万行的文档上逐行 startsWith 比全文
    // globalMatch 快一个量级，而且不会被回溯咬（正则对长行的回溯是安全事故级别的坑）。
    static QList<OutlineItem> extract(const QString &text);

    // 层级 → 缩进像素。抽成纯函数是为了"层级视觉"能被单独测，
    // 同时也是**唯一一份**缩进定义：面板的 QTreeWidget::setIndentation() 就取它的值，
    // 不在界面代码里另写一个常数（否则改一处忘一处）。
    static int indentForLevel(int level);

    // 这一行是不是围栏行（``` 或 ~~~，允许行首缩进）。
    // ★ 这是**全局唯一**的围栏判据：MarkdownHighlighter 也调用它（原先那里写的是
    //   正则 ^\s*(```|~~~)）。改成手写扫描有两个好处：
    //     ① 规则只有一份，高亮和大纲不可能漂移；
    //     ② extract() 每行都要判一次，省掉正则匹配的固定开销。
    //   等价语义：行首的 ' ' 或 '\t' 之后紧跟 ``` 或 ~~~。
    static bool isFenceLine(const QString &line);

    // 面板里一行显示成什么样。只负责**截断**：层级缩进由 QTreeWidget 自己做
    // （真父子树），标题里带的尾部空白已经剥掉。超长标题从尾部截，末尾补一个"…"。
    static QString displayTextFor(const OutlineItem &item, int maxChars = 120);

private:
    // 一层缩进的像素宽度。14 是 Qt 默认缩进（也是各家大纲面板的习惯值）：
    // 六层叠起来 70px，在 210px 宽的面板里放得下。
    static constexpr int kIndentStepPx = 14;
};

}  // namespace markdown_editor::core::document

#endif  // MARKDOWNOUTLINE_H
