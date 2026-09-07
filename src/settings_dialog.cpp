#include "vpet/settings_dialog.h"

#include "vpet/agent/agent_runtime.h"
#include "vpet/settings_config.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace vpet
{

namespace
{

constexpr int MIN_TIMEOUT_MS = 1000;
constexpr int MAX_TIMEOUT_MS = 120000;
constexpr int TIMEOUT_SPIN_STEP_MS = 1000;

/**
 * @brief 构建只读路径提示标签（小号灰字，可选中复制）
 * @param[in] text 提示文本
 * @param[in] parent 父控件
 * @return 标签指针
 */
QLabel *CreatePathLabel(const QString &text, QWidget *parent)
{
    QLabel *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setStyleSheet(QStringLiteral("color: gray; font-size: 8pt;"));
    return label;
}

} // anonymous namespace

SettingsDialog::SettingsDialog(AgentRuntime *runtime, QWidget *parent)
    : QDialog(parent)
    , m_runtime(runtime)
    , m_baseUrlEdit(nullptr)
    , m_apiKeyEdit(nullptr)
    , m_modelEdit(nullptr)
    , m_timeoutSpin(nullptr)
    , m_memoryEnabledCheck(nullptr)
    , m_embeddingEnabledCheck(nullptr)
    , m_embeddingModelEdit(nullptr)
    , m_embeddingModelDirEdit(nullptr)
    , m_warningLabel(nullptr)
    , m_llmConfigPath()
    , m_memoryConfigPath()
{
    setWindowTitle(QStringLiteral("设置"));
    setModal(true);
    setMinimumWidth(460);

    if (m_runtime != nullptr)
    {
        m_llmConfigPath = m_runtime->ResolveLlmConfigPath();
        m_memoryConfigPath = m_runtime->ResolveMemoryConfigPath();
    }

    BuildUi();
}

void SettingsDialog::BuildUi()
{
    LlmSettingsFields llmFields;
    MemorySettingsFields memoryFields;
    QString llmLoadError;
    QString memoryLoadError;
    const bool llmLoaded = LoadLlmFields(m_llmConfigPath, llmFields, llmLoadError);
    const bool memoryLoaded = LoadMemoryFields(m_memoryConfigPath, memoryFields, memoryLoadError);

    m_warningLabel = new QLabel(this);
    m_warningLabel->setWordWrap(true);
    m_warningLabel->setStyleSheet(QStringLiteral("color: #b36b00;"));

    if (!llmLoaded || !memoryLoaded)
    {
        // 读取失败仍允许打开窗口查看，但明确警告保存会被拒绝，
        // 不会悄悄覆盖现有（非法）配置。
        QString warningText = QStringLiteral("现有配置读取失败，保存将被拒绝：\n");

        if (!llmLoaded)
        {
            warningText += llmLoadError;
        }

        if (!memoryLoaded)
        {
            if (!llmLoaded)
            {
                warningText += QStringLiteral("\n");
            }

            warningText += memoryLoadError;
        }

        m_warningLabel->setText(warningText);
        m_warningLabel->setVisible(true);
    }
    else
    {
        m_warningLabel->setVisible(false);
    }

    // ---- 语言模型页 ----
    m_baseUrlEdit = new QLineEdit(llmFields.baseUrl.trimmed(), this);
    m_baseUrlEdit->setPlaceholderText(QStringLiteral("https://api.example.com"));

    m_apiKeyEdit = new QLineEdit(llmFields.apiKey, this);
    m_apiKeyEdit->setEchoMode(QLineEdit::Password);

    // 明文/遮罩切换；密钥默认遮罩，仅本窗口内可见，不写入日志。
    QCheckBox *showKeyCheck = new QCheckBox(QStringLiteral("显示密钥"), this);
    connect(showKeyCheck, &QCheckBox::toggled, this, &SettingsDialog::ToggleApiKeyVisible);

    m_modelEdit = new QLineEdit(llmFields.model.trimmed(), this);
    m_modelEdit->setPlaceholderText(QStringLiteral("例如 deepseek-chat"));

    m_timeoutSpin = new QSpinBox(this);
    m_timeoutSpin->setRange(MIN_TIMEOUT_MS, MAX_TIMEOUT_MS);
    m_timeoutSpin->setSingleStep(TIMEOUT_SPIN_STEP_MS);
    m_timeoutSpin->setValue(qBound(MIN_TIMEOUT_MS, llmFields.timeoutMs, MAX_TIMEOUT_MS));
    m_timeoutSpin->setSuffix(QStringLiteral(" ms"));

    QWidget *llmPage = new QWidget(this);
    QFormLayout *llmForm = new QFormLayout(llmPage);
    llmForm->addRow(QStringLiteral("Base URL"), m_baseUrlEdit);

    QHBoxLayout *apiKeyRow = new QHBoxLayout();
    apiKeyRow->addWidget(m_apiKeyEdit, 1);
    apiKeyRow->addWidget(showKeyCheck);
    llmForm->addRow(QStringLiteral("API Key"), apiKeyRow);

    llmForm->addRow(QStringLiteral("模型名称"), m_modelEdit);
    llmForm->addRow(QStringLiteral("请求超时"), m_timeoutSpin);

    // ---- 长期记忆页 ----
    m_memoryEnabledCheck = new QCheckBox(QStringLiteral("启用长期记忆"), this);
    m_memoryEnabledCheck->setChecked(memoryFields.memoryEnabled);

    m_embeddingEnabledCheck = new QCheckBox(QStringLiteral("启用向量化检索"), this);
    m_embeddingEnabledCheck->setChecked(memoryFields.embeddingEnabled);

    m_embeddingModelEdit = new QLineEdit(memoryFields.embeddingModel.trimmed(), this);

    m_embeddingModelDirEdit = new QLineEdit(memoryFields.embeddingModelDir.trimmed(), this);
    m_embeddingModelDirEdit->setPlaceholderText(
        QStringLiteral("留空使用安装目录 models/embedding/<模型>"));

    QPushButton *browseButton = new QPushButton(QStringLiteral("浏览..."), this);
    connect(browseButton, &QPushButton::clicked,
            this, &SettingsDialog::BrowseEmbeddingModelDir);

    QHBoxLayout *modelDirRow = new QHBoxLayout();
    modelDirRow->addWidget(m_embeddingModelDirEdit);
    modelDirRow->addWidget(browseButton);

    QWidget *memoryPage = new QWidget(this);
    QVBoxLayout *memoryLayout = new QVBoxLayout(memoryPage);
    memoryLayout->addWidget(m_memoryEnabledCheck);

    QGroupBox *embeddingGroup =
        new QGroupBox(QStringLiteral("向量化（本地 ONNX 推理，不上传记忆文本）"), this);
    QFormLayout *embeddingForm = new QFormLayout(embeddingGroup);
    embeddingForm->addRow(m_embeddingEnabledCheck);
    embeddingForm->addRow(QStringLiteral("模型"), m_embeddingModelEdit);
    embeddingForm->addRow(QStringLiteral("模型目录"), modelDirRow);
    memoryLayout->addWidget(embeddingGroup);
    memoryLayout->addStretch(1);

    QTabWidget *tabWidget = new QTabWidget(this);
    tabWidget->addTab(llmPage, QStringLiteral("语言模型"));
    tabWidget->addTab(memoryPage, QStringLiteral("长期记忆"));

    QLabel *restartLabel = new QLabel(QStringLiteral("保存后需重启程序才能生效。"), this);
    restartLabel->setStyleSheet(QStringLiteral("color: #b36b00;"));

    QLabel *pathLabel = CreatePathLabel(
        QStringLiteral("配置文件：\n%1\n%2").arg(m_llmConfigPath, m_memoryConfigPath),
        this);

    QDialogButtonBox *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &SettingsDialog::SaveSettings);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(m_warningLabel);
    mainLayout->addWidget(tabWidget);
    mainLayout->addWidget(restartLabel);
    mainLayout->addWidget(pathLabel);
    mainLayout->addWidget(buttonBox);
}

