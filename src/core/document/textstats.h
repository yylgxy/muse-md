#ifndef TEXTSTATS_H
#define TEXTSTATS_H

#include <QString>

namespace markdown_editor::core::document {

// 写作统计（C5）：一次算出字数 / 词数 / 段数 / 句数 / 阅读时长。★ 零 Widgets 依赖。
//
// ---- 口径必须写清楚，否则这组数字没法解释 ----
//   * characters：**字符**数（含空白、含标点、含中英文字符），就是"这篇多长"。
//     注意和"字数"混用会出问题：中英混排时"字数"没有公认定义，所以这里只报"字符"。
//   * words：**词**数。中英混排的定义是：
//       - 一段连续汉字（含中文标点以外的汉字字符）算 1 个词 —— 中文没有空格分词，
//         按"连续汉字串"计数是可解释、可复现的口径（不是"字数"，也不是分词器的结果）；
//       - 英文 / 数字按空白与标点切成词。
//     近似（写出来免得被当成 bug）：撇号、点号也是标点，所以 "don't" 算 2 个词、
//     "3.14" 算 2 个词。要做到"撇号不切、小数点不切"就得引一套词法规则，
//     对一个写作统计来说那笔钱不值得 —— 但口径必须说清楚。
//   * paragraphs：**段**数 = 被空行分隔的非空块（连续非空行算一段）。和焦点模式里的
//     "当前段落"是同一个定义 —— 同一件事只有一套口径。
//   * sentences：**句**数 = 以 。！？!?… 结尾的句段数（连续终止符算一句，避免"！！！"
//     被算成三句）。近似：英文里 "test.txt" 的"." 也会被算成句末，小写继续句会被多算 ——
//     接受这个近似（要精确就得做句法分析，那不是写作统计的活）。
//   * readingMinutes：阅读时长 = 中文按 300 字/分钟、英文按 200 词/分钟，各算各的再相加
//     （中英混排下比"总字符数 ÷ 一个常数"更贴近实际），至少 1 分钟（有内容时）。
//
// ---- 为什么不放在状态栏实时算 ----
//   这是每次按键都会变的东西，而它要遍历全文。所以策略是**按需算**：状态栏继续用它
//   已有的 O(1) 字符数；用户点「工具 → 写作统计」时才跑一次 compute()。
//   这样统计功能对打字延迟的影响是**零**，代价是数字要手点一下才刷新 —— 这个取舍
//   换来的是不必再加第三个定时器。
struct TextStats
{
    int characters = 0;
    int words = 0;
    int paragraphs = 0;
    int sentences = 0;
    int readingMinutes = 0;

    static TextStats compute(const QString &text);

    // 一行摘要，给状态栏/提示用："1234 字符 · 56 词 · 12 段 · 3 分钟"
    static QString format(const TextStats &stats);

    // 详情（对话框里显示），逐项带口径说明 —— 数字旁边没有口径就等于没说清。
    static QString detail(const TextStats &stats);
};

}  // namespace markdown_editor::core::document

#endif  // TEXTSTATS_H
