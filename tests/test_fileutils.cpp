// FileUtils 的契约测试：把 fileutils.h 里写的每一条约定都验一遍。
//
// 跑法（二选一）：
//   1) 在构建目录里执行:  ctest -C Debug --output-on-failure
//   2) 直接运行 bin/Debug/test_fileutils.exe（PASS/FAIL 打印在控制台，退出码 0 = 全部通过）
//
// 测试文件全部写在系统临时目录下的 md-editor-fileutils-test/，不会污染源码树。

#include "fileutils.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <cstdio>

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

// 绕过 FileUtils，直接按原始字节造文件（用来造 BOM 文件这类特殊样本）
void writeRaw(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(bytes);
    file.close();
}

}  // namespace

int main()
{
    const QString work = QDir(QDir::tempPath()).filePath(QStringLiteral("md-editor-fileutils-test"));
    QDir(work).removeRecursively();
    if (!QDir().mkpath(work)) {
        std::printf("cannot create temp dir: %s\n", work.toUtf8().constData());
        return 2;
    }

    QString err;

    // ---------------- exists ----------------
    check(FileUtils::exists(work), "exists: directory -> true");
    check(!FileUtils::exists(work + "/nope.md"), "exists: missing -> false");

    // ---------------- getSuffix（fileutils.h 契约里的 8 条）----------------
    struct Case
    {
        const char *path;
        const char *expect;
    };
    const Case cases[] = {
        {"D:/a/b.md", "md"},
        {"D:/a/b.MD", "md"},
        {"D:/a/notes.tar.gz", "gz"},
        {".gitignore", "gitignore"},
        {"noext", ""},
        {"D:/v1.2/readme", ""},
        {"a.", ""},
        {"", ""},
    };
    for (const Case &c : cases) {
        const QString got = FileUtils::getSuffix(QString::fromUtf8(c.path));
        const QString want = QString::fromUtf8(c.expect);
        check(got == want, QStringLiteral("getSuffix(\"%1\")").arg(QString::fromUtf8(c.path)),
              QStringLiteral("got=[%1] want=[%2]").arg(got, want));
    }

    // ---------------- readFile：各种失败与边界 ----------------
    QString content = QStringLiteral("SENTINEL");
    err = QStringLiteral("STALE");
    bool ok = FileUtils::readFile(work + "/missing.md", content, &err);
    check(!ok, "readFile: missing file -> false");
    check(content == QStringLiteral("SENTINEL"), "readFile: content untouched on failure");
    check(!err.isEmpty(), "readFile: error message set on failure");

    content = QStringLiteral("SENTINEL");
    err.clear();
    ok = FileUtils::readFile(work, content, &err);
    check(!ok, "readFile: directory -> false");
    check(content == QStringLiteral("SENTINEL"), "readFile: content untouched (dir case)");

    writeRaw(work + "/empty.md", QByteArray());
    content = QStringLiteral("SENTINEL");
    ok = FileUtils::readFile(work + "/empty.md", content, &err);
    check(ok && content.isEmpty(), "readFile: empty file -> true + empty string");

    writeRaw(work + "/bom.md", QByteArray("\xEF\xBB\xBF") + QByteArray("hello"));
    content.clear();
    ok = FileUtils::readFile(work + "/bom.md", content, &err);
    check(ok && content == QStringLiteral("hello"), "readFile: UTF-8 BOM stripped",
          QStringLiteral("len=%1 firstU+%2")
              .arg(content.length())
              .arg(content.isEmpty() ? 0 : int(content.at(0).unicode()), 4, 16, QLatin1Char('0')));

    // ---------------- 往返：中文 + 换行符原样 ----------------
    const QString original = QString::fromUtf8("中文标题\n第二行\nend");
    err = QStringLiteral("STALE");
    ok = FileUtils::writeFile(work + "/rt.md", original, &err);
    QString back;
    const bool ok2 = FileUtils::readFile(work + "/rt.md", back, &err);
    check(ok && ok2 && back == original, "round-trip: write + read == original",
          QStringLiteral("written=%1B read=%2B").arg(original.toUtf8().size()).arg(back.toUtf8().size()));

    const QString mixed = QString::fromUtf8("a\r\nb\nc\r\n");
    FileUtils::writeFile(work + "/mixed.md", mixed, &err);
    QString mixedBack;
    FileUtils::readFile(work + "/mixed.md", mixedBack, &err);
    check(mixedBack == mixed, "round-trip: mixed CRLF/LF preserved byte-for-byte",
          QStringLiteral("in=%1B out=%2B").arg(mixed.toUtf8().size()).arg(mixedBack.toUtf8().size()));

    // ---------------- writeFile：error 清空 / 覆盖 / 建目录 ----------------
    err = QStringLiteral("STALE");
    FileUtils::writeFile(work + "/rt2.md", QStringLiteral("x"), &err);
    check(err.isEmpty(), "writeFile: error cleared on success", QStringLiteral("errlen=%1").arg(err.length()));

    FileUtils::writeFile(work + "/trunc.md", QString(1000, QLatin1Char('A')));
    FileUtils::writeFile(work + "/trunc.md", QStringLiteral("BBB"));
    QString truncated;
    FileUtils::readFile(work + "/trunc.md", truncated, &err);
    check(truncated == QStringLiteral("BBB"), "writeFile: overwrite truncates old content",
          QStringLiteral("len=%1").arg(truncated.length()));

    const QString deep = work + "/sub/deep/a.md";
    err.clear();
    ok = FileUtils::writeFile(deep, QStringLiteral("x"), &err);
    check(ok && QFileInfo::exists(deep), "writeFile: creates missing parent dirs");

    writeRaw(work + "/afile", QByteArray("x"));
    err.clear();
    ok = FileUtils::writeFile(work + "/afile/child.md", QStringLiteral("x"), &err);
    check(!ok, "writeFile: parent path is a file -> false (no crash)");

    // ---------------- writeFile：失败时不能破坏原文件（QSaveFile 的意义）----------------
    const QString readOnly = work + "/readonly.md";
    writeRaw(readOnly, QByteArray("ORIGINAL"));
    QFile::setPermissions(readOnly, QFileDevice::ReadOwner | QFileDevice::ReadUser);
    err.clear();
    ok = FileUtils::writeFile(readOnly, QStringLiteral("NEW"), &err);
    QString readOnlyContent;
    FileUtils::readFile(readOnly, readOnlyContent, &err);
    check(!ok && readOnlyContent == QStringLiteral("ORIGINAL"),
          "writeFile: failed write leaves original intact",
          QStringLiteral("ok=%1 content=%2B").arg(ok ? 1 : 0).arg(readOnlyContent.toUtf8().size()));
    QFile::setPermissions(readOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                       | QFileDevice::ReadUser | QFileDevice::WriteUser);

    QDir(work).removeRecursively();

    std::printf("\nFAIL count = %d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
