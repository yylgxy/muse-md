#include "textstats.h"

#include <QChar>
#include <QList>
#include <QStringList>

namespace markdown_editor::core::document {

namespace {

// 汉字（含常见扩展区）：CJK 统一表意文字 + 扩展 A/B + 兼容表意文字。
// 用它把"连续汉字串"切成词。不含日文假名/韩文 —— 这个编辑器的目标场景是中英混排，
// 把假名算成什么词没有公认答案，算进"词"里反而不如按英文规则处理（会各成一段）。
bool isCjk(QChar ch)
{
    const ushort code = ch.unicode();
    if (code >= 0x4E00 && code <= 0x9FFF) {
        return true;  // CJK 统一表意文字（常用汉字）
    }
    if (code >= 0x3400 && code <= 0x4DBF) {
        return true;  // 扩展 A
    }
    if (code >= 0xF900 && code <= 0xFAFF) {
        return true;  // 兼容表意文字
    }
    return false;
}

// 句末标点：中英文都要认，连续出现算一句（"！！！" 是语气，不是三句话）
//
// ★ 中文标点必须写成 QChar(0x3002) 这类码点，**不能写 QLatin1Char('。')**：
//   QLatin1Char 只能装单个 Latin-1 字节，而 '。' 是 U+3002，塞进去会截成半个 UTF-8 字节，
//   结果是"编译通过、断言全对、功能全废" —— 中文句末标点一个也不认，句数恒为 0。
//   这个坑是 test_textstats 的 D1/D2/D5/G2 一起抓出来的。
bool isSentenceEnd(QChar ch)
{
    switch (ch.unicode()) {
    case 0x3002:  // 。
    case 0xFF01:  // ！
    case 0xFF1F:  // ？
    case '.':     // 半角句点
    case '!':
    case '?':
    case 0x2026:  // …
        return true;
    default:
        return false;
    }
}

bool isWordSeparator(QChar ch)
{
    return ch.isSpace() || ch.isPunct() || ch.isSymbol();
}

}  // namespace

TextStats TextStats::compute(const QString &text)
{
    TextStats stats;
    stats.characters = static_cast<int>(text.size());
    if (text.isEmpty()) {
        return stats;
    }

    // ---- 单遍扫描：词 / 段 / 句一起数 ----
    // 不按行切、不建 QStringList：这几项都能在一次遍历里得到，省掉一次全文切分
    //（和 MarkdownOutline 一样，能单遍就别多遍）。
    //
    // ★ 段的判据是"空行分隔"，**不是"一个换行一段"**：
    //   连续几行非空文字属于同一段，所以"段落结束"这件事要等到**遇到空行**才知道，
    //   而不是每见到 '\n' 就收尾。用 lineHasContent 记住"刚过去的这一行有没有内容"，
    //   遇到 '\n' 时才能判断刚结束的是不是空行。
    //   这条口径和焦点模式里的「当前段落」是同一个（那边按 block.text().trimmed() 判）。
    bool inWord = false;         // 当前是否在"一个词"里（中文连续汉字串 或 英文单词）
    bool inCjkRun = false;       // 中文词与英文词是两种词，边界处要各算一次
    bool inParagraph = false;    // 当前这一段还没被空行收尾
    bool lineHasContent = false; // 刚过去的这一行有非空白字符
    bool pendingSentence = false;  // 见过句末标点，还没遇到非标点字符

    const auto endWord = [&stats, &inWord, &inCjkRun]() {
        if (inWord) {
            ++stats.words;
            inWord = false;
            inCjkRun = false;
        }
    };

    for (const QChar ch : text) {
        // ---- 段落 ----
        if (ch == QLatin1Char('\n')) {
            // 换行：先收尾当前词；再看刚过去的这一行是不是空行
            endWord();
            if (!lineHasContent && inParagraph) {
                ++stats.paragraphs;
                inParagraph = false;
            }
            lineHasContent = false;
            continue;
        }

        // ---- 段落内容标记 ----
        // 口径和焦点模式一致：一行里只要有**一个非空白字符**就算有内容（标点也算 ——
        // "——" 单独一行是一段；"   " 这样的行不算）。
        // 放在句/词判定之前：句末标点会 continue 走人，不能让它漏掉"这一行有内容"。
        if (!ch.isSpace()) {
            inParagraph = true;
            lineHasContent = true;
        }

        // ---- 句子 ----
        if (isSentenceEnd(ch)) {
            endWord();
            if (!pendingSentence) {
                ++stats.sentences;
                pendingSentence = true;  // 连续标点只算一句
            }
            continue;
        }
        // 空白也算"遇到非标点字符"的前一种：它不清 pendingSentence，
        // 因为"句末。 下一个"里空格不该让后续文本重新开句。
        if (!ch.isSpace()) {
            pendingSentence = false;
        }

        // ---- 词 ----
        if (isCjk(ch)) {
            // 中文：连续的汉字算一个词
            if (!inWord || !inCjkRun) {
                endWord();  // 上一个是英文词（或没有词）→ 先收尾
                inWord = true;
                inCjkRun = true;
            }
            continue;
        }

        if (isWordSeparator(ch)) {
            endWord();
            continue;
        }

        // 英文 / 数字 / 其它字母：连续到下一个分隔符算一个词
        if (!inWord || inCjkRun) {
            endWord();
            inWord = true;
            inCjkRun = false;
        }
    }

    endWord();
    if (inParagraph) {
        ++stats.paragraphs;
    }

    // ---- 阅读时长：中文按字、英文按词，各算各的再相加 ----
    // 这里要单独统计"汉字数"与"英文词数"，所以再扫一遍 —— compute 是按需调用的
    //（不是每次按键），多这一遍的成本可以接受，换来的是口径清楚。
    int cjkChars = 0;
    int latinWords = 0;
    bool inLatin = false;
    for (const QChar ch : text) {
        if (isCjk(ch)) {
            ++cjkChars;
            inLatin = false;
            continue;
        }
        if (isWordSeparator(ch)) {
            inLatin = false;
            continue;
        }
        if (!inLatin) {
            ++latinWords;
            inLatin = true;
        }
    }

    const double minutes = double(cjkChars) / 300.0 + double(latinWords) / 200.0;
    // 有内容时至少 1 分钟：显示"0 分钟"会让人以为算坏了
    stats.readingMinutes = minutes <= 0.0 ? 0 : (minutes < 1.0 ? 1 : static_cast<int>(minutes + 0.5));
    return stats;
}

QString TextStats::format(const TextStats &stats)
{
    return QStringLiteral("%1 字符 · %2 词 · %3 段 · %4 分钟")
        .arg(stats.characters)
        .arg(stats.words)
        .arg(stats.paragraphs)
        .arg(stats.readingMinutes);
}

QString TextStats::detail(const TextStats &stats)
{
    return QStringLiteral("%1\n\n"
                          "· 字符：含空白与标点，就是这篇东西有多长\n"
                          "· 词：中文按「连续汉字串」算一个词（中文没有空格分词），"
                          "英文按空白与标点切；中英混排时两边各算各的\n"
                          "· 段：被空行分隔的非空块（和焦点模式的「当前段落」同一口径）\n"
                          "· 句：以 。！？.!?… 结尾；连续终止符算一句（「！！！」不是三句）\n"
                          "· 阅读时长：中文 300 字/分钟 + 英文 200 词/分钟，至少 1 分钟")
        .arg(format(stats));
}

}  // namespace markdown_editor::core::document