void SettingsDialog::ToggleApiKeyVisible(bool visible)
{
    if (m_apiKeyEdit != nullptr)
    {
        m_apiKeyEdit->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
    }
}

void SettingsDialog::BrowseEmbeddingModelDir()
{
    if (m_embeddingModelDirEdit == nullptr)
    {
        return;
    }

    const QString selectedDir = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("选择 embedding 模型目录"),
        m_embeddingModelDirEdit->text().trimmed());

    if (!selectedDir.isEmpty())
    {
        m_embeddingModelDirEdit->setText(QDir::toNativeSeparators(selectedDir));
    }
}

void SettingsDialog::SaveSettings()
{
    LlmSettingsFields llmFields;
    llmFields.baseUrl = (m_baseUrlEdit != nullptr) ? m_baseUrlEdit->text() : QString();
    llmFields.apiKey = (m_apiKeyEdit != nullptr) ? m_apiKeyEdit->text() : QString();
    llmFields.model = (m_modelEdit != nullptr) ? m_modelEdit->text() : QString();
    llmFields.timeoutMs = (m_timeoutSpin != nullptr) ? m_timeoutSpin->value() : 30000;

    MemorySettingsFields memoryFields;
    memoryFields.memoryEnabled =
        (m_memoryEnabledCheck != nullptr) ? m_memoryEnabledCheck->isChecked() : true;
    memoryFields.embeddingEnabled =
        (m_embeddingEnabledCheck != nullptr) ? m_embeddingEnabledCheck->isChecked() : false;
    memoryFields.embeddingModel =
        (m_embeddingModelEdit != nullptr) ? m_embeddingModelEdit->text() : QString();
    memoryFields.embeddingModelDir =
        (m_embeddingModelDirEdit != nullptr) ? m_embeddingModelDirEdit->text().trimmed() : QString();

    QString llmError;
    QString memoryError;

    // 校验失败时不写任何文件，避免两份配置保存到一半。
    if (!ValidateLlmFields(llmFields, llmError)
        || !ValidateMemoryFields(memoryFields, memoryError))
    {
        QString message;

        if (!llmError.isEmpty())
        {
            message += QStringLiteral("语言模型：") + llmError;
        }

        if (!memoryError.isEmpty())
        {
            if (!message.isEmpty())
            {
                message += QStringLiteral("\n");
            }

            message += QStringLiteral("长期记忆：") + memoryError;
        }

        QMessageBox::warning(this, QStringLiteral("设置无效"), message);
        return;
    }

    const bool llmSaved = SaveLlmFields(m_llmConfigPath, llmFields, llmError);
    const bool memorySaved = SaveMemoryFields(m_memoryConfigPath, memoryFields, memoryError);

    if (!llmSaved || !memorySaved)
    {
        // 如实报告每一份文件的失败情况，不虚报成功。
        QString message;

        if (!llmSaved)
        {
            message += QStringLiteral("语言模型配置保存失败：") + llmError;
        }

        if (!memorySaved)
        {
            if (!message.isEmpty())
            {
                message += QStringLiteral("\n");
            }

            message += QStringLiteral("记忆配置保存失败：") + memoryError;
        }

        QMessageBox::critical(this, QStringLiteral("保存失败"), message);
        return;
    }

    QMessageBox::information(this,
                             QStringLiteral("设置已保存"),
                             QStringLiteral("设置已保存。\n重启程序后生效。"));
    accept();
}

} // namespace vpet
