#ifndef VPET_SETTINGS_DIALOG_H
#define VPET_SETTINGS_DIALOG_H

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace vpet
{

class AgentRuntime;

/**
 * @brief 设置窗口（LLM 与长期记忆配置）
 *
 * 编辑 llm_config.json 与 memory_config.json 中面板管理的字段：
 * 文本 LLM 的 Base URL / API Key / 模型名称 / 超时，以及长期记忆
 * 开关与本地 ONNX 向量化配置（实际 schema 无远端 embedding API）。
 *
 * 保存时保留文件中未编辑的其余 JSON 字段；目标文件存在但 JSON
 * 非法时拒绝写入。所有更改在重启程序后生效。
 */
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    /**
     * @brief 构造设置窗口
     * @param[in] runtime Agent 运行时；不得为空
     * @param[in] parent 父窗口
     */
    explicit SettingsDialog(AgentRuntime *runtime, QWidget *parent = nullptr);

private slots:
    /**
     * @brief 校验并保存两项配置文件
     */
    void SaveSettings();

    /**
     * @brief 切换 API Key 输入框明文/遮罩显示
     * @param[in] visible 是否明文显示
     */
    void ToggleApiKeyVisible(bool visible);

    /**
     * @brief 浏览选择 embedding 模型目录
     */
    void BrowseEmbeddingModelDir();

private:
    /**
     * @brief 构建 UI 并载入当前配置
     */
    void BuildUi();

private:
    AgentRuntime *m_runtime;              ///< Agent 运行时，不持有所有权
    QLineEdit *m_baseUrlEdit;             ///< LLM Base URL
    QLineEdit *m_apiKeyEdit;              ///< LLM API Key（默认遮罩显示）
    QLineEdit *m_modelEdit;               ///< LLM 模型名称
    QSpinBox *m_timeoutSpin;              ///< LLM 超时（毫秒）
    QCheckBox *m_memoryEnabledCheck;      ///< 长期记忆开关
    QCheckBox *m_embeddingEnabledCheck;   ///< 向量化开关
    QLineEdit *m_embeddingModelEdit;      ///< embedding 模型标识
    QLineEdit *m_embeddingModelDirEdit;   ///< embedding 模型目录
    QLabel *m_warningLabel;               ///< 配置读取失败警告
    QString m_llmConfigPath;              ///< LLM 配置文件路径
    QString m_memoryConfigPath;           ///< 记忆配置文件路径
};

} // namespace vpet

#endif // VPET_SETTINGS_DIALOG_H
