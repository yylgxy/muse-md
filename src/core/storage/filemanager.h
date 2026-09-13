#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QString>

#include "cachemanager.h"      // 值成员，需要完整类型（它又带来了 textencoding.h 里的 Encoding）
#include "markdowndocument.h"  // 值成员，需要完整类型
#include "versioncontrol.h"    // 值成员，需要完整类型

namespace markdown_editor::core::storage {

// 文件管理器：把"一个 Markdown 文档"和"磁盘上的那个文件"之间的事全包在这里。
//
// 它负责什么：
//   * 四个操作：打开 / 保存 / 另存为 / 新建
//   * 编码检测：读文件时判断它是 UTF-8、UTF-8 带 BOM、UTF-16 还是本机编码（中文 Windows = GBK），
//     并按**原来的编码**写回去 —— 用记事本存过的 GBK 文档，在别的程序里打开不会突然变乱码
//   * 只读文件：打开时检测只读属性，发信号让 UI 提示；保存时提前拦下来并给出可读的原因
//   * 修改标志：内容真的变了才置脏（"输入又删掉"不算），保存/打开后清掉，变化时发信号
//   * 保存成功后自动打一个**轻量快照**（4.2.2，本地历史）—— 见 snapshotAfterSave()
//
// 它**不**负责什么（这些是别的层的事）：
//   * 不弹任何对话框。只读提示、保存失败提示都是 UI 的事 —— 本类只把"原因"通过信号/出参给出。
//     这样它在没有窗口的环境里也能跑（测试、将来的命令行模式都靠这条）。
//   * 不管编辑器控件：编辑器的内容通过 setText() 灌进来，之后由本类保管。
//
// 与 MarkdownDocument 的分工（4.2.1 之后收窄过，边界很清楚）：
//   MarkdownDocument = **纯内存状态**：内容 + 路径 + 脏标志，一行磁盘代码都没有；
//   本类            = 一切"文件层面"的事：读 / 写、编码、只读、文件时间戳、四个操作、对外信号。
//   所以不要在别处再存一份路径或脏标志 —— 那会变成两个事实来源，迟早不一致。
//   渲染也不在这里：Markdown → HTML 由 PreviewRenderer 调 MarkdownParser 完成（那条管线带防抖）。
//
// 分层注记：本类在 core/storage（持久化层），依赖 core/document（要驱动 MarkdownDocument）和
// infrastructure（FileUtils）。core/storage 的 CMake 里 core_document 是 PUBLIC 链接的
// —— markdowndocument.h 出现在本头文件的 include 里，属于接口的一部分。
//
// 编码检测的**已知局限**（启发式的固有代价，不是 bug）：GBK 文件如果恰好整篇都是合法 UTF-8
// 字节序列（例如某些汉字组合），会被判成 UTF-8 而解码错。纯 ASCII 文件不受影响（两种编码都是
// 同一串字节）。真要百分之百可靠，只能靠 BOM 或让用户手动指定编码。
class FileManager : public QObject
{
    Q_OBJECT

public:
    // 文本编码。枚举本体住在 textencoding.h（4.2.3 挪出去的，理由见那个头文件：
    // cachemanager.h 也要用它，不能反过来 include 本文件，否则就循环了）。
    // 这里保留别名，所以外面原来的写法 FileManager::Encoding::Utf8 完全不受影响。
    using Encoding = markdown_editor::core::storage::Encoding;

    explicit FileManager(QObject *parent = nullptr);

    // ============================ 四个操作 ============================

    // 打开文件。成功：内容进文档、路径记住、编码记住、脏标志清掉，发 fileOpened(path)。
    //                文件是只读的 → 仍然算成功（能看能改），但会额外发 readOnlyDetected()。
    // 失败：返回 false，error 非 nullptr 时写入可直接展示的原因；
    //       **对象状态完全不变**（原来打开着的文档、路径、脏标志、编码统统保留）。
    bool openFile(const QString &path, QString *error = nullptr);

    // 保存到当前路径（Ctrl+S）。成功后发 fileSaved(path) 并清掉脏标志。
    // 还没有路径的新文档：返回 false，error 提示应该走「另存为」——这不是错误，是个需要用户决定的分支。
    // 失败（只读 / 无权限 / 被占用 / 磁盘满）：返回 false，**脏标志保持 true**（内容还没落盘，
    // 不能假装保存成功），原文件也不受影响（QSaveFile 保证）。
    bool saveFile(QString *error = nullptr);

