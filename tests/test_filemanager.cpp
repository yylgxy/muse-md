// FileManager（4.2.1 文件管理器）的契约测试。
//
// 覆盖四块：
//   1. 编码检测与转换（纯函数）：BOM / 纯 ASCII / UTF-8 / 无 BOM 的 UTF-16 / GBK；
//      以及"目标编码表示不了字符时自动改用 UTF-8、绝不丢字"这条规则。
//   2. 修改标志：内容真变了才置脏、"输入又删掉"不算、保存后清掉、信号发几次。
//   3. 四个操作（**真的读写磁盘**）：打开 / 保存 / 另存为 / 新建，以及失败时状态不变。
//   4. 只读文件：打开能看、能改，但保存必须失败、脏标志必须留着、磁盘内容不能变。
//
// 测试文件全部写在系统临时目录下的 md-editor-filemanager-test/，不污染源码树。
//
// 跑法：ctest -C Debug --output-on-failure

#include "filemanager.h"
#include "fileutils.h"
#include "versioncontrol.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <cstdio>

using markdown_editor::core::storage::FileManager;
using markdown_editor::core::storage::VersionControl;

namespace {

int g_fail = 0;

void check(bool ok, const QString &what, const QString &detail = QString())
{
    std::printf("%-58s %s", what.toUtf8().constData(), ok ? "PASS" : "FAIL");
    if (!detail.isEmpty()) {
        std::printf("  [%s]", detail.toUtf8().constData());
    }
    std::printf("\n");
    if (!ok) {
        ++g_fail;
    }
}

// 绕过 FileManager，直接按原始字节造样本（BOM / GBK / UTF-16 都得这么造）
void writeRaw(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(bytes);
    file.close();
}

QByteArray readRaw(const QString &path)
{
    QByteArray bytes;
    FileUtils::readFileBytes(path, bytes);
    return bytes;
}

// 这段字节是不是合法 UTF-8？—— 注意：**不能**用 QStringDecoder::hasError() 来判，
// 它对坏字节只塞一个 U+FFFD 替代字符、默认标志下 hasError() 仍是 false（这正是被修掉的 bug）。
// 所以这里也不自己实现校验，而是干脆不用它：测试里只判断"本机编码的字节和 UTF-8 是否相同"。

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QString work = QDir(QDir::tempPath()).filePath(QStringLiteral("md-editor-filemanager-test"));
    QDir(work).removeRecursively();
    if (!QDir().mkpath(work)) {
        std::printf("cannot create temp dir: %s\n", work.toUtf8().constData());
        return 2;
    }

    // ============================ 1. 编码检测（纯函数）============================
    {
        check(FileManager::detectEncoding(QByteArray("\xEF\xBB\xBFhello")) == FileManager::Encoding::Utf8Bom,
              QStringLiteral("detect: UTF-8 BOM"));
        check(FileManager::detectEncoding(QByteArray("hello")) == FileManager::Encoding::Utf8,
              QStringLiteral("detect: 纯 ASCII -> UTF-8"));
        check(FileManager::detectEncoding(QStringLiteral("中文标题").toUtf8()) == FileManager::Encoding::Utf8,
              QStringLiteral("detect: 合法 UTF-8 多字节 -> UTF-8"));
        check(FileManager::detectEncoding(QByteArray()) == FileManager::Encoding::Utf8,
              QStringLiteral("detect: 空内容 -> UTF-8（默认值）"));

        // 带 BOM 的 UTF-16：FF FE = 小端，FE FF = 大端
        check(FileManager::detectEncoding(QByteArray::fromHex("FFFE6100")) == FileManager::Encoding::Utf16LE,
              QStringLiteral("detect: UTF-16 LE BOM"));
        check(FileManager::detectEncoding(QByteArray::fromHex("FEFF0061")) == FileManager::Encoding::Utf16BE,
              QStringLiteral("detect: UTF-16 BE BOM"));

        // 没有 BOM 的 UTF-16：靠"NUL 字节占四分之一以上"这个特征认出来
        check(FileManager::detectEncoding(QByteArray::fromHex("610062006300")) == FileManager::Encoding::Utf16LE,
              QStringLiteral("detect: 无 BOM UTF-16 LE（NUL 落在奇数位）"));
        check(FileManager::detectEncoding(QByteArray::fromHex("006100620063")) == FileManager::Encoding::Utf16BE,
              QStringLiteral("detect: 无 BOM UTF-16 BE（NUL 落在偶数位）"));

        // 本机编码：用 QString::toLocal8Bit() 现场生成，不写死字节（不同机器代码页可能不同）。
        // "要不要跑这段检查"的判据是"本机编码的字节和 UTF-8 字节是否不同"，
        // 而不是"这段字节是不是合法 UTF-8" —— 后者等于用被测代码自己的逻辑来判断要不要测它。
        const QByteArray localBytes = QStringLiteral("中文标题").toLocal8Bit();
        const bool localIsNotUtf8 = (localBytes != QStringLiteral("中文标题").toUtf8());
        if (localIsNotUtf8) {
            check(FileManager::detectEncoding(localBytes) == FileManager::Encoding::Local8Bit,
                  QStringLiteral("detect: 本机编码（GBK）字节 -> Local8Bit"),
                  QStringLiteral("%1 字节 %2").arg(localBytes.size()).arg(QString::fromLatin1(localBytes.toHex())));
            check(FileManager::decode(localBytes, FileManager::Encoding::Local8Bit) == QStringLiteral("中文标题"),
                  QStringLiteral("decode: Local8Bit 解出正确中文"));
        } else {
            std::printf("%-58s SKIP  [本机 ANSI 代码页是 UTF-8，GBK 检查无意义]\n", "detect: 本机编码（GBK）");
        }

        // 严格性：这四种"看起来像 UTF-8 其实不是"的字节必须落到本机编码，而不是被当成 UTF-8
        check(FileManager::detectEncoding(QByteArray::fromHex("C0AF")) == FileManager::Encoding::Local8Bit,
              QStringLiteral("detect: 过长编码 C0 AF 不算 UTF-8"));
        check(FileManager::detectEncoding(QByteArray::fromHex("EDA080")) == FileManager::Encoding::Local8Bit,
              QStringLiteral("detect: 代理区 ED A0 80 不算 UTF-8"));
        check(FileManager::detectEncoding(QByteArray::fromHex("F4908080")) == FileManager::Encoding::Local8Bit,
              QStringLiteral("detect: 超出 U+10FFFF 的 F4 90 80 80 不算 UTF-8"));
        check(FileManager::detectEncoding(QByteArray::fromHex("E4B8")) == FileManager::Encoding::Local8Bit,
              QStringLiteral("detect: 被截断的序列 E4 B8 不算 UTF-8"));
        check(FileManager::detectEncoding(QStringLiteral("中文🎉").toUtf8()) == FileManager::Encoding::Utf8,
              QStringLiteral("detect: 四字节 UTF-8（emoji）算 UTF-8"));

        // 已知局限（写出来是为了让它可见，不是 bug）：如果 GBK 字节恰好构成合法 UTF-8 序列，
        // 就会被判成 UTF-8。下面这两个字节正好是合法 UTF-8（U+05E2），所以归到 UTF-8。
        check(FileManager::detectEncoding(QByteArray::fromHex("D7A2")) == FileManager::Encoding::Utf8,
              QStringLiteral("detect: 字节恰好合法 UTF-8 时归 UTF-8（已知局限）"));
    }

