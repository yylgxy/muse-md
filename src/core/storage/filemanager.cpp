#include "filemanager.h"

#include "fileutils.h"
#include "logger.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QStringConverter>

namespace markdown_editor::core::storage {

namespace {

// 耗时数字：缓存命中往往不到 0.1ms，用整数毫秒会全都显示成 0
QString elapsedMs(const QElapsedTimer &timer)
{
    return QString::number(double(timer.nsecsElapsed()) / 1e6, 'f', 3);
}

}  // namespace

FileManager::FileManager(QObject *parent) : QObject(parent) {}

// ============================ 四个操作 ============================

bool FileManager::openFile(const QString &path, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    // 计时是给用户看的证据：日志里"已打开(命中缓存) … 耗时 0.02 ms"对比
    // "已打开 … 耗时 1.8 ms"，就是 4.2.3 那条验收（第二次更快）最直接的证明。
    QElapsedTimer timer;
    timer.start();

    const QFileInfo info(path);

    // ---- 第一步：查缓存 ----
    // 先要求文件真实存在：文件都不在了，缓存里的记录也不该采信（那是删掉的文件）。
    if (info.exists() && !info.isDir()) {
        CacheManager::Entry cached;
        switch (m_cache.lookup(path, info, &cached)) {
        case CacheManager::LookupResult::Hit:
            // 命中：不读盘、不解码，直接用内存里的内容
            applyOpenedContent(path, info, cached.text, cached.encoding);
            LOG_INFO("已打开(命中缓存): %1（编码 %2，%3 字符，耗时 %4 ms）",
                     path,
                     encodingName(cached.encoding),
                     cached.text.size(),
                     elapsedMs(timer));
            return true;
        case CacheManager::LookupResult::Stale:
            // 有记录但文件在磁盘上变了（别的程序改过）：必须重新读盘，否则会拿旧内容覆盖新改动
            LOG_INFO("缓存已过期（文件被别处改过），重新读盘: %1", path);
            break;
        case CacheManager::LookupResult::Miss:
            break;
        }
    }

    // ---- 第二步：读盘。失败就整件事作废：下面的状态一个都不动，
    //      这样"打开失败"不会把用户当前正在编辑的文档弄丢 ----
    QByteArray raw;
    if (!FileUtils::readFileBytes(path, raw, error)) {
        LOG_ERROR("打开失败: %1（%2）", path, error != nullptr ? *error : QString());
        return false;
    }

    // 判编码 → 解码成文本。这两步是纯函数，不碰磁盘，所以能单独测。
    const Encoding detected = detectEncoding(raw);
    const QString content = decode(raw, detected);

    // ---- 二进制文件：明确拒绝，而不是把几 MB 乱码灌进编辑器和预览 ----
    // 场景很真实：在文件树里双击到 .pdb / .ilk / .docx 就走到这里了。那种内容既没法编辑，
    // 又会被推给预览的渲染进程（那是把预览拖死、甚至让预览永久白屏的最主要来源）。
    // 拒绝时"对象状态完全不变"，和别的打开失败一样。
    if (looksBinary(content)) {
        if (error != nullptr) {
            *error = QStringLiteral("这看起来是二进制文件（不是文本），Markdown 编辑器打不开它：\n%1\n\n"
                                    "（%2 字节。想编辑它请用专门的工具。）")
                         .arg(path)
                         .arg(raw.size());
        }
        LOG_WARN("拒绝打开：看起来是二进制文件 %1（%2 字节，按 %3 解出来仍不像文本）",
                 path,
                 raw.size(),
                 encodingName(detected));
        return false;
    }

    applyOpenedContent(path, info, content, detected);
    LOG_INFO("已打开: %1（编码 %2，%3 字节，耗时 %4 ms）",
             path,
             encodingName(detected),
             raw.size(),
             elapsedMs(timer));

    // 放进缓存：下次打开同一个文件就不用读盘了。
    // 超过单条上限的大文件会被 CacheManager 自己拒掉（记一笔 rejections），不会撑爆内存。
    rememberInCache(path, content, detected, raw.size());
    return true;
}

// 把"打开成功"这件事一次性落地：状态 + 信号。
// 读盘命中和缓存命中两条路径共用它，是为了保证两者的行为完全一致 ——
// 否则很容易写出"走缓存时忘了清脏标志"这类只在特定路径复现的 bug。
void FileManager::applyOpenedContent(const QString &path,
                                     const QFileInfo &info,
                                     const QString &content,
                                     Encoding encoding)
{
    m_encoding = encoding;
    // 复用上面已经查好的 info：打开一个文件本来就要 stat 一次（判缓存是否过期），
    // 判只读再查第二遍是白花钱 —— 这台机器上一次 stat 大约几百微秒，比读缓存本身还贵。
    m_readOnly = isReadOnlyFile(info);

    m_document.setMarkdownText(content);
    m_document.setFilePath(path);
    m_document.setModified(false);

    emit modificationChanged(false);
    emit fileOpened(path);

    if (m_readOnly) {
        // 只读不是失败：文件照样能看能改，只是保存会失败。把"原因"交给 UI 去提示。
        const QString reason = QStringLiteral("这个文件是只读的，内容可以看也可以改，但「保存」会失败。\n"
                                              "想保留修改请用「另存为」存到别的位置：\n%1")
                                   .arg(path);
        LOG_WARN("文件是只读的: %1", path);
        emit readOnlyDetected(path, reason);
    }
}

void FileManager::rememberInCache(const QString &path, const QString &content, Encoding encoding, qint64 fileSize)
{
    CacheManager::Entry entry;
    entry.text = content;
    entry.encoding = encoding;
    entry.size = fileSize;
    // 记下"这份内容对应的磁盘状态"：下次查找时用它判断文件有没有被改过
    entry.lastModified = QFileInfo(path).lastModified();

    if (!m_cache.insert(path, entry)) {
        LOG_INFO("未进缓存（超过单条上限 %1 字节，或缓存已关闭）: %2", m_cache.maxEntryBytes(), path);
    }
}

bool FileManager::looksBinary(const QString &text)
{
    if (text.isEmpty()) {
        return false;  // 空文件当然不是二进制（新建出来的空文件也要能打开）
    }

    // 只看开头一段就够了：二进制文件的开头从来不会像文本；
    // 顺带让这个判断的开销与文件大小无关（几 MB 的文件也是常数时间）。
    const int sample = qMin(text.size(), 64 * 1024);
    int suspicious = 0;

    for (int i = 0; i < sample; ++i) {
        const ushort code = text.at(i).unicode();
        if (code == 0) {
            return true;  // NUL：解码后的文本里不该出现（UTF-16 在解码阶段已经配成字符了）
        }
        // 控制字符里只有 tab / 换行 / 回车 / 换页是文本里正常的东西
        if (code < 0x20 && code != '\t' && code != '\n' && code != '\r' && code != '\f') {
            ++suspicious;
        }
    }

    // 要求"数量够多"且"比例够高"：少数控制字符（老文件用 ^Z 结尾之类）不该被判成二进制
    return suspicious >= 16 && suspicious * 10 > sample;
}

bool FileManager::saveFile(QString *error)
{
    if (m_document.getFilePath().isEmpty()) {
        // 新文档还没有路径。这不是"出错"，而是一个必须由用户决定的分支
        // （保存到哪儿），所以只给出提示，让 UI 去弹「另存为」。
        if (error != nullptr) {
            *error = QStringLiteral("这个文档还没保存过，请用「另存为」选一个位置");
        }
        return false;
    }

    return writeTo(m_document.getFilePath(), error);
}

bool FileManager::saveFileAs(const QString &path, QString *error)
{
    if (path.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("保存路径是空的");
        }
        return false;
    }

