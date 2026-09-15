#include "thememanager.h"

#include "configmanager.h"
#include "logger.h"

#include <QApplication>
#include <QFile>
#include <QStyle>
#include <QWidget>

using markdown_editor::core::document::ThemePalette;

namespace {

// 资源路径：两份 QSS 都在 qrc 里（resources/styles/），打包进 exe，不依赖磁盘文件
QString styleSheetResourcePath(ThemeManager::Theme theme)
{
    return (theme == ThemeManager::Theme::Dark) ? QStringLiteral(":/styles/dark.qss")
                                                : QStringLiteral(":/styles/light.qss");
}

// 暗色主题的调色板。QSS 管不到的地方（原生对话框、禁用态文字、交替行底色…）靠它兜住，
// 否则切到暗色后会零星留下几块系统白底 —— 那种"没切干净"比不切还难看。
QPalette darkPalette()
{
    QPalette palette;
    const QColor window(0x0d, 0x11, 0x17);
    const QColor base(0x0d, 0x11, 0x17);
    const QColor alternate(0x16, 0x1b, 0x22);
    const QColor text(0xc9, 0xd1, 0xd9);
    const QColor button(0x21, 0x26, 0x2d);
    const QColor highlight(0x1f, 0x6f, 0xeb);
    const QColor disabled(0x6e, 0x76, 0x81);

    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, alternate);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, button);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, QColor(0xff, 0x7b, 0x72));
    palette.setColor(QPalette::Highlight, highlight);
    palette.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    palette.setColor(QPalette::ToolTipBase, alternate);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::PlaceholderText, disabled);
    palette.setColor(QPalette::Link, QColor(0x58, 0xa6, 0xff));
    palette.setColor(QPalette::LinkVisited, QColor(0xd2, 0xa8, 0xff));

    palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);

    return palette;
}

}  // namespace

ThemeManager::ThemeManager(QObject *parent) : QObject(parent)
{
    // 记住系统原本的调色板：切回亮色时要还原成它（而不是我们自己拼一套"亮色"，
    // 那样在 Windows 上会丢掉系统的字体/间隔等细节）。
    m_lightPalette = QApplication::style()->standardPalette();
}

ThemeManager::~ThemeManager() = default;

ThemeManager &ThemeManager::instance()
{
    // 函数内静态局部变量：C++11 起初始化是线程安全的，不需要自己加锁
    static ThemeManager manager;
    return manager;
}

ThemeManager::Theme ThemeManager::theme() const
{
    return m_theme;
}

QString ThemeManager::themeId(Theme theme)
{
    return (theme == Theme::Dark) ? QStringLiteral("dark") : QStringLiteral("light");
}

ThemeManager::Theme ThemeManager::themeFromId(const QString &id)
{
    // 认不出来就当亮色：配置被手改坏了、或者将来加了第三种主题时，
    // "退化成亮色"总比"启动就崩"或者"一片什么都看不清"好。
    return (id.trimmed().compare(QLatin1String("dark"), Qt::CaseInsensitive) == 0) ? Theme::Dark : Theme::Light;
}

QString ThemeManager::styleSheetFor(Theme theme)
{
    const QString path = styleSheetResourcePath(theme);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        LOG_WARN("读不到主题样式表（%1），界面会退化成系统默认外观。"
                 "检查 resources/resources.qrc 里有没有把 styles/ 下的两个 qss 加进去", path);
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

ThemePalette ThemeManager::editorPalette(Theme theme)
{
    return (theme == Theme::Dark) ? ThemePalette::dark() : ThemePalette::light();
}

QString ThemeManager::storageKey()
{
    return QStringLiteral("theme");
}

void ThemeManager::setTheme(Theme theme)
{
    if (m_applied && theme == m_theme) {
        // 已经应用过、而且主题没变：**什么都不做**。
        // 这条不是省事：重复应用会触发整棵控件树的重新 polish，表现就是"点一下闪一下"。
        return;
    }

    m_theme = theme;
    applyToApplication(theme);
    m_applied = true;

    // 落盘：下次启动还是这个主题。写配置失败不影响本次切换（只记日志）。
    ConfigManager::setValue(storageKey(), themeId(theme));

    emit themeChanged(theme);
    LOG_INFO("主题已切换: %1", themeId(theme));
}

void ThemeManager::applySavedTheme()
{
    const Theme saved = themeFromId(ConfigManager::value(storageKey()).toString());
    // 即使和当前默认值相同也要走一遍 setTheme()：m_applied 还是 false 时会真的应用，
    // 否则启动时用的就是系统配色（而不是我们的亮色主题）。
    setTheme(saved);
}

void ThemeManager::applyToApplication(Theme theme)
{
    // 1) 调色板：先设它，再设样式表。顺序反过来的话，QSS 应用过程中控件可能
    //    先用旧调色板重绘一次（能看出来的闪一下）。
    if (theme == Theme::Dark) {
        QApplication::setPalette(darkPalette());
    } else {
        QApplication::setPalette(m_lightPalette);
    }

    // 2) 样式表：一次设给 QApplication，整棵控件树一次换完 ——
    //    逐个控件 setStyleSheet 才会出现"这块换了那块还没换"的中间态。
    //    注意：setStyleSheet() 不是静态函数（styleSheet() 才是），得通过 qApp 调。
    qApp->setStyleSheet(styleSheetFor(theme));
}