    // ============================ 1b. 编解码往返 ============================
    {
        check(FileManager::decode(QByteArray("\xEF\xBB\xBFhello"), FileManager::Encoding::Utf8Bom)
                  == QStringLiteral("hello"),
              QStringLiteral("decode: BOM 不进文本内容"));
        check(FileManager::decode(QStringLiteral("标题 **粗体**").toUtf8(), FileManager::Encoding::Utf8)
                  == QStringLiteral("标题 **粗体**"),
              QStringLiteral("decode: UTF-8 往返"));

        const QByteArray utf16 = FileManager::encode(QStringLiteral("你好"), FileManager::Encoding::Utf16LE);
        check(utf16.startsWith("\xFF\xFE"), QStringLiteral("encode: UTF-16 LE 写出 BOM"));
        check(FileManager::decode(utf16, FileManager::Encoding::Utf16LE) == QStringLiteral("你好"),
              QStringLiteral("decode: UTF-16 LE 往返"));

        const QByteArray bom = FileManager::encode(QStringLiteral("hi"), FileManager::Encoding::Utf8Bom);
        check(bom.startsWith("\xEF\xBB\xBF") && bom.mid(3) == QByteArray("hi"),
              QStringLiteral("encode: UTF-8 BOM 写出 BOM + 内容"));

        // ★ 关键规则：目标编码表示不了某些字符时，改用 UTF-8，绝不丢字
        FileManager::Encoding used = FileManager::Encoding::Local8Bit;
        const QByteArray fallback = FileManager::encode(QStringLiteral("中文🎉"), FileManager::Encoding::Local8Bit, &used);
        check(used == FileManager::Encoding::Utf8,
              QStringLiteral("encode: GBK 存不了 emoji -> 自动改用 UTF-8"), FileManager::encodingName(used));
        check(FileManager::decode(fallback, used) == QStringLiteral("中文🎉"),
              QStringLiteral("encode: 回退后内容一字不差（没有变成 ? ）"));

        // 能表示的情况不应该乱换编码
        FileManager::Encoding used2 = FileManager::Encoding::Utf8;
        const QByteArray gbk = FileManager::encode(QStringLiteral("中文"), FileManager::Encoding::Local8Bit, &used2);
        check(used2 == FileManager::Encoding::Local8Bit && FileManager::decode(gbk, used2) == QStringLiteral("中文"),
              QStringLiteral("encode: 能表示时不换编码"));
    }

    // ============================ 2. 修改标志 ============================
    {
        FileManager files;
        int modifiedSignals = 0;
        bool lastModified = false;
        QObject::connect(&files, &FileManager::modificationChanged, [&](bool m) {
            ++modifiedSignals;
            lastModified = m;
        });

        check(files.fileName() == QStringLiteral("未命名") && !files.hasFilePath(),
              QStringLiteral("新建状态: 没有路径、标题显示未命名"));
        check(!files.isModified() && files.text().isEmpty() && !files.isReadOnly(),
              QStringLiteral("新建状态: 未修改、内容为空、不是只读"));
        check(files.encoding() == FileManager::Encoding::Utf8, QStringLiteral("新建状态: 默认 UTF-8"));

        check(!files.setText(QString()), QStringLiteral("setText: 内容没变时返回 false（不置脏）"));
        check(modifiedSignals == 0 && !files.isModified(), QStringLiteral("setText: 没变就不发信号"));

        check(files.setText(QStringLiteral("# 标题")), QStringLiteral("setText: 内容变了返回 true"));
        check(files.isModified(), QStringLiteral("setText: 置脏"));
        check(modifiedSignals == 1 && lastModified, QStringLiteral("setText: 发了一次 modificationChanged(true)"));

        check(!files.setText(QStringLiteral("# 标题")), QStringLiteral("setText: 重复设置同一内容仍返回 false"));
        check(modifiedSignals == 1, QStringLiteral("setText: 不重复发信号"));

        files.setModified(false);
        check(!files.isModified() && modifiedSignals == 2 && !lastModified,
              QStringLiteral("setModified(false): 清脏并发信号"));
    }