    return writeTo(path, error);
}

void FileManager::newFile()
{
    const bool wasModified = m_document.isModified();

    m_document.setMarkdownText(QString());
    m_document.setFilePath(QString());
    m_document.setModified(false);
    m_encoding = Encoding::Utf8;  // 新文档默认 UTF-8（不带 BOM）
    m_readOnly = false;

    if (wasModified) {
        emit modificationChanged(false);
    }

    LOG_INFO("新建文档");
}

bool FileManager::writeTo(const QString &path, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    // 只读 / 是目录 / 目录不可写 —— 提前拦下来，给出人话原因。
    // 不拦的话 QSaveFile 也会失败，但错误信息往往是"另一个程序正在使用此文件"这种误导人的话。
    const QString problem = writabilityProblem(path);
    if (!problem.isEmpty()) {
        if (error != nullptr) {
            *error = problem;
        }
        LOG_ERROR("保存失败: %1（%2）", path, problem);
        return false;
    }

    // 按文档自己的编码写回：GBK 的老文档保存后还是 GBK，用记事本打开不会变乱码。
    // 编码表示不了内容时会自动改成 UTF-8（actualEncoding 会告诉我们）。
    Encoding used = m_encoding;
    const QByteArray bytes = encode(m_document.getMarkdownText(), m_encoding, &used);
    if (used != m_encoding) {
        LOG_WARN("当前编码（%1）表示不了文档里的字符，本次已改用 %2 保存",
                 encodingName(m_encoding),
                 encodingName(used));
        m_encoding = used;
    }

    if (!FileUtils::writeFileBytes(path, bytes, error)) {
        // 写失败：脏标志保持 true（内容没落盘，不能假装保存成功），原文件也没被动过
        LOG_ERROR("保存失败: %1（%2）", path, error != nullptr ? *error : QString());
        return false;
    }

    m_document.setFilePath(path);  // 「另存为」语义：写完就认这个新路径
    m_document.setModified(false);
    m_readOnly = isReadOnlyFile(path);

    emit modificationChanged(false);
    emit fileSaved(path);

    LOG_INFO("已保存: %1（编码 %2，%3 字节）", path, encodingName(m_encoding), bytes.size());

    // 缓存里那份内容已经旧了。直接换成刚写进去的（而不是删掉）——
    // "保存之后紧接着又打开同一个文件"是很常见的操作，这样它也是缓存命中。
    rememberInCache(path, m_document.getMarkdownText(), m_encoding, bytes.size());

    // 保存成功之后再打快照。顺序有讲究：先把 fileSaved 发出去（界面立刻更新成"已保存"），
    // 再做可能要多花几十毫秒的 git 操作，用户不会觉得保存卡。
    snapshotAfterSave();
    return true;
}

