#include "syncbridge.h"

#include "logger.h"

#include <QRegularExpression>
#include <QStringList>

namespace markdown_editor::core::document {
namespace {

QString stripCr(const QString &line)
{
    return line.endsWith(QLatin1Char('\r')) ? line.left(line.size() - 1) : line;
}

bool isBlank(const QString &line)
{
    return line.trimmed().isEmpty();
}

// 各种"块开头"的识别式。都是简化版：目标是"和 md4c 渲染出的顶层元素数量对得上"，
// 不追求 CommonMark 的全部边界（对不上也不会整体错位，见头文件里的容错说明）。
const QRegularExpression &fenceRe()
{
    static const QRegularExpression re(QStringLiteral("^ {0,3}(`{3,}|~{3,})"));
    return re;
}
const QRegularExpression &atxHeadingRe()
{
    static const QRegularExpression re(QStringLiteral("^ {0,3}#{1,6}(\\s|$)"));
    return re;
}
const QRegularExpression &thematicBreakRe()
{
    static const QRegularExpression re(
        QStringLiteral("^ {0,3}((\\*\\s*){3,}|(-\\s*){3,}|(_\\s*){3,})$"));
    return re;
}
const QRegularExpression &blockQuoteRe()
{
    static const QRegularExpression re(QStringLiteral("^ {0,3}>"));
    return re;
}
const QRegularExpression &bulletRe()
{
    static const QRegularExpression re(QStringLiteral("^ {0,3}[-*+]\\s"));
    return re;
}
const QRegularExpression &orderedRe()
{
    static const QRegularExpression re(QStringLiteral("^ {0,3}\\d{1,9}[.)]\\s"));
    return re;
}
const QRegularExpression &indentedCodeRe()
{
    static const QRegularExpression re(QStringLiteral("^( {4,}|\\t)\\S"));
    return re;
}
const QRegularExpression &setextUnderlineRe()
{
    static const QRegularExpression re(QStringLiteral("^ {0,3}(=+|-+)\\s*$"));
    return re;
}
const QRegularExpression &tableDelimiterRe()
{
    static const QRegularExpression re(
        QStringLiteral("^ {0,3}\\|?\\s*:?-{1,}:?\\s*(\\|\\s*:?-{1,}:?\\s*)*\\|?\\s*$"));
    return re;
}

bool isIndentedContinuation(const QString &line)
{
    return line.startsWith(QLatin1String("  ")) || line.startsWith(QLatin1Char('\t'));
}

// 会不会"打断"当前块（成为新的顶层块）
bool startsNewTopLevelBlock(const QString &line)
{
    return atxHeadingRe().match(line).hasMatch()
        || fenceRe().match(line).hasMatch()
        || thematicBreakRe().match(line).hasMatch()
        || bulletRe().match(line).hasMatch()
        || orderedRe().match(line).hasMatch()
        || blockQuoteRe().match(line).hasMatch();
}

}  // namespace

SyncBridge::SyncBridge(QObject *parent)
    : QObject(parent)
{
}

void SyncBridge::reportEditorScroll(int line)
{
    emit editorScrolled(line);
}

void SyncBridge::reportPreviewClick(int line)
{
    emit previewClicked(line);
}

QList<int> SyncBridge::buildLineMap(const QString &markdown)
{
    QList<int> lines;

    const QStringList src = markdown.split(QLatin1Char('\n'));
    const int lineCount = src.size();

    int i = 0;
    while (i < lineCount) {
        const QString line = stripCr(src.at(i));

        if (isBlank(line)) {
            ++i;
            continue;
        }

        // ---------- 围栏代码块：``` 或 ~~~，一直吃到闭合成或文件结束 ----------
        const QRegularExpressionMatch fenceMatch = fenceRe().match(line);
        if (fenceMatch.hasMatch()) {
            lines.append(i + 1);
            const QString fence = fenceMatch.captured(1);
            const int fenceLength = fence.size();
            const QRegularExpression closeRe(
                QStringLiteral("^ {0,3}%1{%2,}\\s*$")
                    .arg(QRegularExpression::escape(QString(fence.at(0))))
                    .arg(fenceLength));
            ++i;
            while (i < lineCount) {
                const QString current = stripCr(src.at(i));
                ++i;
                if (closeRe.match(current).hasMatch()) {
                    break;
                }
            }
            continue;
        }

        // ---------- ATX 标题：一行一个块 ----------
        if (atxHeadingRe().match(line).hasMatch()) {
            lines.append(i + 1);
            ++i;
            continue;
        }

        // ---------- 分隔线：一行一个块 ----------
        if (thematicBreakRe().match(line).hasMatch()) {
            lines.append(i + 1);
            ++i;
            continue;
        }

        // ---------- 引用：多行算一个块（含"懒续行"）----------
        if (blockQuoteRe().match(line).hasMatch()) {
            lines.append(i + 1);
            ++i;
            while (i < lineCount) {
                const QString current = stripCr(src.at(i));
                if (isBlank(current)) {
                    // 空行之后还带 > 才算同一个引用
                    if (i + 1 < lineCount && blockQuoteRe().match(stripCr(src.at(i + 1))).hasMatch()) {
                        ++i;
                        continue;
                    }
                    break;
                }
                if (blockQuoteRe().match(current).hasMatch()) {
                    ++i;
                    continue;
                }
                if (startsNewTopLevelBlock(current)) {
                    break;
                }
                ++i;  // 懒续行：不带 > 但仍是引用内容
            }
            continue;
        }

        // ---------- 列表：**整个列表算一个块**（对应一个 <ul>/<ol>）----------
        if (bulletRe().match(line).hasMatch() || orderedRe().match(line).hasMatch()) {
            lines.append(i + 1);
            const bool ordered = orderedRe().match(line).hasMatch();
            ++i;
            while (i < lineCount) {
                const QString current = stripCr(src.at(i));

                if (isBlank(current)) {
                    // 跳掉连续空行，看后面还是不是这个列表
                    int j = i + 1;
                    while (j < lineCount && isBlank(stripCr(src.at(j)))) {
                        ++j;
                    }
                    if (j >= lineCount) {
                        break;
                    }
                    const QString next = stripCr(src.at(j));
                    const bool sameTypeItem =
                        ordered ? orderedRe().match(next).hasMatch() : bulletRe().match(next).hasMatch();
                    if (sameTypeItem || isIndentedContinuation(next)) {
                        i = j;
                        continue;
                    }
                    break;
                }

                const bool sameTypeItem =
                    ordered ? orderedRe().match(current).hasMatch() : bulletRe().match(current).hasMatch();
                if (sameTypeItem || isIndentedContinuation(current)) {
                    ++i;
                    continue;
                }

                // 标记类型换了（- 变成 1.）→ md4c 会渲染成两个列表，这里也要断开
                const bool otherTypeItem =
                    ordered ? bulletRe().match(current).hasMatch() : orderedRe().match(current).hasMatch();
                if (otherTypeItem || startsNewTopLevelBlock(current)) {
                    break;
                }

                ++i;  // 列表项的懒续行
            }
            continue;
        }

        // ---------- 缩进代码块（4 空格 / Tab）----------
        if (indentedCodeRe().match(line).hasMatch()) {
            lines.append(i + 1);
            ++i;
            while (i < lineCount) {
                const QString current = stripCr(src.at(i));
                if (isBlank(current)) {
                    if (i + 1 < lineCount && indentedCodeRe().match(stripCr(src.at(i + 1))).hasMatch()) {
                        ++i;
                        continue;
                    }
                    break;
                }
                if (!indentedCodeRe().match(current).hasMatch()) {
                    break;
                }
                ++i;
            }
            continue;
        }

        // ---------- GFM 表格：表头 + 分隔行 + 若干数据行算一个块 ----------
        if (line.contains(QLatin1Char('|')) && i + 1 < lineCount
            && tableDelimiterRe().match(stripCr(src.at(i + 1))).hasMatch()) {
            lines.append(i + 1);
            i += 2;  // 表头 + 分隔行
            while (i < lineCount) {
                const QString current = stripCr(src.at(i));
                if (isBlank(current) || !current.contains(QLatin1Char('|'))) {
                    break;
                }
                ++i;
            }
            continue;
        }

        // ---------- setext 标题：文字 + === / --- ----------
        if (i + 1 < lineCount && setextUnderlineRe().match(stripCr(src.at(i + 1))).hasMatch()) {
            lines.append(i + 1);
            i += 2;
            continue;
        }

        // ---------- 剩下的都是段落：吃到空行 / 下一个块开头 ----------
        lines.append(i + 1);
        ++i;
        while (i < lineCount) {
            const QString current = stripCr(src.at(i));
            if (isBlank(current)) {
                break;
            }
            // 注意顺序：先判断"下面那行是 setext 下划线"（那这一段其实是标题，一起吃掉），
            // 再判断普通块开头 —— 否则 `文字\n---` 会被当成"段落 + 分隔线"两个块。
            if (i + 1 < lineCount && setextUnderlineRe().match(stripCr(src.at(i + 1))).hasMatch()) {
                i += 2;
                break;
            }
            if (startsNewTopLevelBlock(current)) {
                break;
            }
            ++i;
        }
    }

    return lines;
}

}  // namespace markdown_editor::core::document