    // ============================ 3. 四个操作（真磁盘）============================
    {
        FileManager files;
        QStringList opened;
        QStringList saved;
        QObject::connect(&files, &FileManager::fileOpened, [&opened](const QString &p) { opened << p; });
        QObject::connect(&files, &FileManager::fileSaved, [&saved](const QString &p) { saved << p; });

        // ---- 打开一个 UTF-8 文件 ----
        const QString pathUtf8 = work + QStringLiteral("/a.md");
        FileUtils::writeFileBytes(pathUtf8, QStringLiteral("# 标题\n\n正文 **粗体**\n").toUtf8());

        QString err;
        check(files.openFile(pathUtf8, &err), QStringLiteral("openFile: UTF-8 文件 -> true"), err);
        check(files.text() == QStringLiteral("# 标题\n\n正文 **粗体**\n"),
              QStringLiteral("openFile: 内容正确加载"), QStringLiteral("%1 字符").arg(files.text().size()));
        check(files.filePath() == pathUtf8 && files.fileName() == QStringLiteral("a.md"),
              QStringLiteral("openFile: 路径与文件名正确"));
        check(!files.isModified(), QStringLiteral("openFile: 脏标志已清"));
        check(files.encoding() == FileManager::Encoding::Utf8, QStringLiteral("openFile: 编码识别为 UTF-8"));
        check(opened.size() == 1 && opened.first() == pathUtf8, QStringLiteral("openFile: 发了 fileOpened 信号"));
        check(err.isEmpty(), QStringLiteral("openFile: 成功时 error 被清空"));

        // ---- 打开失败：状态必须一点不变 ----
        const QString textBefore = files.text();
        const QString pathBefore = files.filePath();
        check(!files.openFile(work + QStringLiteral("/missing.md"), &err),
              QStringLiteral("openFile: 文件不存在 -> false"));
        check(!err.isEmpty(), QStringLiteral("openFile: 失败时给出原因"), err);
        check(files.text() == textBefore && files.filePath() == pathBefore,
              QStringLiteral("openFile: 失败不破坏当前已打开的文档"));
        check(!files.openFile(work, &err), QStringLiteral("openFile: 传目录 -> false"));
        check(err.contains(QStringLiteral("文件夹")), QStringLiteral("openFile: 目录的原因说明清楚"), err);

        // ---- 修改后保存：磁盘文件必须真的更新 ----
        files.setText(QStringLiteral("# 新标题\n\n改过的内容\n"));
        check(files.isModified(), QStringLiteral("保存前: 脏标志为 true"));
        check(readRaw(pathUtf8) != files.text().toUtf8(), QStringLiteral("保存前: 磁盘内容还是旧的"));

        check(files.saveFile(&err), QStringLiteral("saveFile: 保存成功"), err);
        check(readRaw(pathUtf8) == files.text().toUtf8(),
              QStringLiteral("saveFile: ★磁盘文件已更新（内容一字不差）"),
              QStringLiteral("%1 字节").arg(readRaw(pathUtf8).size()));
        check(!files.isModified(), QStringLiteral("saveFile: 脏标志已清"));
        check(saved.size() == 1 && saved.first() == pathUtf8, QStringLiteral("saveFile: 发了 fileSaved 信号"));

        // ---- 另存为：新路径出现文件，内部路径切过去 ----
        const QString pathAs = work + QStringLiteral("/sub/deep/b.md");  // 目录都不存在，应自动创建
        check(files.saveFileAs(pathAs, &err), QStringLiteral("saveFileAs: 自动创建父目录并保存"), err);
        check(QFileInfo::exists(pathAs) && readRaw(pathAs) == files.text().toUtf8(),
              QStringLiteral("saveFileAs: 新文件内容正确"));
        check(files.filePath() == pathAs, QStringLiteral("saveFileAs: 内部路径切到新路径"));
        check(saved.size() == 2, QStringLiteral("saveFileAs: 又发了一次 fileSaved"));

        // ---- 另存为到不合法位置 ----
        check(!files.saveFileAs(work, &err), QStringLiteral("saveFileAs: 目标是目录 -> false"));
        check(files.filePath() == pathAs, QStringLiteral("saveFileAs: 失败后路径不变"));
        check(!files.saveFileAs(QString(), &err), QStringLiteral("saveFileAs: 空路径 -> false"));

        // ---- 新建：清空一切；此时 Ctrl+S 应该提示去另存为 ----
        files.newFile();
        check(files.text().isEmpty() && !files.hasFilePath() && !files.isModified(),
              QStringLiteral("newFile: 内容清空、路径丢掉、脏标志清掉"));
        check(files.encoding() == FileManager::Encoding::Utf8 && !files.isReadOnly(),
              QStringLiteral("newFile: 编码回到 UTF-8、只读状态清掉"));
        check(!files.saveFile(&err), QStringLiteral("newFile 后 saveFile: 没有路径 -> false"));
        check(err.contains(QStringLiteral("另存为")), QStringLiteral("saveFile 无路径时提示走另存为"), err);

        // ---- 往返：GBK 文档编辑后保存，编码不能变 ----
        // （只在"本机编码不是 UTF-8"的机器上有意义，判据同前）
        const QByteArray localBytes = QStringLiteral("老文档：中文内容\n").toLocal8Bit();
        if (localBytes != QStringLiteral("老文档：中文内容\n").toUtf8()) {
            const QString pathLocal = work + QStringLiteral("/gbk.md");
            writeRaw(pathLocal, localBytes);

            FileManager local;
            check(local.openFile(pathLocal, &err), QStringLiteral("GBK 文档: 打开成功"), err);
            check(local.encoding() == FileManager::Encoding::Local8Bit,
                  QStringLiteral("GBK 文档: 编码识别为本机编码"));
            check(local.text() == QStringLiteral("老文档：中文内容\n"),
                  QStringLiteral("GBK 文档: 中文没有乱码"));

            local.setText(QStringLiteral("老文档：改过了\n"));
            check(local.saveFile(&err), QStringLiteral("GBK 文档: 保存成功"), err);
            check(local.encoding() == FileManager::Encoding::Local8Bit,
                  QStringLiteral("GBK 文档: 保存后仍是本机编码（不回退成 UTF-8）"));
            check(readRaw(pathLocal) == QStringLiteral("老文档：改过了\n").toLocal8Bit(),
                  QStringLiteral("GBK 文档: 磁盘上仍是 GBK 字节"));

            // 再打开一次，确认往返一致
            FileManager again;
            again.openFile(pathLocal, &err);
            check(again.text() == QStringLiteral("老文档：改过了\n") && !again.isModified(),
                  QStringLiteral("GBK 文档: 重新打开内容一致、未修改"));
        }
    }