// ============================ 版本控制（4.2.2）============================

void FileManager::snapshotAfterSave()
{
    if (!m_autoSnapshot || m_document.getFilePath().isEmpty()) {
        return;
    }

    if (!VersionControl::isGitAvailable()) {
        // 没装 git 不是"保存失败"，只提示一次就够（用 WARN 让它在日志里显眼）
        LOG_WARN("系统里没有找到 git，这次保存没有留下快照（装好 Git 并加入 PATH 后会自动开始记录）");
        return;
    }

    const QString repoDir = m_history.repositoryPathFor(m_document.getFilePath());

    QString error;
    if (!m_history.isRepository(repoDir) && !m_history.initRepository(repoDir, &error)) {
        LOG_WARN("建快照仓库失败（不影响保存）: %1", error);
        return;
    }

    // 备注带时间戳（4.2.2 的要求），这类备注在 git log 里一眼能看出是自动快照
    const QString hash =
        m_history.commitSnapshot(repoDir, m_document.getMarkdownText(), VersionControl::defaultSnapshotMessage(), &error);

    if (hash.isEmpty()) {
        if (error.isEmpty()) {
            // 内容与上一版完全一样（比如打开后没改就按了 Ctrl+S）
            LOG_INFO("快照: 内容与上一版相同，没有新建提交");
        } else {
            LOG_WARN("打快照失败（不影响保存）: %1", error);
        }
        return;
    }

    LOG_INFO("快照成功: %1 已记录到 %2", hash.left(7), repoDir);
}

VersionControl *FileManager::versionControl()
{
    return &m_history;
}

const VersionControl *FileManager::versionControl() const
{
    return &m_history;
}

