#ifndef DRAFTRECOVERY_H
#define DRAFTRECOVERY_H

#include <QDateTime>
#include <QList>
#include <QString>

namespace markdown_editor::core::storage {

// 未保存内容的崩溃恢复（A3）。
//
// ---- 为什么单独一个类（本项目的惯例）----
// MainWindow 没法在测试里实例化（构造就要拉起 Chromium），而"草稿能不能正确写下来、
// 读回来、清干净"是必须自动验证的。所以这一层只依赖 FileUtils（原子写），不认识界面。
//
// ---- 语义边界（写清楚，免得它慢慢长成一个什么都管的类）----
//   * 它**不认识文档管理器**，也不认识编辑器 —— 只收"路径 + 内容 + 光标"三个值；
//   * 它**不判断"该不该存"**（那是调用方的策略：多久存一次、什么时机存）；
//   * 它**不弹任何窗**：恢复时问不问用户、怎么问，是界面层的事。
//
// ---- 三条设计决定（面试会问，这里先答）----
//   1. **不用 QSettings 存草稿**。配置文件是给人改的键值对，草稿是**文档内容** ——
//      两者混在一起，用户手改配置时可能把草稿弄坏。草稿正文走 FileUtils::writeFile()
//      （QSaveFile 原子写），manifest 也走同一个函数。
//   2. **草稿目录独立于文档目录**（放 AppData）。理由和 VersionControl 把仓库放 AppData
//      完全一样：不往用户的笔记目录里塞杂物。
//   3. **写入频率由调用方定**（15 秒定时 + 切标签/失焦/关窗口前补一次）。这一类只提供
//      "存/查/删/清"四个动作，不含任何计时器。
//
// ---- 磁盘布局 ----
//     <root>/manifest.json     索引：每条草稿对应哪个文档、什么时候写的、光标在哪
//     <root>/<sha1(路径)>.draft   草稿正文（UTF-8，原子写）
// 新建的未保存文档没有路径，用 displayName 的哈希当键（它同时被当作"身份"）。
class DraftRecovery
{
public:
    struct Draft
    {
        QString documentPath;  // 空 = 新建的未命名文档
        QString displayName;   // 给人看的名字（新建文档是"未命名 1"）
        QDateTime savedAt;
        int cursorLine = 1;    // 恢复时光标回到哪（1 起算，和全项目一致）
        int cursorColumn = 1;
        QString contentPath;   // 草稿正文文件（绝对路径）

        // 这条草稿的"身份"：有路径用路径，没路径用显示名。
        // 抽成函数是因为 store / discard / pending 三处都要用同一个规则 ——
        // 各写一遍迟早会漂移（比如只在其中一处把路径归一化成绝对路径）。
        QString key() const;
    };

    explicit DraftRecovery(const QString &rootDir = QString());  // 空 = 默认 AppData 路径

    // <AppData>/Dev/MarkdownEditor/drafts
    static QString defaultRootDir();

    QString rootDir() const;

    // 写一份草稿：内容原子落在 <root>/<hash>.draft，然后更新 manifest.json。
    // 失败只写日志、返回 false —— 草稿写不下去绝不该影响正常保存。
    //
    // 成功标准是"正文写成功 **且** manifest 更新成功"。正文成功但 manifest 失败时，
    // 会把那个孤儿正文文件删掉再返回 false（否则 prune 会把它当成"已失效文件"，
    // 逻辑就不自洽了）。
    bool store(const Draft &draft, const QString &content, QString *error = nullptr);

    // 有草稿的文档列表（按 savedAt 新→旧）。启动时用它问用户"要不要恢复"。
    // 容错：manifest 坏了 → 返回空列表（不抛异常、不崩）；
    //       正文文件被手工删了 → 这一条不返回，并顺手把 manifest 里的残项清掉。
    QList<Draft> pending() const;

    // 删掉某个文档的草稿（保存成功、或用户明确放弃恢复时调）。
    // 传的是文档路径；对未命名文档传它的显示名。
    bool discard(const QString &documentPath, QString *error = nullptr);
    bool discardAll(QString *error = nullptr);

    // 清理超过 maxAgeDays 天的草稿（manifest 里已不存在的正文文件也顺手清掉）。
    // 返回清掉的条数。
    int pruneOlderThan(int maxAgeDays);

private:
    // manifest.json 的读写。读失败返回空列表（容错优先：草稿坏了不该让程序起不来）。
    struct Entry;
    QList<Entry> readManifest(bool *ok = nullptr) const;
    bool writeManifest(const QList<Entry> &entries, QString *error) const;

    QString contentPathFor(const QString &key) const;

    QString m_rootDir;
};

}  // namespace markdown_editor::core::storage

#endif // DRAFTRECOVERY_H