    // ============================ 3b. 文件时间戳 ============================
    // （4.2.1 从 MarkdownDocument 搬过来的：这属于"文件层面"的信息，要碰磁盘）
    {
        FileManager files;
        check(!files.modifiedTime().isValid() && !files.createdTime().isValid(),
              QStringLiteral("时间戳: 没有路径时是无效时间（用 isValid 判断）"));

        const QString stamped = work + QStringLiteral("/times.md");
        FileUtils::writeFileBytes(stamped, QByteArray("x"));

        QString err;
        check(files.openFile(stamped, &err), QStringLiteral("时间戳: 打开文件"), err);
        check(files.modifiedTime().isValid(), QStringLiteral("时间戳: 文件存在 -> modifiedTime 有效"));
        check(files.createdTime().isValid(), QStringLiteral("时间戳: 文件存在 -> createdTime 有效"));

        files.newFile();
        check(!files.modifiedTime().isValid(), QStringLiteral("时间戳: newFile 之后又变成无效"));
    }

    // ============================ 3c. 二进制文件（拒绝打开，且不误伤文本）============================
    // 这一段是真实故障的回归测试：在文件树里双击到 .pdb / .ilk / .docx 这类二进制文件时，
    // 内容被当成文本读进来，再整段推给预览，把预览的渲染进程拖死 ——
    // 界面从此一片空白，连日志都不报错，只能重启程序。
    // 所以现在：二进制文件在"打开"这一步就被明确拒绝（并说清原因）。
    {
        // ---- 纯函数层面的规则 ----
        check(!FileManager::looksBinary(QString()), QStringLiteral("binary: 空内容 -> 不是二进制"));
        check(!FileManager::looksBinary(QStringLiteral("# 标题\n\n正文，中文英文混排\n")),
              QStringLiteral("binary: 正常中英文文本 -> 不是二进制"));
        check(!FileManager::looksBinary(QStringLiteral("第一行\r\n第二行\t带制表符和换页\f")),
              QStringLiteral("binary: 制表符/换行/换页都算正常文本"));
        check(FileManager::looksBinary(QStringLiteral("abc\x00") + QStringLiteral("def")),
              QStringLiteral("binary: 出现 NUL 字符 -> 是二进制"));
        check(FileManager::looksBinary(QString(200, QChar(1))),
              QStringLiteral("binary: 大量控制字符 -> 是二进制"));
        check(!FileManager::looksBinary(QString(200, QLatin1Char('a')) + QChar(0x1A)),
              QStringLiteral("binary: 只末尾一个控制字符（老文件用 ^Z 结尾）-> 仍算文本"));

        // ---- 真的读盘：二进制文件必须被拒绝，而且当前文档一点都不能变 ----
        const QString binPath = work + QStringLiteral("/fake.exe");
        QByteArray bin("MZ");  // 假装是个可执行文件头
        bin.append(QByteArray(64, '\0'));
        for (int i = 0; i < 256; ++i) {
            bin.append(char(i));
        }
        writeRaw(binPath, bin);

        FileManager files;
        const QString textPath = work + QStringLiteral("/text-before-binary.md");
        FileUtils::writeFileBytes(textPath, QStringLiteral("打开失败不该影响这份内容\n").toUtf8());
        QString err;
        check(files.openFile(textPath, &err), QStringLiteral("binary: 先打开一个正常文档"), err);
        const QString textBefore = files.text();

        check(!files.openFile(binPath, &err), QStringLiteral("binary: 打开二进制文件 -> false"));
        check(err.contains(QStringLiteral("二进制")), QStringLiteral("binary: 原因里明说是二进制文件"), err);
        check(files.text() == textBefore && files.filePath() == textPath,
              QStringLiteral("binary: 拒绝之后当前文档完全没变"));
        check(!files.isModified(), QStringLiteral("binary: 拒绝之后脏标志也没被弄脏"));

        // ---- ★ 绝不能误伤：UTF-16（原始字节里满是 0x00）和 GBK 都必须照常打开 ----
        // 这是"在解码之后判断"这条设计的价值所在：UTF-16 文本的 0x00 在解码时已经
        // 和相邻字节配成字符了，所以它不会被误判成二进制。
        const QString u16Path = work + QStringLiteral("/utf16-note.md");
        writeRaw(u16Path, FileManager::encode(QStringLiteral("# UTF-16 标题\n\n正文\n"),
                                             FileManager::Encoding::Utf16LE));
        check(readRaw(u16Path).contains('\0'),
              QStringLiteral("binary 反例: 这个 UTF-16 文件的原始字节里确实有 0x00"));
        check(files.openFile(u16Path, &err), QStringLiteral("binary 反例: UTF-16 文档照常打开"), err);
        check(files.text() == QStringLiteral("# UTF-16 标题\n\n正文\n") && !files.isModified(),
              QStringLiteral("binary 反例: 内容正确（没有被误判成二进制）"),
              QStringLiteral("%1 字符").arg(files.text().size()));

        const QString gbkPath = work + QStringLiteral("/gbk-note.md");
        writeRaw(gbkPath, FileManager::encode(QStringLiteral("中文笔记，GBK 编码\n"), FileManager::Encoding::Local8Bit));
        check(files.openFile(gbkPath, &err), QStringLiteral("binary 反例: GBK 文档照常打开"), err);
        check(files.text() == QStringLiteral("中文笔记，GBK 编码\n"),
              QStringLiteral("binary 反例: GBK 内容正确"), files.text());
    }