bool FileManager::restoreSnapshot(const QString &rev, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    if (m_document.getFilePath().isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("这个文档还没保存过，没有历史版本可以回滚");
        }
        return false;
    }

    const QString repoDir = m_history.repositoryPathFor(m_document.getFilePath());

    // 先把内容取到手，再动状态：取失败时当前编辑内容一个字符都不该丢
    //（"内容为空但没报错" = 那一版就是空文档，是正常结果）。
    QString historyError;
    const QString content = m_history.contentOf(repoDir, rev, &historyError);
    if (!historyError.isEmpty()) {
        if (error != nullptr) {
            *error = historyError;
        }
        LOG_ERROR("回滚失败: %1（%2）", rev.left(7), historyError);
        return false;
    }

    if (content == m_document.getMarkdownText()) {
        LOG_INFO("回滚: %1 的内容与当前完全相同，什么都没变", rev.left(7));
        return true;
    }

    // 刻意只改内存、不写磁盘：用户看到内容、确认没问题之后再按 Ctrl+S。
    // 这样"回滚"永远是可撤销的（不满意就不保存）。
    m_document.setMarkdownText(content);
    m_document.setModified(true);
    emit modificationChanged(true);

    LOG_INFO("已回滚到 %1（内容已载入编辑器，尚未写盘，等用户保存）", rev.left(7));
    return true;
}

void FileManager::setAutoSnapshotEnabled(bool enabled)
{
    m_autoSnapshot = enabled;
}

bool FileManager::autoSnapshotEnabled() const
{
    return m_autoSnapshot;
}

// ============================ 缓存（4.2.3）============================

CacheManager *FileManager::cacheManager()
{
    return &m_cache;
}

const CacheManager *FileManager::cacheManager() const
{
    return &m_cache;
}

// ============================ 内容与状态 ============================

QString FileManager::text() const
{
    return m_document.getMarkdownText();
}

bool FileManager::setText(const QString &text)
{
    if (text == m_document.getMarkdownText()) {
        return false;  // 内容没变：不动脏标志（"输入又删掉"不该算已修改）
    }

    const bool wasModified = m_document.isModified();

    m_document.setMarkdownText(text);  // 内部同样只在内容变化时置脏

    if (!wasModified && m_document.isModified()) {
        emit modificationChanged(true);
    }
    return true;
}

QString FileManager::filePath() const
{
    return m_document.getFilePath();
}

QString FileManager::fileName() const
{
    const QString path = m_document.getFilePath();
    return path.isEmpty() ? QStringLiteral("未命名") : QFileInfo(path).fileName();
}

bool FileManager::hasFilePath() const
{
    return !m_document.getFilePath().isEmpty();
}

bool FileManager::isModified() const
{
    return m_document.isModified();
}

void FileManager::setModified(bool modified)
{
    const bool wasModified = m_document.isModified();
    m_document.setModified(modified);
    if (wasModified != modified) {
        emit modificationChanged(modified);
    }
}

bool FileManager::isReadOnly() const
{
    return m_readOnly;
}

FileManager::Encoding FileManager::encoding() const
{
    return m_encoding;
}

QDateTime FileManager::createdTime() const
{
    // 路径为空时 QFileInfo("").birthTime() 就是无效时间，调用方用 isValid() 判断即可
    return QFileInfo(m_document.getFilePath()).birthTime();
}

QDateTime FileManager::modifiedTime() const
{
    return QFileInfo(m_document.getFilePath()).lastModified();
}

QString FileManager::encodingName(Encoding encoding)
{
    switch (encoding) {
    case Encoding::Utf8Bom:
        return QStringLiteral("UTF-8 (带 BOM)");
    case Encoding::Utf16LE:
        return QStringLiteral("UTF-16 LE");
    case Encoding::Utf16BE:
        return QStringLiteral("UTF-16 BE");
    case Encoding::Local8Bit:
        return QStringLiteral("本地编码 (GBK/CP936)");
    case Encoding::Utf8:
    default:
        return QStringLiteral("UTF-8");
    }
}

// ============================ 编码检测与转换 ============================

