#include "findreplacedialog.h"

#include "editorwidget.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

FindReplaceDialog::FindReplaceDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("查找与替换"));
    setModal(false);  // 非模态：查着改着是最常见的用法（理由见头文件）

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(QStringLiteral("查找内容"));
    m_searchEdit->setClearButtonEnabled(true);

    m_replaceEdit = new QLineEdit(this);
    m_replaceEdit->setPlaceholderText(QStringLiteral("替换为"));
    m_replaceEdit->setClearButtonEnabled(true);

    m_caseCheck = new QCheckBox(QStringLiteral("区分大小写"), this);

    m_findPrevButton = new QPushButton(QStringLiteral("上一个"), this);
    m_findNextButton = new QPushButton(QStringLiteral("下一个"), this);
    m_replaceButton = new QPushButton(QStringLiteral("替换"), this);
    m_replaceAllButton = new QPushButton(QStringLiteral("全部替换"), this);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("查找"), m_searchEdit);
    form->addRow(QStringLiteral("替换"), m_replaceEdit);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addWidget(m_findPrevButton);
    buttonRow->addWidget(m_findNextButton);
    buttonRow->addSpacing(12);
    buttonRow->addWidget(m_replaceButton);
    buttonRow->addWidget(m_replaceAllButton);
    buttonRow->addStretch(1);

    auto *closeRow = new QHBoxLayout;
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), this);
    closeRow->addStretch(1);
    closeRow->addWidget(closeButton);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_caseCheck);
    layout->addLayout(buttonRow);
    layout->addWidget(m_status);
    layout->addLayout(closeRow);

    // ---------------- 接线 ----------------
    // 回车 = 查找下一个（打字之后顺手回车是最自然的操作）
    connect(m_searchEdit, &QLineEdit::returnPressed, this, [this] { findNext(); });
    connect(m_replaceEdit, &QLineEdit::returnPressed, this, [this] { replaceOne(); });
    // 输入框内容一变就更新"共 N 处"
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this] { refreshStatus(); });
    connect(m_caseCheck, &QCheckBox::toggled, this, [this] { refreshStatus(); });

    connect(m_findNextButton, &QPushButton::clicked, this, [this] { findNext(); });
    connect(m_findPrevButton, &QPushButton::clicked, this, [this] { findPrevious(); });
    connect(m_replaceButton, &QPushButton::clicked, this, [this] { replaceOne(); });
    connect(m_replaceAllButton, &QPushButton::clicked, this, [this] { replaceAll(); });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);

    updateButtons();
    refreshStatus();
}

// ============================ 绑定与状态 ============================

void FindReplaceDialog::setEditor(EditorWidget *editor)
{
    m_editor = editor;
    updateButtons();
    refreshStatus();
}

EditorWidget *FindReplaceDialog::editor() const
{
    return m_editor;
}

QString FindReplaceDialog::searchText() const
{
    return m_searchEdit->text();
}

void FindReplaceDialog::setSearchText(const QString &text)
{
    m_searchEdit->setText(text);
}

QString FindReplaceDialog::replaceText() const
{
    return m_replaceEdit->text();
}

void FindReplaceDialog::setReplaceText(const QString &text)
{
    m_replaceEdit->setText(text);
}

bool FindReplaceDialog::caseSensitive() const
{
    return m_caseCheck->isChecked();
}

void FindReplaceDialog::setCaseSensitive(bool enabled)
{
    m_caseCheck->setChecked(enabled);
}

QString FindReplaceDialog::statusText() const
{
    return m_status->text();
}

void FindReplaceDialog::focusSearchField()
{
    m_searchEdit->setFocus();
    m_searchEdit->selectAll();  // 直接打字就能覆盖上一次的搜索词
}

// ============================ 机制 ============================

bool FindReplaceDialog::findNext()
{
    if (m_editor == nullptr) {
        return false;
    }
    const bool found = m_editor->findNext(m_searchEdit->text(), caseSensitive());
    refreshStatus(found ? QString() : QStringLiteral("没有找到"));
    return found;
}

bool FindReplaceDialog::findPrevious()
{
    if (m_editor == nullptr) {
        return false;
    }
    const bool found = m_editor->findPrevious(m_searchEdit->text(), caseSensitive());
    refreshStatus(found ? QString() : QStringLiteral("没有找到"));
    return found;
}

bool FindReplaceDialog::replaceOne()
{
    if (m_editor == nullptr) {
        return false;
    }

    // 先试"替换当前选中的那一处"；没选中/选中的不是它，就当成"替换下一个"：
    // 先跳过去再替换 —— 这样用户点一次"替换"就能一路替换下去。
    if (!m_editor->replaceCurrent(m_searchEdit->text(), m_replaceEdit->text(), caseSensitive())) {
        if (!m_editor->findNext(m_searchEdit->text(), caseSensitive())) {
            refreshStatus(QStringLiteral("没有找到"));
            return false;
        }
        m_editor->replaceCurrent(m_searchEdit->text(), m_replaceEdit->text(), caseSensitive());
    }

    refreshStatus();
    return true;
}

int FindReplaceDialog::replaceAll()
{
    if (m_editor == nullptr) {
        return 0;
    }

    const int count = m_editor->replaceAll(m_searchEdit->text(), m_replaceEdit->text(), caseSensitive());
    refreshStatus(count > 0 ? QStringLiteral("已替换 %1 处").arg(count) : QStringLiteral("没有找到"));
    return count;
}

// ============================ 内部 ============================

void FindReplaceDialog::refreshStatus(const QString &prefix)
{
    const QString text = m_searchEdit->text();
    if (m_editor == nullptr) {
        m_status->setText(QStringLiteral("没有打开的文档"));
        updateButtons();
        return;
    }
    if (text.isEmpty()) {
        m_status->setText(prefix.isEmpty() ? QStringLiteral("输入要查找的内容") : prefix);
        updateButtons();
        return;
    }

    const int total = m_editor->countOccurrences(text, caseSensitive());
    QString message = (total > 0) ? QStringLiteral("共 %1 处").arg(total) : QStringLiteral("没有找到");
    if (!prefix.isEmpty()) {
        message = prefix + QStringLiteral("（") + message + QStringLiteral("）");
    }
    m_status->setText(message);
    updateButtons();
}

void FindReplaceDialog::updateButtons()
{
    const bool usable = (m_editor != nullptr);
    m_findNextButton->setEnabled(usable);
    m_findPrevButton->setEnabled(usable);
    m_replaceButton->setEnabled(usable);
    m_replaceAllButton->setEnabled(usable);
    m_searchEdit->setEnabled(usable);
    m_replaceEdit->setEnabled(usable);
    m_caseCheck->setEnabled(usable);
}
