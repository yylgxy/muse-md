#ifndef LINEDIFF_H
#define LINEDIFF_H

#include <QList>
#include <QString>
#include <QStringList>

namespace markdown_editor::core::document {

// 行级差异（B1）：纯算法。除了 QString / QStringList 之外**不依赖任何东西**
// —— 不认识 QWidget、不认识 QProcess、不认识 git。
//
// ---- 为什么自己写，而不是调 `git diff` ----
//   1. 版本历史面板要"按行显示 + 点一行跳到编辑器那一行"，需要的是**结构化结果**
//      （哪一段是删的、在旧文本的第几行、对应新文本的第几行），而 `git diff` 给的
//      是一段给人看的 unified 文本。想从文本反解出结构，等于把已经算过的东西再解析一遍。
//   2. `git diff` 的输出格式会随 git 版本 / 语言环境漂移（虽然已经固定了 LC_ALL=C），
//      而快照仓库所在的机器上有没有 git、装的是哪个版本，都不是我们能保证的
//      —— VersionControl 的注释里已经写明"没有 git 时一切操作返回失败"。
//   3. 这是全项目唯一一处**纯算法、可脱离界面测试、能讲复杂度**的地方。
//
// ---- 接口约定（和项目其它地方一致）----
//   * 行号一律 **1 起算**（和 SyncBridge / EditorWidget::goToLine / 状态栏一致）。
//   * 不在这里做"高亮 / 着色 / 省略号"——那是界面层的事。保持纯净才测得动。
//   * 两处刻意的取舍，写在这里免得被当成 bug：
//       - **`\r\n` 与 `\n` 视为同一种换行**：splitLines() 会剥掉行尾的 `\r`。
//         不剥的话，"整个文件只是被换了换行符"会被判成"全文都变了"——
//         这是 diff 实现最经典的假阳性。
//       - **最后一行有没有换行符，不体现**：`"a\nb"` 与 `"a\nb\n"` 算作相同的两份文本。
//         按"行内容"比较是这个工具的定位（看历史、比差异），不是做字节级校验。
//
// ---- 算法选型（三阶段，见各自函数的注释）----
//   阶段 1｜行哈希预去重：把每行映射成 int，之后所有比较都是整数比较。
//   阶段 2｜总行数 <= kDpLimit → 动态规划 LCS（最好读、最容易验证、结果可证最小）。
//   阶段 3｜否则            → Myers O(ND)（内存 O(N)，大文件可用）。
//   降级  ｜超过 kFallbackThreshold 或编辑距离超过 kMaxEditDistance → 整段替换。
//
// ---- 降级策略（必须说清楚，面试必问）----
//   阈值都是对 **max(旧行数, 新行数)** 说的（文档的规模），不是对"两边加起来"说的
//   —— 不然"5 万行的文档只改 3 行"会被判成 10 万行而降级，把最容易算的情况推给降级路径。
//
//   | 条件 | 策略 | 理由 |
//   |---|---|---|
//   | max(n,m) > kFallbackThreshold(50000) | 不尝试对齐，整段替换 | 保证最坏情况有上界；用户要的是"看出改了哪块"，不是完美对齐 |
//   | \|n-m\| > kLineCountDeltaThreshold(2000) | 同上 | 见下面的"等价的提前判定" |
//   | Myers 编辑距离 > kMaxEditDistance(2000) | 同上 | **最容易被忽略的一条**：Myers 的复杂度是 O((N+M)·D)，"两份文件完全不同"时 D≈N+M，会退化成平方级；同时为了能回溯必须保存每轮的 V，内存是 O(D²)。两条都必须在阈值上刹住，否则大文件既慢又吃内存 |
//   | 单行超过 kMaxHashChars(4096) | 只在**哈希阶段**按字符截断 | 避免为一行哈希几十万字符。代价：两行极长且前 4096 字符相同的行会被判为同一行 —— 只影响"压成一行的 JSON"这类病态输入，且降级方向是"少报差异"，不会崩 |
//
//   **等价的提前判定**：每做一次编辑，新旧文本的长度差最多变化 1，所以编辑距离
//   必然满足 D >= |n - m|。于是 "|n - m| > kMaxEditDistance" 和
//   "D > kMaxEditDistance" 是同一件事 —— 前者不用跑算法就知道，后者要跑满
//   2000 轮才发现。所以这一条不是额外的保守阈值，而是**同一条闸门的廉价前置判定**：
//   能提前否掉，就不去启动一次注定超预算的 Myers。
//
//   注意降级**不是失败**：Result::degraded 为 true，hunks 是"全部删 + 全部增"，
//   界面照样能显示"这两个版本差很多"，只是不再逐行对齐。
//
// 用法：
//     const LineDiff::Result r = LineDiff::compute(LineDiff::splitLines(oldText),
//                                                 LineDiff::splitLines(newText));
//     if (!r.identical()) { ... }
class LineDiff
{
public:
    enum class Kind
    {
        Equal,
        Insert,
        Delete,
    };

