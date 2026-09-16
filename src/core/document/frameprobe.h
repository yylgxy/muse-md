#ifndef FRAMEPROBE_H
#define FRAMEPROBE_H

#include <QElapsedTimer>

namespace markdown_editor::core::document {

// 帧间隔统计探针（性能优化：定位"到底哪一侧掉帧"）。
//
// 为什么需要它：用户反馈"浏览时有些卡顿、掉帧"，但**掉帧发生在哪一侧是没法靠猜的** ——
// 可能是 Qt 编辑器在重绘（重绘太慢 → 帧间隔拉长），也可能是预览那侧的 Chromium
// 在软件合成（网页重绘慢 → 网页帧间隔拉长）。这两个原因的修法完全不同。
//
// 所以这里只做一件小事：在"每一帧"发生时记一笔时间戳，然后在滚动停手之后
// 给出一句话的数字：多少帧、平均间隔、最长间隔。日志里一看就知道该压哪边。
//
// 纯计时逻辑、不依赖界面 → 可以单独测（主窗口/工作台本身在测试里造不出来）。
class FrameProbe
{
public:
    struct Summary
    {
        int frames = 0;       // 采样到的帧数（相邻两帧之间才算一个间隔，所以间隔数 = 帧数 - 1）
        double averageMs = 0; // 平均帧间隔
        double worstMs = 0;   // 最长帧间隔（"卡了一下"就是它）
        double fps = 0;       // 平均帧率（由 averageMs 换算，方便直接判断）
    };

    // 记一帧（每次视口要重绘时调用一次；第一帧只是计时起点）
    void recordFrame();

    // 停止统计（滚动停手时调用）：内部只是标记"这一轮结束"，数据留着给 summary()
    void finish();

    bool hasSamples() const;
    int frameCount() const;

    Summary summary() const;

    // 开始新一轮统计（清空旧数据）
    void reset();

    // 纯函数：由"帧数 + 总时长 + 最长间隔"算出摘要。抽出来是为了能直接测换算规则。
    static Summary buildSummary(int frames, double totalMs, double worstMs);

private:
    QElapsedTimer m_clock;
    bool m_started = false;
    int m_frames = 0;
    double m_worstMs = 0;
    double m_lastMs = 0;
};

}  // namespace markdown_editor::core::document

#endif // FRAMEPROBE_H