    // ============================ 3d. 内存：缓存的归属与上限（7.2）============================
    // "关闭文件释放缓存"在这个项目里靠的是**归属设计**：CacheManager 是 FileManager 的
    // 值成员，一个文档一个缓存 —— 标签一关、FileManager 一销毁，缓存跟着消失，
    // 不存在"关掉的文档还占着内存"。这里把这条钉住。
    {
        const QString cacheDoc = work + QStringLiteral("/cache-owner.md");
        FileUtils::writeFileBytes(cacheDoc, QStringLiteral("缓存归属测试\n").toUtf8());

        {
            FileManager session;
            QString err;
            check(session.openFile(cacheDoc, &err), QStringLiteral("缓存: 打开一个文件"), err);
            check(session.cacheManager()->size() > 0,
                  QStringLiteral("缓存: 打开之后这份内容进了缓存"),
                  QStringLiteral("%1 条").arg(session.cacheManager()->size()));
            check(session.cacheManager()->maxBytes() > 0,
                  QStringLiteral("缓存: 有字节预算（不会无限增长）"),
                  QStringLiteral("预算 %1 字节").arg(session.cacheManager()->maxBytes()));
        }

        // 上面那个 FileManager 已经销毁（等价于标签被关掉）：新的必须是"干净"的
        FileManager fresh;
        check(fresh.cacheManager()->size() == 0,
              QStringLiteral("缓存: 上一个文档销毁后，新的从空缓存开始（不跨文档累积）"));
        check(fresh.cacheManager()->hits() == 0 && fresh.cacheManager()->misses() == 0,
              QStringLiteral("缓存: 命中等统计也是新的（每个文档各算各的）"));
    }

    // ============================ 3e. 外部修改检测（7.3）============================
    {
        const QString extDoc = work + QStringLiteral("/external.md");
        FileUtils::writeFileBytes(extDoc, QStringLiteral("第一版内容\n").toUtf8());

        FileManager files;
        QString err;
        check(files.openFile(extDoc, &err), QStringLiteral("外部修改: 打开文件"), err);

        check(!files.hasExternalChange(), QStringLiteral("外部修改: 刚打开时没有变化"));
        check(files.externalChangeReason().isEmpty(), QStringLiteral("外部修改: 原因也是空的"));

        // 模拟"别的程序改了它"：内容变长（大小一定不同，不必依赖时间戳精度）
        FileUtils::writeFileBytes(extDoc, QStringLiteral("第一版内容\n别人加了一行\n").toUtf8());
        check(files.hasExternalChange(), QStringLiteral("外部修改: 磁盘被改之后能检测到"));
        check(files.externalChangeReason().contains(QStringLiteral("别的程序")),
              QStringLiteral("外部修改: 原因是「被别的程序修改」，能直接展示给用户"),
              files.externalChangeReason());
        check(files.text() == QStringLiteral("第一版内容\n"),
              QStringLiteral("外部修改: 检测是「只报告」，编辑器里的内容没被动"),
              files.text());

        // ---- 用户选"保留我的"：把当前磁盘状态记成基准，不再反复提示 ----
        files.acceptCurrentDiskState();
        check(!files.hasExternalChange(), QStringLiteral("外部修改: 选择保留之后不再提示"));

        // ---- 用户选"重载"：内容换成磁盘上的，脏标志清掉 ----
        FileUtils::writeFileBytes(extDoc, QStringLiteral("磁盘上的第三版\n").toUtf8());
        check(files.hasExternalChange(), QStringLiteral("外部修改: 又一次外部改动被检测到"));
        err.clear();
        check(files.reloadFromDisk(&err), QStringLiteral("外部修改: 重载成功"), err);
        check(files.text() == QStringLiteral("磁盘上的第三版\n"),
              QStringLiteral("外部修改: 内容换成了磁盘上的版本"), files.text());
        check(!files.isModified(), QStringLiteral("外部修改: 重载后脏标志被清掉（刚读的就是磁盘上的）"));
        check(!files.hasExternalChange(), QStringLiteral("外部修改: 重载之后基准就是磁盘，不再报变化"));

        // ---- 文件被删掉也算外部变化，而且重载要失败得干净 ----
        const QString beforeDelete = files.text();
        QFile::remove(extDoc);
        check(files.hasExternalChange(), QStringLiteral("外部修改: 文件被删掉也算变化"));
        check(files.externalChangeReason().contains(QStringLiteral("删除")),
              QStringLiteral("外部修改: 原因里说清是「被删除/移走」"), files.externalChangeReason());
        err.clear();
        check(!files.reloadFromDisk(&err), QStringLiteral("外部修改: 文件不在时重载失败"));
        check(!err.isEmpty(), QStringLiteral("外部修改: 并且给出原因"), err);
        check(files.text() == beforeDelete,
              QStringLiteral("外部修改: ★重载失败时当前内容一个字节都没动"));

        // 文件不在了、用户又选"保留我的"：不该每次都问
        files.acceptCurrentDiskState();
        check(!files.hasExternalChange(), QStringLiteral("外部修改: 文件不存在时也能把基准「记下来」"));

        // 没保存过的新文档谈不上外部修改
        FileManager fresh;
        fresh.setText(QStringLiteral("还没保存过"));
        check(!fresh.hasExternalChange(), QStringLiteral("外部修改: 新文档（没有路径）永远不报外部修改"));
        err.clear();
        check(!fresh.reloadFromDisk(&err), QStringLiteral("外部修改: 新文档没有可重载的东西（失败而不是崩）"));
        check(!err.isEmpty(), QStringLiteral("外部修改: 并且说明了原因"), err);
    }

