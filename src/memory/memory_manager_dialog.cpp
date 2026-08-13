#include "vpet/memory/memory_manager_dialog.h"

#include "vpet/agent/agent_runtime.h"
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

namespace vpet
{

namespace
{

/**
 * @brief 将记忆类型转换为用户可读文本
 * @param[in] type 记忆类型
 * @return 类型文本
 */
QString TypeText(MemoryEntry::Type type)
{
    switch (type)
    {
        case MemoryEntry::Type::Fact:
            return QStringLiteral("事实");
        case MemoryEntry::Type::Preference:
            return QStringLiteral("偏好");
        case MemoryEntry::Type::Procedure:
            return QStringLiteral("流程");
        case MemoryEntry::Type::Correction:
            return QStringLiteral("纠正");
        case MemoryEntry::Type::Negative:
            return QStringLiteral("避免");
    }

    return QStringLiteral("记忆");
}

/**
 * @brief 按逗号和空白解析标签
 * @param[in] text 标签文本
 * @return 去重后的标签
 */
QStringList ParseTags(const QString &text)
{
    QStringList tags;

    for (const QString &rawTag : text.split(QRegularExpression(QStringLiteral("[,，\\s]+")),
                                            Qt::SkipEmptyParts))
    {
        const QString tag = rawTag.trimmed();

        if (!tag.isEmpty() && !tags.contains(tag))
        {
            tags.append(tag);
        }
    }

    return tags;
}

} // anonymous namespace

MemoryManagerDialog::MemoryManagerDialog(AgentRuntime *runtime, QWidget *parent)
    : QDialog(parent)
    , m_runtime(runtime)
    , m_entryList(nullptr)
    , m_contentEdit(nullptr)
    , m_tagsEdit(nullptr)
    , m_saveButton(nullptr)
    , m_forgetButton(nullptr)
    , m_exportButton(nullptr)
    , m_importButton(nullptr)
    , m_helpfulButton(nullptr)
    , m_unhelpfulButton(nullptr)
    , m_pollTimer(nullptr)
    , m_statusLabel(nullptr)
    , m_entries()
{
    setWindowTitle(QStringLiteral("长期记忆"));
    resize(760, 480);
    BuildUi();
}

MemoryManagerDialog::~MemoryManagerDialog() = default;

void MemoryManagerDialog::ShowAndRefresh()
{
    show();
    raise();
    activateWindow();
    Refresh();
}

void MemoryManagerDialog::BuildUi()
{
    m_entryList = new QListWidget(this);
    m_entryList->setMinimumWidth(300);
    m_contentEdit = new QPlainTextEdit(this);
    m_tagsEdit = new QPlainTextEdit(this);
    m_tagsEdit->setMaximumHeight(64);

    auto *formLayout = new QFormLayout();
    formLayout->addRow(QStringLiteral("内容"), m_contentEdit);
    formLayout->addRow(QStringLiteral("标签"), m_tagsEdit);

    m_saveButton = new QPushButton(QStringLiteral("保存"), this);
    m_forgetButton = new QPushButton(QStringLiteral("删除"), this);
    m_helpfulButton = new QPushButton(QStringLiteral("有帮助"), this);
    m_unhelpfulButton = new QPushButton(QStringLiteral("无帮助"), this);
    m_exportButton = new QPushButton(QStringLiteral("导出"), this);
    m_importButton = new QPushButton(QStringLiteral("导入"), this);
    auto *refreshButton = new QPushButton(QStringLiteral("刷新"), this);

    auto *buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(m_saveButton);
    buttonLayout->addWidget(m_forgetButton);
    buttonLayout->addWidget(m_helpfulButton);
    buttonLayout->addWidget(m_unhelpfulButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(refreshButton);
    buttonLayout->addWidget(m_exportButton);
    buttonLayout->addWidget(m_importButton);

    auto *rightLayout = new QVBoxLayout();
    rightLayout->addWidget(new QLabel(QStringLiteral("选中记忆"), this));
    rightLayout->addLayout(formLayout);
    rightLayout->addLayout(buttonLayout);

    auto *rightWidget = new QWidget(this);
    rightWidget->setLayout(rightLayout);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_entryList);
    splitter->addWidget(rightWidget);
    splitter->setStretchFactor(1, 1);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(splitter);
    m_statusLabel = new QLabel(QStringLiteral("就绪"), this);
    mainLayout->addWidget(m_statusLabel);
    setLayout(mainLayout);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(100);

    connect(m_entryList, &QListWidget::itemClicked,
            this, &MemoryManagerDialog::LoadSelectedEntry);
    connect(refreshButton, &QPushButton::clicked, this, &MemoryManagerDialog::Refresh);
    connect(m_saveButton, &QPushButton::clicked, this, &MemoryManagerDialog::SaveSelectedEntry);
    connect(m_forgetButton, &QPushButton::clicked,
            this, &MemoryManagerDialog::ForgetSelectedEntry);
    connect(m_exportButton, &QPushButton::clicked, this, &MemoryManagerDialog::ExportMemory);
    connect(m_importButton, &QPushButton::clicked, this, &MemoryManagerDialog::ImportMemory);
    connect(m_helpfulButton, &QPushButton::clicked,
            this, &MemoryManagerDialog::SubmitHelpfulFeedback);
    connect(m_unhelpfulButton, &QPushButton::clicked,
            this, &MemoryManagerDialog::SubmitUnhelpfulFeedback);
    connect(m_pollTimer, &QTimer::timeout, this, &MemoryManagerDialog::PollReadyResult);
    connect(m_runtime, &AgentRuntime::LogMessage,
            this, &MemoryManagerDialog::OnRuntimeLogMessage);
}

void MemoryManagerDialog::Refresh()
{
    if (m_runtime == nullptr)
    {
        return;
    }

    quint64 requestId = 0;

    if (m_runtime->RequestMemoryList(requestId))
    {
        m_pollTimer->start();
    }
}

void MemoryManagerDialog::PollReadyResult()
{
    if (m_runtime == nullptr)
    {
        m_pollTimer->stop();
        return;
    }

    QVector<MemoryEntry> entries;

    if (!m_runtime->TakeMemoryListResult(entries))
    {
        return;
    }

    m_pollTimer->stop();
    m_entries = entries;
    m_entryList->clear();

    for (const MemoryEntry &entry : m_entries)
    {
        auto *item = new QListWidgetItem(EntryDisplayText(entry), m_entryList);
        item->setData(Qt::UserRole, entry.id);
    }

    if (!m_entries.isEmpty())
    {
        m_entryList->setCurrentRow(0);
        LoadSelectedEntry(m_entryList->item(0));
    }
}

void MemoryManagerDialog::LoadSelectedEntry(QListWidgetItem *item)
{
    if (item == nullptr)
    {
        return;
    }

    const QString memoryId = item->data(Qt::UserRole).toString();

    for (const MemoryEntry &entry : m_entries)
    {
        if (entry.id == memoryId)
        {
            m_contentEdit->setPlainText(entry.content);
            m_tagsEdit->setPlainText(entry.tags.join(QStringLiteral(", ")));
            return;
        }
    }
}

bool MemoryManagerDialog::GetSelectedEntry(MemoryEntry &entry) const
{
    const QListWidgetItem *item = m_entryList->currentItem();

    if (item == nullptr)
    {
        return false;
    }

    const QString memoryId = item->data(Qt::UserRole).toString();

    for (const MemoryEntry &candidate : m_entries)
    {
        if (candidate.id == memoryId)
        {
            entry = candidate;
            return true;
        }
    }

    return false;
}

void MemoryManagerDialog::SaveSelectedEntry()
{
    MemoryEntry entry;

    if ((m_runtime == nullptr) || !GetSelectedEntry(entry))
    {
        return;
    }

    entry.content = m_contentEdit->toPlainText().trimmed();
    entry.tags = ParseTags(m_tagsEdit->toPlainText());
    quint64 requestId = 0;
    QString errorMessage;

    if (!m_runtime->UpdateMemory(entry, requestId, errorMessage))
    {
        QMessageBox::warning(this, QStringLiteral("无法保存"), errorMessage);
        return;
    }

    Refresh();
}

void MemoryManagerDialog::ForgetSelectedEntry()
{
    MemoryEntry entry;

    if ((m_runtime == nullptr) || !GetSelectedEntry(entry))
    {
        return;
    }

    if (QMessageBox::question(this,
                              QStringLiteral("删除记忆"),
                              QStringLiteral("确定删除选中的记忆吗？")) != QMessageBox::Yes)
    {
        return;
    }

    quint64 requestId = 0;

    if (m_runtime->ForgetMemory(entry.id, requestId))
    {
        Refresh();
    }
}

void MemoryManagerDialog::ExportMemory()
{
    if (m_runtime == nullptr)
    {
        return;
    }

    const QString filePath = QFileDialog::getSaveFileName(this,
                                                          QStringLiteral("导出长期记忆"),
                                                          QString(),
                                                          QStringLiteral("JSON 文件 (*.json)"));

    if (filePath.isEmpty())
    {
        return;
    }

    quint64 requestId = 0;
    m_runtime->ExportMemory(filePath, requestId);
}

void MemoryManagerDialog::ImportMemory()
{
    if (m_runtime == nullptr)
    {
        return;
    }

    const QString filePath = QFileDialog::getOpenFileName(this,
                                                          QStringLiteral("导入长期记忆"),
                                                          QString(),
                                                          QStringLiteral("JSON 文件 (*.json)"));

    if (filePath.isEmpty())
    {
        return;
    }

    if (QMessageBox::question(this,
                              QStringLiteral("导入长期记忆"),
                              QStringLiteral("导入会替换当前记忆图，确定继续吗？"))
        != QMessageBox::Yes)
    {
        return;
    }

    quint64 requestId = 0;

    if (m_runtime->ImportMemory(filePath, requestId))
    {
        Refresh();
    }
}

void MemoryManagerDialog::SubmitHelpfulFeedback()
{
    MemoryEntry entry;

    if ((m_runtime == nullptr) || !GetSelectedEntry(entry))
    {
        return;
    }

    quint64 requestId = 0;
    m_runtime->SubmitMemoryFeedback({ entry.id }, true, requestId);
}

void MemoryManagerDialog::SubmitUnhelpfulFeedback()
{
    MemoryEntry entry;

    if ((m_runtime == nullptr) || !GetSelectedEntry(entry))
    {
        return;
    }

    quint64 requestId = 0;
    m_runtime->SubmitMemoryFeedback({ entry.id }, false, requestId);
}

void MemoryManagerDialog::OnRuntimeLogMessage(const QString &message)
{
    if ((m_statusLabel == nullptr) || !message.startsWith(QStringLiteral("Memory ")))
    {
        return;
    }

    m_statusLabel->setText(message);
}

QString MemoryManagerDialog::EntryDisplayText(const MemoryEntry &entry)
{
    const QString conflictMarker = entry.hasConflict ? QStringLiteral("[冲突] ") : QString();
    return QStringLiteral("%1[%2] %3").arg(conflictMarker,
                                             TypeText(entry.type),
                                             entry.content);
}

} // namespace vpet