    // 另存到指定路径（成功后内部路径切过去，即"另存为"语义），其余同 saveFile()。
    bool saveFileAs(const QString &path, QString *error = nullptr);

    // 新建：清空内容、丢掉路径、脏标志清掉、编码回到 UTF-8 默认。
    // 不发信号 —— 调用方自己知道它调了这个（要清编辑器、要换预览的 baseUrl）；
    // 只是"脏标志从 true 变 false"这件事会照常通过 modificationChanged 发出去。
    void newFile();

    // ============================ 内容与状态 ============================

    QString text() const;  // 当前 Markdown 文本

    // 从编辑器回灌内容（用户敲字走这里）。
    // 内容与当前**完全一致**时返回 false 且什么都不做 —— 免得"输入又删掉"被判成已修改。
    // 内容变了：置脏（首次变脏时发 modificationChanged(true)）并返回 true。
    bool setText(const QString &text);

    QString filePath() const;   // 磁盘路径；空 = 还没保存过的新文档
    QString fileName() const;   // 只要文件名（用于标题栏）；没有路径时返回"未命名"
    bool hasFilePath() const;
    bool isModified() const;    // 有未保存的修改
    void setModified(bool modified);
    bool isReadOnly() const;    // 当前文件是否只读（打开/保存时检测出来的结果）
    Encoding encoding() const;  // 当前文档的编码（决定保存时怎么写）

    static QString encodingName(Encoding encoding);  // 给人看的名字，进日志和提示语

    // 文件的创建 / 最后修改时间（每次都查一次磁盘）。
    // 路径为空或文件不存在时返回**无效**的 QDateTime —— 用 isValid() 判断后再显示。
    // 放在这里而不是 MarkdownDocument 里：这属于"文件层面"的信息（要碰磁盘），
    // 文档模型那边保持纯内存。
    QDateTime createdTime() const;
    QDateTime modifiedTime() const;

    // ============================ 编码（纯函数）============================
    // 三个都是 static 且不碰文件系统，所以能脱离磁盘单独测。

    // 判断一段字节是什么编码。规则见 .cpp 里的注释（BOM → NUL 比例 → 严格 UTF-8 校验 → 本机编码）。
    static Encoding detectEncoding(const QByteArray &raw);

    // 按指定编码把字节解码成文本（BOM 会被跳过，不会变成文本开头的怪字符）。
    static QString decode(const QByteArray &raw, Encoding encoding);

    // 按指定编码把文本编码成字节。
    // 特殊规则：如果这个编码**表示不了**文本里的某些字符（例如 GBK 存不了 emoji），
    // 会自动改用 UTF-8 并**保证内容不变**（宁可换编码，不丢字）。
    // actualEncoding 非 nullptr 时写入实际用的编码，调用方据此更新自己的状态。
    static QByteArray encode(const QString &text, Encoding encoding, Encoding *actualEncoding = nullptr);

    // ============================ 可写性（只读提示的依据）============================

    // 文件存在且带只读属性（或没有写权限）→ true。文件不存在 → false（新文件不算只读）。
    static bool isReadOnlyFile(const QString &path);

    // 同上，但复用调用方已经查好的 QFileInfo：打开文件时本来就要查一次文件状态
    //（判断缓存是否过期、看文件在不在），再查第二遍纯属浪费 —— 每次 stat 在这台机器上要几百微秒。
    static bool isReadOnlyFile(const QFileInfo &info);

    // 能不能往这个路径写？返回空字符串 = 可以；否则返回一句可以直接展示给用户的原因。
    // 检查三件事：路径是不是目录、文件是不是只读、所在目录有没有写权限
    //（QSaveFile 是在目标目录里先写临时文件再改名，所以目录也必须可写）。
    // 目录本身还不存在时不报错 —— FileUtils 会自动创建父目录，真失败再由它给出原因。
    static QString writabilityProblem(const QString &path);

    // ============================ 版本控制（4.2.2）============================

    // 本文件管理器内嵌的版本控制服务：查历史、看差异都通过它。
    // 它的快照仓库位置、是否启用自动快照也都由这里统一管，UI 不用自己拼路径。
    VersionControl *versionControl();
    const VersionControl *versionControl() const;