    // ============================ 4. 只读文件 ============================
    {
        const QString pathRo = work + QStringLiteral("/readonly.md");
        FileUtils::writeFileBytes(pathRo, QStringLiteral("只读内容\n").toUtf8());

        // 设成只读（Windows 上就是文件的"只读"属性）
        QFile::setPermissions(pathRo,
                              QFileDevice::ReadOwner | QFileDevice::ReadUser | QFileDevice::ReadGroup
                                  | QFileDevice::ReadOther);

        check(FileManager::isReadOnlyFile(pathRo), QStringLiteral("readonly: 识别出只读属性"));
        check(!FileManager::isReadOnlyFile(work + QStringLiteral("/nope.md")),
              QStringLiteral("readonly: 不存在的文件不算只读"));
        // 4.2.3 加的 QFileInfo 重载（打开文件时复用已经查好的文件状态，省一次 stat）
        check(FileManager::isReadOnlyFile(QFileInfo(pathRo)),
              QStringLiteral("readonly: QFileInfo 重载与路径版本结论一致"));
        check(FileManager::isReadOnlyFile(QFileInfo(pathRo)) == FileManager::isReadOnlyFile(pathRo),
              QStringLiteral("readonly: 两个重载对同一个文件答案相同"));
        check(FileManager::writabilityProblem(pathRo).contains(QStringLiteral("只读")),
              QStringLiteral("writabilityProblem: 只读文件给出人话原因"));
        check(FileManager::writabilityProblem(work + QStringLiteral("/ok.md")).isEmpty(),
              QStringLiteral("writabilityProblem: 可写路径返回空"));
        check(FileManager::writabilityProblem(work).contains(QStringLiteral("文件夹")),
              QStringLiteral("writabilityProblem: 目录被拦下"));

        FileManager files;
        bool readOnlySignal = false;
        QString readOnlyReason;
        QObject::connect(&files, &FileManager::readOnlyDetected, [&](const QString &, const QString &reason) {
            readOnlySignal = true;
            readOnlyReason = reason;
        });

        QString err;
        check(files.openFile(pathRo, &err), QStringLiteral("readonly: 打开只读文件仍然成功"), err);
        check(files.isReadOnly(), QStringLiteral("readonly: isReadOnly() 为 true"));
        check(readOnlySignal && !readOnlyReason.isEmpty(),
              QStringLiteral("readonly: 发了 readOnlyDetected（带可展示的原因）"));
        check(files.text() == QStringLiteral("只读内容\n"), QStringLiteral("readonly: 内容正确"));

        // 能改（内存里），但保存必须失败，且不能假装保存成功
        check(files.setText(QStringLiteral("想改的内容\n")), QStringLiteral("readonly: 允许编辑内容"));
        check(!files.saveFile(&err), QStringLiteral("readonly: 保存 -> false（不假装成功）"));
        check(err.contains(QStringLiteral("只读")), QStringLiteral("readonly: 失败原因说明是只读"), err);
        check(files.isModified(), QStringLiteral("readonly: 保存失败后脏标志仍然保留"));
        check(readRaw(pathRo) == QStringLiteral("只读内容\n").toUtf8(),
              QStringLiteral("readonly: ★磁盘内容没有被改坏"));

        // 用「另存为」可以救出来
        const QString rescued = work + QStringLiteral("/rescued.md");
        check(files.saveFileAs(rescued, &err), QStringLiteral("readonly: 另存为到别处可以成功"), err);
        check(readRaw(rescued) == QStringLiteral("想改的内容\n").toUtf8(),
              QStringLiteral("readonly: 另存为的内容正确"));
        check(!files.isModified(), QStringLiteral("readonly: 另存为后脏标志清掉"));

        // 收尾：把只读属性去掉，否则临时目录删不掉
        QFile::setPermissions(pathRo,
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser
                                  | QFileDevice::WriteUser);
    }

    // ============================ 5. 保存时自动打快照（4.2.2 集成）============================
    // 这里验证的是"两个模块接在一起对不对"：保存一次 → 留下一条历史；改内容再存 → 又一条。
    // 需要机器上有 git；没有就跳过（这不是本项目代码的问题）。
    if (VersionControl::isGitAvailable()) {
        const QString histRoot = work + QStringLiteral("/history-root");
        FileManager files;
        // 把快照仓库指到临时目录，绝不动 AppData 里那份真实历史
        files.versionControl()->setHistoryRoot(histRoot);

        const QString doc = work + QStringLiteral("/snapshot-demo.md");
        FileUtils::writeFileBytes(doc, QStringLiteral("第一版\n").toUtf8());

        QString err;
        check(files.openFile(doc, &err), QStringLiteral("自动快照: 打开文档"), err);
        check(files.saveFile(&err), QStringLiteral("自动快照: 第一次保存"), err);

        const QString repoDir = files.versionControl()->repositoryPathFor(doc);
        const auto historyMessages = [&files, &repoDir]() {
            QString listErr;
            const QList<VersionControl::Commit> commits = files.versionControl()->history(repoDir, 20, &listErr);
            QStringList out;
            for (const VersionControl::Commit &commit : commits) {
                out << commit.message;
            }
            return out;
        };

        QStringList messages = historyMessages();
        check(messages.size() == 1, QStringLiteral("自动快照: 保存一次就有 1 条历史"),
              QStringLiteral("%1 条").arg(messages.size()));
        check(!messages.isEmpty() && messages.first().startsWith(QStringLiteral("保存快照 ")),
              QStringLiteral("自动快照: 备注带时间戳"), messages.value(0));

        files.setText(QStringLiteral("第二版\n"));
        check(files.saveFile(&err), QStringLiteral("自动快照: 改内容后再保存"), err);
        check(historyMessages().size() == 2, QStringLiteral("自动快照: 变成 2 条历史（每次保存一条）"));

        // 关掉自动快照后不该再增加
        files.setAutoSnapshotEnabled(false);
        files.setText(QStringLiteral("第三版\n"));
        files.saveFile(&err);
        check(historyMessages().size() == 2, QStringLiteral("自动快照: 关掉之后保存不再产生历史"));
        files.setAutoSnapshotEnabled(true);

        // 注意这里的分界：快照比的是"相对**上一个快照**有没有变化"，不是"相对上一次保存"。
        // 刚才那次保存是在关掉开关时做的，所以内容还没进历史 —— 重新打开开关后再保存，
        // 会正常补上一次提交（这是对的）。
        check(files.saveFile(&err), QStringLiteral("自动快照: 重新打开开关后保存"), err);
        const int afterCatchUp = historyMessages().size();
        check(afterCatchUp == 3, QStringLiteral("自动快照: 补上关掉期间漏掉的那一版"),
              QStringLiteral("%1 条").arg(afterCatchUp));

        // 真正的"内容没变"：连着保存两次，第二次不该新增
        check(files.saveFile(&err), QStringLiteral("自动快照: 重复保存同一内容仍算保存成功"), err);
        check(historyMessages().size() == afterCatchUp,
              QStringLiteral("自动快照: 内容没变 -> 不新增历史"));
    } else {
        std::printf("%-58s SKIP  [系统里没找到 git]\n", "自动快照: 保存时创建历史");
    }

