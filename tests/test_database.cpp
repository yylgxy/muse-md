// DataBase 的契约测试：把 database.h 里写的约定验一遍。
//
// 跑法：ctest -C Debug --output-on-failure   或直接运行 bin/Debug/test_database.exe
//
// 注意两点：
//  1. QSqlDatabase 需要一个 QCoreApplication（Qt 靠它加载 SQL 驱动插件），
//     所以这个测试和 test_fileutils 不同，必须建 app 对象。
//  2. 测试全在系统临时目录里做，不碰你的真实数据。

#include "database.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>

#include <cstdio>

namespace {

int g_fail = 0;

void check(bool ok, const QString &what, const QString &detail = QString())
{
    std::printf("%-54s %s", what.toUtf8().constData(), ok ? "PASS" : "FAIL");
    if (!detail.isEmpty()) {
        std::printf("  [%s]", detail.toUtf8().constData());
    }
    std::printf("\n");
    if (!ok) {
        ++g_fail;
    }
}

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("MarkdownEditorTest"));

    const QString work = QDir(QDir::tempPath()).filePath(QStringLiteral("md-editor-db-test"));
    QDir(work).removeRecursively();

    // 故意用一个还不存在的多级目录：openDB 应该自动建出来
    const QString dbPath = work + QStringLiteral("/sub/deep/notes.db");

    DataBase &db = DataBase::getInstance();

    // ---------------- 打开 ----------------
    check(db.openDB(dbPath), "openDB: 父目录不存在也能打开（自动创建）");
    check(QFileInfo::exists(dbPath), "openDB: 数据库文件已生成");
    check(db.isOpen(), "isOpen: true");

    const QList<QVariantMap> tables = db.querySQL(
        QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' AND name='note_meta'"));
    check(tables.size() == 1, "initTable: note_meta 表已建好");

    // ---------------- 插入：重点验"值里有单引号和 %1" ----------------
    NoteMeta meta;
    meta.filePath = work + QStringLiteral("/a%1b/note's.md");  // 路径里同时有 %1 和单引号
    meta.title = QStringLiteral("It's a %1 test");           // 标题里也有
    meta.tags = QStringLiteral("a, b");

    check(db.addNoteMeta(meta), "addNoteMeta: 含单引号 / %1 的值能正常插入");

    QList<NoteMeta> all = db.selectAllNotes();
    check(all.size() == 1, "selectAllNotes: 查到 1 条");
    if (all.size() == 1) {
        check(all[0].title == meta.title, "读回的 title 与写入完全一致", all[0].title);
        check(all[0].filePath == meta.filePath, "读回的 filePath 与写入完全一致");
        check(all[0].createTime > 0, "createTime 为 0 时自动填了当前时间");
        check(all[0].id > 0, "id 由数据库自增生成");
    }

    check(!db.addNoteMeta(meta), "addNoteMeta: 同一路径重复插入被 UNIQUE 拒绝");

    // ---------------- 更新 ----------------
    NoteMeta changed = meta;
    changed.title = QStringLiteral("改过的标题 'x'");
    check(db.updateNoteMeta(changed), "updateNoteMeta: 更新成功");
    all = db.selectAllNotes();
    check(all.size() == 1 && all[0].title == changed.title, "updateNoteMeta: 内容确实变了");

    NoteMeta ghost;
    ghost.filePath = work + QStringLiteral("/不存在的.md");
    ghost.title = QStringLiteral("x");
    check(!db.updateNoteMeta(ghost), "updateNoteMeta: 路径不存在 -> false");

    // ---------------- 删除 ----------------
    check(db.deleteNoteByPath(meta.filePath), "deleteNoteByPath: 删除成功");
    check(db.selectAllNotes().isEmpty(), "deleteNoteByPath: 表已空");
    check(!db.deleteNoteByPath(meta.filePath), "deleteNoteByPath: 同一个路径再删 -> false");

    // ---------------- 错误 SQL 不能崩 ----------------
    check(!db.execSQL(QStringLiteral("THIS IS NOT VALID SQL")), "execSQL: 非法 SQL -> false");
    check(db.querySQL(QStringLiteral("SELECT * FROM 不存在的表")).isEmpty(),
          "querySQL: 查不存在的表 -> 空列表");

    // ---------------- 关闭 / 重新打开（验证连接真的被释放）----------------
    db.closeDB();
    check(!db.isOpen(), "closeDB: isOpen = false");

    check(db.openDB(dbPath), "重新 openDB 成功（说明连接已正确释放）");
    NoteMeta second;
    second.filePath = work + QStringLiteral("/c.md");
    second.title = QStringLiteral("第二条");
    check(db.addNoteMeta(second), "重新打开后仍能写入");
    check(db.selectAllNotes().size() == 1, "重新打开后能读回数据（文件里的数据还在）");

    db.closeDB();

    // ---------------- 未打开时的行为：返回 false，不能崩 ----------------
    check(!db.execSQL(QStringLiteral("SELECT 1")), "未打开时 execSQL -> false");
    check(db.querySQL(QStringLiteral("SELECT 1")).isEmpty(), "未打开时 querySQL -> 空列表");
    check(db.selectAllNotes().isEmpty(), "未打开时 selectAllNotes -> 空列表");

    QDir(work).removeRecursively();

    std::printf("\nFAIL count = %d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