    // 保存成功后要不要自动打快照（默认 **开启**）。
    // 关掉它的场景：用户明确不想留历史、或者不希望在磁盘上多出东西。
    void setAutoSnapshotEnabled(bool enabled);
    bool autoSnapshotEnabled() const;

    // 回滚：把某个历史版本的内容取回来放进文档。
    // **不写磁盘** —— 内容只是载入内存并把文档标成"已修改"，由用户看过之后决定要不要 Ctrl+S。
    // 这么设计是有意的：直接覆盖文件会让"回滚"变成不可撤销的破坏性操作，
    // 而能撤销它的东西恰恰就是版本历史本身。
    // 成功：true，文档内容已换成那一版（modificationChanged 会在真的变了时发出来）。
    // 失败：false + error，**文档状态一个字节都不动**（版本号不存在、还没保存过、没有历史……）。
    bool restoreSnapshot(const QString &rev, QString *error = nullptr);

    // ============================ 缓存（4.2.3）============================

    // 最近打开过的文件内容缓存（LRU）。打开文件时会先查它：
    //   命中且没过期 → 不读盘，直接用内存里的内容；
    //   过期（文件被别的程序改过）→ 重新读盘并覆盖缓存。
    // 返回可写的指针是为了让 UI 能调 setMaxEntries()/statisticsText() 这类配置和统计。
    CacheManager *cacheManager();
    const CacheManager *cacheManager() const;

signals:
    // 文件已成功打开（内容已经进文档、脏标志已清）
    void fileOpened(const QString &path);

    // 文件已成功写入磁盘（内容已经真的落盘，脏标志已清）
    void fileSaved(const QString &path);

    // 修改标志变化（true = 有未保存的修改）。UI 用它更新标题栏的 * 和「保存」的可用状态。
    void modificationChanged(bool modified);

    // 打开的文件是只读的。reason 是一句可直接展示的提示语（UI 决定弹不弹、怎么弹）。
    void readOnlyDetected(const QString &path, const QString &reason);

private:
    // saveFile / saveFileAs 共用的落地动作：可写性检查 → 按当前编码编码 → 原子写 → 更新状态 → 发信号
    bool writeTo(const QString &path, QString *error);

    // 把"打开成功"这件事一次性落到状态里（编码、只读、文档内容、脏标志）。
    // 读盘命中和缓存命中两条路径共用它，保证两条路径的状态变化一模一样。
    // info 是调用方已经查好的文件状态（顺便省掉一次 stat）。
    void applyOpenedContent(const QString &path, const QFileInfo &info, const QString &content, Encoding encoding);

    // 把刚读到的内容放进缓存（会按文件当前的修改时间/大小记下"新鲜度"）。
    void rememberInCache(const QString &path, const QString &content, Encoding encoding, qint64 fileSize);

    // 保存成功之后自动打快照（4.2.2）。
    // 约定：**任何失败都不影响"保存成功"这个结论** —— 没装 git、仓库建不起来、
    // 提交失败，全都只写一条日志。用户按 Ctrl+S 的目的是保存文件，不是维护历史。
    void snapshotAfterSave();

    // 严格的 UTF-8 校验（RFC 3629）。detectEncoding 靠它区分"UTF-8"和"本机编码"。
    // 为什么不用 Qt 的解码器：QStringDecoder 遇到坏字节只塞一个 U+FFFD 替代字符，
    // 默认标志下 hasError() **不会**变 true —— 实测把 GBK 字节喂给它，hasError() 仍然是 false，
    // 用它当判据会把 GBK 文档误判成 UTF-8（然后整篇乱码）。
    static bool isValidUtf8(const QByteArray &bytes);

    markdown_editor::core::document::MarkdownDocument m_document;  // 内容 + 路径 + 脏标志 + HTML 缓存

    VersionControl m_history;      // 轻量快照（4.2.2），仓库位置由它自己决定
    CacheManager m_cache;          // 最近打开文件的内容缓存（4.2.3）
    bool m_autoSnapshot = true;    // 保存后自动打快照
    Encoding m_encoding = Encoding::Utf8;  // 当前文档的编码（打开时检测，保存时按它写回）
    bool m_readOnly = false;               // 当前文件是否只读
};

}  // namespace markdown_editor::core::storage

#endif // FILEMANAGER_H