    // ============================ 6. 缓存服务（4.2.3 集成）============================
    // 验收点是"重复打开同一个文件，第二次直接走缓存、更快"。这里不光看统计数字，
    // 还用一个"铁证"来证明第二次真的没有读盘（见下面那段注释）。
    {
        FileManager files;
        files.cacheManager()->setMaxBytes(32);  // 预算调小，方便验证淘汰（几行文本 32 字节够放 2~3 条）

        const QString docA = work + QStringLiteral("/cache-a.md");
        const QString docB = work + QStringLiteral("/cache-b.md");
        const QString docC = work + QStringLiteral("/cache-c.md");
        FileUtils::writeFileBytes(docA, QStringLiteral("甲文件内容\n").toUtf8());
        FileUtils::writeFileBytes(docB, QStringLiteral("乙文件内容\n").toUtf8());
        FileUtils::writeFileBytes(docC, QStringLiteral("丙文件内容\n").toUtf8());

        QString err;
        check(files.openFile(docA, &err), QStringLiteral("缓存: 第一次打开 A"), err);
        check(files.cacheManager()->size() == 1, QStringLiteral("缓存: 打开后内容进了缓存"));
        check(files.cacheManager()->misses() == 1 && files.cacheManager()->hits() == 0,
              QStringLiteral("缓存: 第一次是未命中（老老实实读了盘）"));

        check(files.openFile(docA, &err), QStringLiteral("缓存: 第二次打开 A"));
        check(files.cacheManager()->hits() == 1, QStringLiteral("缓存: 第二次命中（★这就是验收点）"));
        check(files.text() == QStringLiteral("甲文件内容\n"), QStringLiteral("缓存: 命中时内容正确"));

        // ---- 铁证：证明第二次真的没读盘 ----
        // 把磁盘上的内容换成**同样长度**的别的内容，再把修改时间改回缓存里记的那个值。
        // 于是"修改时间 + 字节数"两项都对得上，缓存察觉不到 —— 打开拿到的仍是缓存里的旧内容。
        // 只有"根本没看磁盘"才会是这个结果。
        const QDateTime cachedTime = QFileInfo(docA).lastModified();
        FileUtils::writeFileBytes(docA, QStringLiteral("乙文件内容\n").toUtf8());  // 与甲文件内容等长
        {
            QFile fix(docA);
            fix.open(QIODevice::ReadWrite);
            // Qt 6.5 的 setFileTime 没有默认参数，必须显式说明改的是"修改时间"
            fix.setFileTime(cachedTime, QFileDevice::FileModificationTime);
            fix.close();
        }
        const bool trickHit = (files.openFile(docA, &err)
                               && files.text() == QStringLiteral("甲文件内容\n")
                               && files.cacheManager()->hits() == 2);
        check(trickHit, QStringLiteral("缓存: ★内容变了但时间/大小没变 → 仍返回缓存内容（证明没读盘）"));

        // 把故意制造的错位清掉：先丢掉那条记录、重新打开一次，让缓存和磁盘重新对齐。
        // （必须先清掉再打开，否则下面那次"文件被外部改过"会被算成未命中而不是过期 —— 我第一版就写错了。）
        files.cacheManager()->remove(docA);
        check(files.openFile(docA, &err), QStringLiteral("缓存: 重新打开 A，让缓存与磁盘对齐"), err);

        // ---- 文件被别的程序改过（大小也变了）→ 必须重新读盘，不能拿旧内容顶 ----
        FileUtils::writeFileBytes(docA, QStringLiteral("甲文件内容被别的程序改长了\n").toUtf8());
        check(files.openFile(docA, &err), QStringLiteral("缓存: 文件被外部改过后重新打开"), err);
        check(files.text() == QStringLiteral("甲文件内容被别的程序改长了\n"),
              QStringLiteral("缓存: ★过期后重新读盘（不会用旧内容覆盖别人的改动）"));
        check(files.cacheManager()->staleCount() >= 1, QStringLiteral("缓存: 记了一次过期"));

        // ---- 保存之后，缓存里应该就是刚保存的内容 ----
        files.setText(QStringLiteral("保存后的新内容\n"));
        check(files.saveFile(&err), QStringLiteral("缓存: 保存"), err);
        check(files.openFile(docA, &err) && files.text() == QStringLiteral("保存后的新内容\n"),
              QStringLiteral("缓存: 保存后重新打开，内容与磁盘一致"));

        // ---- 淘汰：用三个**独立、内容等长**的文件验证 LRU（字节预算模型下，长度必须可控）----
        // 预算 32 字节，每条 16 字节恰好放 2 条；访问 A 再打开 C，被淘汰的应是 B（LRU）。
        // 用独立文件而不是复用 docA：docA 在前面已被改成"保存后的新内容"（22 字节），
        // 长度不再是 16 字节，会污染字节预算的推算。
        const QString lruA = work + QStringLiteral("/lru-a.md");
        const QString lruB = work + QStringLiteral("/lru-b.md");
        const QString lruC = work + QStringLiteral("/lru-c.md");
        FileUtils::writeFileBytes(lruA, QStringLiteral("甲甲甲甲甲\n").toUtf8());  // 5 字 + 换行 = 16 字节
        FileUtils::writeFileBytes(lruB, QStringLiteral("乙乙乙乙乙\n").toUtf8());
        FileUtils::writeFileBytes(lruC, QStringLiteral("丙丙丙丙丙\n").toUtf8());

        files.cacheManager()->clear();
        check(files.openFile(lruA, &err), QStringLiteral("缓存: 打开 A"), err);
        check(files.openFile(lruB, &err), QStringLiteral("缓存: 打开 B"), err);
        check(files.openFile(lruA, &err), QStringLiteral("缓存: 再打开 A（刷新它的最近使用时间）"), err);
        check(files.openFile(lruC, &err), QStringLiteral("缓存: 打开 C（触发淘汰）"), err);
        check(files.cacheManager()->size() == 2, QStringLiteral("缓存: 条数不超过预算能放下的条数"));
        check(!files.cacheManager()->contains(lruB), QStringLiteral("缓存: 最久未使用的 B 被淘汰"));
        check(files.cacheManager()->contains(lruA), QStringLiteral("缓存: 刚访问过的 A 还在（LRU）"));

        // ---- 计时参考（只打印不断言：机器负载会让时间抖动，断言会变成不稳定的测试）----
        QElapsedTimer timer;
        timer.start();
        files.cacheManager()->remove(docA);
        files.openFile(docA, &err);
        const double fromDisk = double(timer.nsecsElapsed()) / 1e6;
        timer.restart();
        files.openFile(docA, &err);
        const double fromCache = double(timer.nsecsElapsed()) / 1e6;
        std::printf("%-58s %s\n",
                    "计时参考（不是断言）",
                    QStringLiteral("读盘 %1 ms → 命中缓存 %2 ms")
                        .arg(fromDisk, 0, 'f', 3)
                        .arg(fromCache, 0, 'f', 3)
                        .toUtf8()
                        .constData());
    }

