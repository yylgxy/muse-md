#!/usr/bin/env python3
"""性能回归门禁（D2）。

对比一次新的 bench 输出和一个基线 JSON，判断算法级指标有没有显著回退。

用法：
    python bench/check_regression.py <新结果.json> <baseline.json> [--threshold 20]

判据：
    * 只对**算法级指标**设门禁 —— 见 k_ALGO_PREFIXES。这些是纯 CPU/内存里的逻辑
      （buildLineMap / LineDiff / detectEncoding），不受磁盘、网络、CI 机器快慢影响；
    * 其余指标（索引总耗时、导出落盘、gc 之类）**一律不设门禁** —— 它们对磁盘/环境太敏感，
      硬卡会把 CI 变成 flaky（路线图 D2 明确说不要对"索引总耗时"设门禁）；
    * 阈值是**相对**的（默认 20%）：CI 机器绝对速度不同，比"慢了百分之几"才有意义。

为什么不用绝对时间：同一份代码，在满载的 CI runner 上可能整体慢 30%，但算法级指标
的相对回退（比如 buildLineMap 慢了 2 倍）才是真正的回归信号。

退出码：0 = 通过（或没有可比指标）；1 = 有指标回退超阈值；2 = 用法/文件错误。
"""

import json
import sys
from pathlib import Path

# 算法级指标的前缀。命中这些前缀的样本才参与门禁比对。
# 刻意**排除**：索引（B2 首次/二次索引，磁盘敏感）、导出落盘（B5 exportHtml，磁盘敏感）、
# 造样本（B1/B3/B5 "造 … 文档"，只是准备数据不是被测逻辑）。
k_ALGO_PREFIXES = (
    "B3 SyncBridge::buildLineMap",
    "B4 detectEncoding",
    "B4 decode",
    "B1 setPlainText",
    "B1 大文档连续插入",
    "B5 standaloneHtml",
)


def load_samples(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    return {s["name"]: s for s in data.get("samples", [])}


def is_algo_metric(name: str) -> bool:
    return any(name.startswith(prefix) for prefix in k_ALGO_PREFIXES)


def main() -> int:
    args = sys.argv[1:]
    threshold = 20.0

    positional = []
    i = 0
    while i < len(args):
        if args[i] == "--threshold" and i + 1 < len(args):
            threshold = float(args[i + 1])
            i += 2
            continue
        positional.append(args[i])
        i += 1

    if len(positional) != 2:
        print("用法：check_regression.py <新结果.json> <baseline.json> [--threshold 20]",
              file=sys.stderr)
        return 2

    new_path, base_path = positional
    if not Path(new_path).exists() or not Path(base_path).exists():
        print(f"文件不存在：{new_path} 或 {base_path}", file=sys.stderr)
        return 2

    new = load_samples(new_path)
    base = load_samples(base_path)

    regressions = []
    for name, sample in new.items():
        if not is_algo_metric(name):
            continue
        if name not in base:
            # 基线里没有的算法指标：不判回退（可能是新加的），但打印一句让人知道
            print(f"[skip] 基线里没有这个指标：{name}")
            continue

        base_median = base[name]["median"]
        new_median = sample["median"]

        # 基线接近 0（比如 1e-04 的编码检测）时，相对比没有意义 —— 跳过
        if base_median < 0.01:
            continue

        ratio = new_median / base_median
        pct = (ratio - 1.0) * 100.0
        if pct > threshold:
            regressions.append((name, base_median, new_median, pct))
            print(f"[FAIL] {name}: 基线 {base_median:.2f} ms → 现在 {new_median:.2f} ms（+{pct:.1f}%）")
        else:
            print(f"[ok]   {name}: 基线 {base_median:.2f} ms → 现在 {new_median:.2f} ms（{pct:+.1f}%）")

    if regressions:
        print(f"\n{len(regressions)} 个算法级指标回退超过 {threshold:.0f}%，门禁未通过。")
        return 1

    print(f"\n所有算法级指标回退都在 {threshold:.0f}% 以内，门禁通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