FileManager::Encoding FileManager::detectEncoding(const QByteArray &raw)
{
    // 1) BOM 是唯一"板上钉钉"的证据，先看它
    if (raw.startsWith("\xEF\xBB\xBF")) {
        return Encoding::Utf8Bom;
    }
    if (raw.startsWith("\xFF\xFE")) {
        return Encoding::Utf16LE;
    }
    if (raw.startsWith("\xFE\xFF")) {
        return Encoding::Utf16BE;
    }

    if (raw.isEmpty()) {
        return Encoding::Utf8;  // 空文件没线索，选默认
    }

    // 2) 没有 BOM 的 UTF-16：ASCII 字符在 UTF-16 里必然带一个 0x00 字节，
    //    所以"超过四分之一的字节是 NUL"基本就能断定是 UTF-16（正常文本里不会出现 NUL）。
    int nulls = 0;
    int nullsOnOdd = 0;
    for (int i = 0; i < raw.size(); ++i) {
        if (raw.at(i) == '\0') {
            ++nulls;
            if (i % 2 != 0) {
                ++nullsOnOdd;
            }
        }
    }
    if (nulls > 0 && nulls * 4 > raw.size()) {
        // 低位字节在前（"a" 存成 61 00，NUL 落在奇数下标）→ 小端
        return (nullsOnOdd * 2 >= nulls) ? Encoding::Utf16LE : Encoding::Utf16BE;
    }

    // 3) 严格按 UTF-8 走一遍：整篇都是合法 UTF-8（纯 ASCII 也算）就认 UTF-8
    if (isValidUtf8(raw)) {
        return Encoding::Utf8;
    }

    // 4) 都不是 → 只能按本机编码处理（中文 Windows 上就是 GBK）。
    //    这是绝大多数老中文文档的归宿。
    return Encoding::Local8Bit;
}

// 严格的 UTF-8 校验（RFC 3629）。逐字节走一遍，任何一处不合法就返回 false。
//
// 关键点（这三条正好是"看起来像 UTF-8 其实不是"的常见陷阱）：
//   * 续字节必须落在 0x80..0xBF
//   * 不许过长编码（例如用 2 字节表示 ASCII，C0 AF）
//   * 不许出现 UTF-16 代理区（ED A0 80 ~ ED BF BF）和 U+10FFFF 以上的码点（F4 90 起）
bool FileManager::isValidUtf8(const QByteArray &bytes)
{
    const char *data = bytes.constData();
    const int size = bytes.size();
    int i = 0;

    while (i < size) {
        const unsigned char lead = static_cast<unsigned char>(data[i]);

        int extra = 0;               // 这个字符后面还有几个续字节
        unsigned char firstLow = 0x80;   // 第一个续字节的下限（用来挡掉过长编码）
        unsigned char firstHigh = 0xBF;  // 第一个续字节的上限（用来挡掉代理区）

        if (lead < 0x80) {
            ++i;
            continue;  // ASCII，最常见的情况
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            extra = 1;  // 注意从 C2 起：C0/C1 只可能构成过长编码
        } else if (lead == 0xE0) {
            extra = 2;
            firstLow = 0xA0;  // E0 80..9F 是过长编码
        } else if (lead >= 0xE1 && lead <= 0xEC) {
            extra = 2;
        } else if (lead == 0xED) {
            extra = 2;
            firstHigh = 0x9F;  // ED A0..BF 是 UTF-16 代理区，UTF-8 里非法
        } else if (lead >= 0xEE && lead <= 0xEF) {
            extra = 2;
        } else if (lead == 0xF0) {
            extra = 3;
            firstLow = 0x90;  // F0 80..8F 是过长编码
        } else if (lead >= 0xF1 && lead <= 0xF3) {
            extra = 3;
        } else if (lead == 0xF4) {
            extra = 3;
            firstHigh = 0x8F;  // F4 90.. 超出 U+10FFFF
        } else {
            return false;  // 0x80..0xC1 和 0xF5..0xFF 都不可能是首字节
        }

        if (i + extra >= size) {
            return false;  // 序列被截断了
        }

        const unsigned char first = static_cast<unsigned char>(data[i + 1]);
        if (first < firstLow || first > firstHigh) {
            return false;
        }
        for (int k = 2; k <= extra; ++k) {
            const unsigned char cont = static_cast<unsigned char>(data[i + k]);
            if (cont < 0x80 || cont > 0xBF) {
                return false;
            }
        }

        i += extra + 1;
    }

    return true;
}

