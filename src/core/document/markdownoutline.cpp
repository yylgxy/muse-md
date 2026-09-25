#include "markdownoutline.h"

#include <QChar>

namespace markdown_editor::core::document {

namespace {

// 行首的空白（高亮器的老正则 ^\s* 只吃 ASCII 空白；这里保持同样的集合，
// 不引入 U+00A0 之类，免得"顺手改好了"变成一个没人要求过的行为变化）。
bool isAsciiBlank(QChar ch)
{
    return ch == QLatin1Char(' ') || ch == QLatin1Char('\t');
}

}  // namespace

bool MarkdownOutline::isFenceLine(const QString &line)
{
    int i = 0;
    while (i < line.size() && isAsciiBlank(line.at(i))) {
        ++i;
    }

    if (line.size() - i < 3) {
        return false;  // 剩下不到 3 个字符，不可能是 ``` / ~~~
    }

    const QChar first = line.at(i);
    if (first != QLatin1Char('`') && first != QLatin1Char('~')) {
        return false;
    }
    // 只要连续三个就行（```lang、``` 都算；四个及以上也照样是围栏）
    return line.at(i + 1) == first && line.at(i + 2) == first;
}

QList<OutlineItem> MarkdownOutline::extract(const QString &text)
{
    QList<OutlineItem> items;
    if (text.isEmpty()) {
        return items;
    }

    // 按 \n 切；\r 交给每行的 trimmed() 处理（不在这里预处理整篇，省一次全文拷贝）。
    // Qt::KeepEmptyParts：空行要保留 —— 否则行号会整体错位，而大纲的全部价值就是行号。
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);

    bool inFence = false;
    for (int index = 0; index < lines.size(); ++index) {
        const QString &raw = lines.at(index);

        // ---- 围栏状态机：和 MarkdownHighlighter 是同一份判据 ----
        if (isFenceLine(raw)) {
            inFence = !inFence;
            continue;  // 围栏行自己不是标题（哪怕它写成 ```# 什么）
        }
        if (inFence) {
            continue;  // 代码块里的 # 一律不算标题
        }

        // ---- 行首最多 3 个空格（4 个就成缩进代码块了）----
        int i = 0;
        while (i < raw.size() && raw.at(i) == QLatin1Char(' ')) {
            ++i;
        }
        if (i > 3) {
            continue;
        }

        // ---- 连续 1–6 个 # ----
        const int hashStart = i;
        while (i < raw.size() && raw.at(i) == QLatin1Char('#')) {
            ++i;
        }
        const int level = i - hashStart;
        if (level == 0 || level > 6) {
            continue;  // "#标题" 没有 # → level 0；"####### 七级" → level 7
        }

        // ---- # 之后必须有空白，否则不是标题（`#标题` 是普通文本）----
        if (i >= raw.size() || !isAsciiBlank(raw.at(i))) {
            continue;
        }

        QString title = raw.mid(i).trimmed();  // trimmed() 顺手把 \r 也去掉

        // ---- 剥结尾的闭合井号：`## 标题 ##` → `标题` ----
        // 只有当那串 # 前面是空白（即整串是独立的）时才算闭合序列：
        // `# C# 语言` 结尾的 # 前面是 C，属于标题文本，不能剥。
        int end = title.size();
        while (end > 0 && title.at(end - 1) == QLatin1Char('#')) {
            --end;
        }
        if (end < title.size() && (end == 0 || title.at(end - 1).isSpace())) {
            title = title.left(end).trimmed();
        }

        // ---- 空标题（`#` 后面只有空白）跳过 ----
        // 定的策略是"不显示"。理由：面板里会出现一个没有文字、点了却会跳行的条目，
        // 用户看到的是一个空白行 —— 与其显示一个看不懂的东西，不如不显示。
        // 有测试覆盖这条策略（面试问"空标题怎么办"时答得出"我定了策略并且测了"）。
        if (title.isEmpty()) {
            continue;
        }

        OutlineItem item;
        item.level = level;
        item.title = title;
        item.line = index + 1;  // 0 起算的 index → 1 起算的行号
        items.append(item);
    }

    return items;
}

int MarkdownOutline::indentForLevel(int level)
{
    // 夹到 1–6：越界的值不抛也不崩，按最近的合法层级算（面板是"显示"用的，
    // 不该因为一个意外的数字整块不画）。
    const int clamped = level < 1 ? 1 : (level > 6 ? 6 : level);
    return (clamped - 1) * kIndentStepPx;
}

QString MarkdownOutline::displayTextFor(const OutlineItem &item, int maxChars)
{
    if (maxChars <= 0) {
        return QString();
    }
    if (item.title.size() <= maxChars) {
        return item.title;
    }
    // 截断留一个位置给省略号，这样显示宽度不会超（"多少字符"和"看起来多长"一致）
    return item.title.left(maxChars - 1) + QStringLiteral("…");
}

}  // namespace markdown_editor::core::document