    // ============================ 7. 回滚到历史版本（4.2.2）============================
    // 契约里最要紧的一条：回滚**只改内存、不写盘**，所以它永远是可撤销的。
    if (VersionControl::isGitAvailable()) {
        const QString rollbackRoot = work + QStringLiteral("/history-root-rollback");
        FileManager files;
        files.versionControl()->setHistoryRoot(rollbackRoot);

        const QString doc = work + QStringLiteral("/rollback-demo.md");
        FileUtils::writeFileBytes(doc, QStringLiteral("第一版内容\n").toUtf8());

        QString err;
        check(files.openFile(doc, &err) && files.saveFile(&err), QStringLiteral("回滚: 打开并保存第一版"), err);

        const QString repoDir = files.versionControl()->repositoryPathFor(doc);
        const QString hash1 = files.versionControl()->history(repoDir, 5, &err).value(0).hash;
        check(!hash1.isEmpty(), QStringLiteral("回滚: 拿到第一版的哈希"));

        files.setText(QStringLiteral("第二版内容\n"));
        check(files.saveFile(&err), QStringLiteral("回滚: 保存第二版"), err);

        // ---- 回滚到第一版：内存变了，磁盘不能变 ----
        check(files.restoreSnapshot(hash1, &err), QStringLiteral("回滚: 回到第一版"), err);
        check(files.text() == QStringLiteral("第一版内容\n"), QStringLiteral("回滚: ★内容已换回第一版"));
        check(files.isModified(), QStringLiteral("回滚: 标成已修改（好提醒用户保存）"));

        QByteArray onDisk;
        FileUtils::readFileBytes(doc, onDisk);
        check(onDisk == QStringLiteral("第二版内容\n").toUtf8(),
              QStringLiteral("回滚: ★磁盘没被动过（所以回滚随时可以放弃）"));

        // ---- 用户确认之后保存，磁盘才变 ----
        check(files.saveFile(&err), QStringLiteral("回滚: 用户确认后保存"), err);
        FileUtils::readFileBytes(doc, onDisk);
        check(onDisk == QStringLiteral("第一版内容\n").toUtf8(), QStringLiteral("回滚: 保存后磁盘变成第一版"));
        check(!files.isModified(), QStringLiteral("回滚: 保存后不再是已修改"));

        // ---- 失败路径：状态一个字节都不能动 ----
        const QString before = files.text();
        check(!files.restoreSnapshot(QStringLiteral("不存在的版本"), &err) && !err.isEmpty(),
              QStringLiteral("回滚: 版本号不存在 → false + 原因"), err);
        check(files.text() == before, QStringLiteral("回滚: 失败时当前内容一点没变"));

        FileManager fresh;  // 没保存过的新文档
        fresh.versionControl()->setHistoryRoot(rollbackRoot);
        check(!fresh.restoreSnapshot(hash1, &err) && !err.isEmpty(),
              QStringLiteral("回滚: 没保存过的文档 → false + 原因"), err);
    } else {
        std::printf("%-58s SKIP  [系统里没找到 git]\n", "回滚到历史版本");
    }

    QDir(work).removeRecursively();

    if (g_fail == 0) {
        std::printf("\n=== FileManager 契约测试：全部通过 ===\n");
    } else {
        std::printf("\n=== FileManager 契约测试：%d 项失败 ===\n", g_fail);
    }
    return g_fail == 0 ? 0 : 1;
}
