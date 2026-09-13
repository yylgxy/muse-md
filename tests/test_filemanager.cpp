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

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <cstdio>

using markdown_editor::core::storage::FileManager;

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

    QDir(work).removeRecursively();

    if (g_fail == 0) {
        std::printf("\n=== FileManager 契约测试：全部通过 ===\n");
    } else {
        std::printf("\n=== FileManager 契约测试：%d 项失败 ===\n", g_fail);
    }
    return g_fail == 0 ? 0 : 1;
}
