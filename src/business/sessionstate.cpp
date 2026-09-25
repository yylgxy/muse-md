#include "sessionstate.h"

#include "configmanager.h"
#include "logger.h"

#include <QFileInfo>

QString SessionState::geometryKey()
{
    return QStringLiteral("window/geometry");
}

QString SessionState::openFilesKey()
{
    return QStringLiteral("session/openFiles");
}

QString SessionState::currentIndexKey()
{
    return QStringLiteral("session/currentIndex");
}

QString SessionState::fileTreeVisibleKey()
{
    return QStringLiteral("session/fileTreeVisible");
}

QString SessionState::searchPanelVisibleKey()
{
    return QStringLiteral("session/searchPanelVisible");
}

QString SessionState::outlinePanelVisibleKey()
{
    return QStringLiteral("session/outlinePanelVisible");
}

QString SessionState::viewStateKey()
{
    return QStringLiteral("session/viewState");
}

// ---- C2：光标/滚动位置的编解码 ----
//
// 存储格式（一条一行，"|" 分隔三个数字）：
//
//     行|列|垂直滚动值            ← makeViewState() 产出的值
//     路径<TAB>行|列|垂直滚动值    ← encodeViewState() 产出的整条
//
// 为什么用 TAB 分隔路径和值：Windows 路径里不能含 TAB，所以不用转义。
// 万一真的遇到含 TAB 的路径（异常情况），encode 会把它整条丢掉 ——
// 宁可少记一个文件的位置，也不能写出一条读回来会串位的记录。

QString SessionState::makeViewState(int line, int column, int scrollValue)
{
    return QStringLiteral("%1|%2|%3")
        .arg(qMax(0, line))
        .arg(qMax(0, column))
        .arg(qMax(0, scrollValue));
}

bool SessionState::parseViewState(const QString &value, int *line, int *column, int *scrollValue)
{
    if (line == nullptr || column == nullptr || scrollValue == nullptr) {
        return false;
    }

    const QStringList fields = value.split(QLatin1Char('|'));
    if (fields.size() != 3) {
        return false;
    }

    bool okLine = false;
    bool okColumn = false;
    bool okScroll = false;
    const int parsedLine = fields.at(0).toInt(&okLine);
    const int parsedColumn = fields.at(1).toInt(&okColumn);
    const int parsedScroll = fields.at(2).toInt(&okScroll);
    if (!okLine || !okColumn || !okScroll) {
        return false;
    }

    // 越界的负数当 0：手改配置写坏了也不该让窗口去设一个非法的光标位置
    *line = qMax(0, parsedLine);
    *column = qMax(0, parsedColumn);
    *scrollValue = qMax(0, parsedScroll);
    return true;
}

void SessionState::setViewState(QHash<QString, QString> *state,
                                const QString &path,
                                int line,
                                int column,
                                int scrollValue)
{
    if (state == nullptr) {
        return;
    }
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return;  // 没保存过的新标签没有路径，不记（下次也开不出来，记了没意义）
    }
    state->insert(trimmed, makeViewState(line, column, scrollValue));
}

QStringList SessionState::encodeViewState(const QHash<QString, QString> &state)
{
    // ★ 先排序再输出：QHash 的遍历顺序是不确定的，不排的话每次存盘写出来的
    //   行序都不一样 —— 配置文件的 diff 会一直变，测试也没法断言具体内容。
    QStringList keys = state.keys();
    keys.sort(Qt::CaseInsensitive);

    QStringList lines;
    lines.reserve(keys.size());
    for (const QString &path : keys) {
        if (path.contains(QLatin1Char('\t')) || path.contains(QLatin1Char('\n'))) {
            continue;  // 分隔符出现在路径里 —— 丢掉这一条，不写出会串位的记录
        }
        const QString value = state.value(path);
        if (value.isEmpty()) {
            continue;
        }
        lines.append(path + QLatin1Char('\t') + value);
    }
    return lines;
}

QHash<QString, QString> SessionState::decodeViewState(const QStringList &lines)
{
    QHash<QString, QString> state;
    for (const QString &line : lines) {
        const int tab = line.indexOf(QLatin1Char('\t'));
        if (tab <= 0) {
            continue;  // 没有分隔符、或者路径为空 —— 坏行，跳过
        }
        const QString path = line.left(tab);
        const QString value = line.mid(tab + 1);

        int parsedLine = 0;
        int parsedColumn = 0;
        int parsedScroll = 0;
        if (!parseViewState(value, &parsedLine, &parsedColumn, &parsedScroll)) {
            continue;  // 数字坏了 —— 当作"这个文件没记过位置"，不影响别的条目
        }
        state.insert(path, value);
    }
    return state;
}

SessionState::Data SessionState::load()
{
    Data data;
    data.geometry = ConfigManager::value(geometryKey()).toByteArray();
    data.openFiles = usableFiles(ConfigManager::stringList(openFilesKey()));
    data.currentIndex = ConfigManager::value(currentIndexKey(), 0).toInt();
    data.fileTreeVisible = ConfigManager::value(fileTreeVisibleKey(), true).toBool();
    data.searchPanelVisible = ConfigManager::value(searchPanelVisibleKey(), false).toBool();
    data.outlinePanelVisible = ConfigManager::value(outlinePanelVisibleKey(), false).toBool();
    data.viewState = decodeViewState(ConfigManager::stringList(viewStateKey()));
    return data;
}

void SessionState::save(const Data &data)
{
    // 几何用 QByteArray 原样存：QSettings 会把它按 base64 写进 ini，
    // 既能手改（虽然没人会去改），也不会因为二进制内容把 ini 写坏。
    if (data.geometry.isEmpty()) {
        ConfigManager::remove(geometryKey());
    } else {
        ConfigManager::setValue(geometryKey(), data.geometry);
    }

    const QStringList files = usableFiles(data.openFiles);
    // setStringList 在遇到空列表时会把这个键删掉（ConfigManager 的约定），正合适
    ConfigManager::setStringList(openFilesKey(), files);
    ConfigManager::setValue(currentIndexKey(), qMax(0, data.currentIndex));
    ConfigManager::setValue(fileTreeVisibleKey(), data.fileTreeVisible);
    ConfigManager::setValue(searchPanelVisibleKey(), data.searchPanelVisible);
    ConfigManager::setValue(outlinePanelVisibleKey(), data.outlinePanelVisible);
    // C2：空表会被 setStringList 顺手删掉这个键（同 openFiles 的约定）
    ConfigManager::setStringList(viewStateKey(), encodeViewState(data.viewState));

    // 关窗口时进程马上就要退出，不能指望"析构时自动落盘"：显式同步一次
    ConfigManager::sync();
    LOG_INFO("会话状态已保存：%1 个文件，当前第 %2 个，%3 条光标位置",
             files.size(),
             data.currentIndex,
             data.viewState.size());
}

QStringList SessionState::usableFiles(const QStringList &paths)
{
    QStringList result;
    for (const QString &path : paths) {
        const QString trimmed = path.trimmed();
        if (trimmed.isEmpty()) {
            continue;  // 空路径（比如没保存过的新标签）不记
        }
        // 去重：同一个文件只留一次（大小写不敏感 —— Windows 上 D:\A.md 和 d:/a.md 是同一个）
        bool duplicated = false;
        for (const QString &existing : result) {
            if (QString::compare(QFileInfo(existing).absoluteFilePath(),
                                 QFileInfo(trimmed).absoluteFilePath(),
                                 Qt::CaseInsensitive)
                == 0) {
                duplicated = true;
                break;
            }
        }
        if (!duplicated) {
            result.append(trimmed);
        }
    }
    return result;
}