QString FileManager::decode(const QByteArray &raw, Encoding encoding)
{
    switch (encoding) {
    case Encoding::Utf8Bom:
        // BOM 只用来标记编码，不属于内容，解码时去掉
        return QString::fromUtf8(raw.startsWith("\xEF\xBB\xBF") ? raw.mid(3) : raw);

    case Encoding::Utf16LE:
    case Encoding::Utf16BE: {
        QByteArray body = raw;
        if (body.startsWith("\xFF\xFE") || body.startsWith("\xFE\xFF")) {
            body = body.mid(2);
        }
        QStringDecoder decoder(encoding == Encoding::Utf16LE ? QStringConverter::Utf16LE
                                                             : QStringConverter::Utf16BE);
        return decoder(body);
    }

    case Encoding::Local8Bit: {
        // QStringConverter::System = 本机 ANSI 代码页（中文 Windows = GBK/CP936）
        QStringDecoder decoder(QStringConverter::System);
        return decoder(raw);
    }

    case Encoding::Utf8:
    default:
        return QString::fromUtf8(raw);
    }
}

QByteArray FileManager::encode(const QString &text, Encoding encoding, Encoding *actualEncoding)
{
    if (actualEncoding != nullptr) {
        *actualEncoding = encoding;
    }

    // UTF-8 能表示任何字符串，不需要任何检查
    if (encoding == Encoding::Utf8) {
        return text.toUtf8();
    }
    if (encoding == Encoding::Utf8Bom) {
        QByteArray out = text.toUtf8();
        out.prepend("\xEF\xBB\xBF");
        return out;
    }

    const bool isUtf16 = (encoding == Encoding::Utf16LE || encoding == Encoding::Utf16BE);
    const QStringConverter::Encoding qtEncoding =
        (encoding == Encoding::Utf16LE)
            ? QStringConverter::Utf16LE
            : ((encoding == Encoding::Utf16BE) ? QStringConverter::Utf16BE : QStringConverter::System);

    QStringEncoder encoder(qtEncoding);
    QByteArray out = encoder(text);

    if (isUtf16) {
        // 打开时带 BOM 的，写回去也带 —— 否则记事本会认不出是 UTF-16
        out.prepend(encoding == Encoding::Utf16LE ? "\xFF\xFE" : "\xFE\xFF");
    }

    // 关键一步：用**同一种编码**再解回来，和原文逐字符比。
    // GBK 存不了 emoji、UTF-16 遇到表示不了的字符时，转换会变成 '?' 之类的替身，
    // 解回来就和原文不一样 —— 那说明会丢字，于是改用 UTF-8：
    // 宁可换编码（文件仍然可读），也不能把用户的字悄悄弄没。
    if (decode(out, encoding) != text) {
        if (actualEncoding != nullptr) {
            *actualEncoding = Encoding::Utf8;
        }
        return text.toUtf8();
    }

    return out;
}

// ============================ 可写性 ============================

bool FileManager::isReadOnlyFile(const QString &path)
{
    return isReadOnlyFile(QFileInfo(path));
}

bool FileManager::isReadOnlyFile(const QFileInfo &info)
{
    // 不存在的文件不算只读（"新建 / 另存为"要写的就是不存在的文件）
    return info.exists() && !info.isWritable();
}

QString FileManager::writabilityProblem(const QString &path)
{
    if (path.isEmpty()) {
        return QStringLiteral("路径是空的");
    }

    const QFileInfo info(path);

    if (info.exists()) {
        if (info.isDir()) {
            return QStringLiteral("%1 是文件夹，不是文件").arg(path);
        }
        if (!info.isWritable()) {
            // Windows 上带"只读"属性的文件，改名覆盖会失败（即使当前用户是管理员），
            // 所以这里提前拦下来，直接告诉用户怎么办。
            return QStringLiteral("文件是只读的：%1\n（去掉文件的「只读」属性，或用「另存为」存到别的位置）")
                .arg(path);
        }
    }

    // QSaveFile 是"在目标目录里先写临时文件、成功后再改名替换"，
    // 所以目录本身也必须可写 —— 否则哪怕目标文件不存在也写不进去。
    const QString dir = info.absolutePath();
    const QFileInfo dirInfo(dir);
    if (dirInfo.exists() && !dirInfo.isWritable()) {
        return QStringLiteral("目录没有写权限：%1").arg(dir);
    }

    return QString();  // 可以写
}

}  // namespace markdown_editor::core::storage