    // 一段连续的同类型行。行号 1 起算。
    struct Hunk
    {
        Kind kind = Kind::Equal;
        int oldStart = 0;  // 旧文本里的起始行（1 起算；Insert 时 = 插入点）
        int newStart = 0;  // 新文本里的起始行
        int oldCount = 0;
        int newCount = 0;
    };

    struct Result
    {
        // 按顺序排列的块：Equal / Delete / Insert 交替出现，相邻同类已合并。
        // 结构不变式（tests/test_linediff.cpp 会钉住它）：
        //   把每个块的 oldCount 加起来 = 旧文本行数，newCount 加起来 = 新文本行数；
        //   且各块的 oldStart / newStart 首尾相接、单调递增，无空洞、无重叠。
        //   （Insert 块的 oldCount 为 0，它记录的是"插入点"。）
        QList<Hunk> hunks;
        int insertedLines = 0;
        int deletedLines = 0;

        // 是否走了上面的降级路径。默认 false。
        bool degraded = false;

        bool identical() const { return insertedLines == 0 && deletedLines == 0; }
    };

    // 主入口：算两份文本的行级差异。内部按规模选算法（见文件头）。
    static Result compute(const QStringList &oldLines, const QStringList &newLines);

    // ---- 纯工具（都能单独测）----

    // 按 `\n` 切行，剥掉行尾的 `\r`；若末尾是空元素（即文本以换行结尾）则去掉它，
    // 这样 `"a\nb"` 与 `"a\nb\n"` 切出同样的两行。
    static QStringList splitLines(const QString &text);

    // 把结构化结果转成给人看的 unified diff 文本（`+` / `-` / 上下文行）。
    // 只用于展示与"和 git 对拍"，**不参与任何判定**。
    //
    // 注意：这段文本**刻意不保证和 `git diff` 逐字节相同**。unified 格式里
    // "两个改动点靠多近才合并成一个 @@ 段"属于实现自由度，不同工具取法不同；
    // 所以测试里和 git 对拍用的是"增删行数 + 能否还原出新文本"，
    // 而不是拿两段文本做字符串比较（那种对拍只会验出格式差异，验不出算法错）。
    static QString toUnifiedText(const Result &result,
                                 const QStringList &oldLines,
                                 const QStringList &newLines,
                                 int contextLines = 3);

    // ---- 阈值（公开是为了让测试和基准能直接引用，避免两边各写一份魔数）----
    // 前三个阈值的完整理由见文件头的降级策略表。
    static constexpr int kDpLimit = 2000;              // max(行数) 不超过它就走路 2（DP LCS）
    static constexpr int kFallbackThreshold = 50000;   // max(行数) 超过它就降级
    static constexpr int kMaxEditDistance = 2000;      // Myers 允许的最大编辑距离
    static constexpr int kLineCountDeltaThreshold = kMaxEditDistance;  // |n-m| 超过它就降级
    static constexpr int kMaxHashChars = 4096;         // 哈希阶段单行最多看这么多字符
};

}  // namespace markdown_editor::core::document

#endif // LINEDIFF_H
