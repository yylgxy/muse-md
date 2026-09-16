#include "frameprobe.h"

namespace markdown_editor::core::document {

FrameProbe::Summary FrameProbe::buildSummary(int frames, double totalMs, double worstMs)
{
    Summary summary;
    summary.frames = frames;
    // 帧间隔数 = 帧数 - 1（相邻两帧之间才有一个间隔）。
    // 一开始写成"除以帧数"，测试立刻抓出来了：三帧两间隔时平均会偏小。
    const int gaps = frames - 1;
    if (gaps > 0 && totalMs > 0.0) {
        summary.averageMs = totalMs / gaps;
        summary.fps = summary.averageMs > 0.0 ? 1000.0 / summary.averageMs : 0.0;
    }
    summary.worstMs = worstMs;
    return summary;
}

void FrameProbe::recordFrame()
{
    if (!m_clock.isValid()) {
        m_clock.start();  // 第一帧：计时从这里开始
    }

    const double now = static_cast<double>(m_clock.nsecsElapsed()) / 1000000.0;
    if (m_frames > 0) {
        const double gap = now - m_lastMs;
        if (gap > m_worstMs) {
            m_worstMs = gap;
        }
    }
    m_lastMs = now;
    ++m_frames;
    m_started = true;
}

void FrameProbe::finish()
{
    m_started = false;
}

bool FrameProbe::hasSamples() const
{
    return m_frames > 0;
}

int FrameProbe::frameCount() const
{
    return m_frames;
}

FrameProbe::Summary FrameProbe::summary() const
{
    // 总时长 = 从第一帧到最后一帧（第一帧本身不产生间隔，所以按帧数算平均）
    return buildSummary(m_frames, m_lastMs, m_worstMs);
}

void FrameProbe::reset()
{
    m_clock.invalidate();
    m_started = false;
    m_frames = 0;
    m_worstMs = 0;
    m_lastMs = 0;
}

}  // namespace markdown_editor::core::document
