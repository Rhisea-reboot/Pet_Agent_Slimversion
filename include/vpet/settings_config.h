#ifndef VPET_SETTINGS_CONFIG_H
#define VPET_SETTINGS_CONFIG_H

#include <QString>

namespace vpet
{

/**
 * @brief 文本 LLM 可编辑配置字段（llm_config.json）
 *
 * 仅覆盖设置面板管理的字段；文件中的其余字段在保存时原样保留。
 */
struct LlmSettingsFields
{
    QString baseUrl;                 ///< OpenAI 兼容 API 根地址
    QString apiKey;                  ///< API Key
    QString model;                   ///< 模型 ID
    int timeoutMs = 30000;           ///< HTTP 超时时间，单位毫秒
};

/**
 * @brief 长期记忆可编辑配置字段（memory_config.json）
 *
 * embedding 仅支持本地 ONNX 后端（实际 schema 无远端 API 字段）；
 * 文件中的 maintenance / embedding 其余字段在保存时原样保留。
 */
struct MemorySettingsFields
{
    bool memoryEnabled = true;       ///< 记忆功能总开关
    bool embeddingEnabled = false;   ///< 向量化开关
    QString embeddingModel = QStringLiteral("BAAI/bge-small-zh-v1.5"); ///< embedding 模型标识
    QString embeddingModelDir;       ///< embedding 模型目录；为空时使用安装目录默认位置
};

/**
 * @brief 校验 LLM 配置字段
 * @param[in] fields 待校验字段
 * @param[out] errorMessage 校验失败描述
 * @return 校验通过返回 true
 *
 * 规则与 LlmClient::NormalizeConfig 保持一致：Base URL 需含协议与主机名，
 * API Key 与模型名称不能为空，超时范围 1000 到 120000 毫秒。
 */
bool ValidateLlmFields(const LlmSettingsFields &fields, QString &errorMessage);

/**
 * @brief 校验记忆配置字段
 * @param[in] fields 待校验字段
 * @param[out] errorMessage 校验失败描述
 * @return 校验通过返回 true
 *
 * 仅在启用向量化时要求模型名称非空（与 MemoryService::ParseConfig 一致）。
 */
bool ValidateMemoryFields(const MemorySettingsFields &fields, QString &errorMessage);

/**
 * @brief 从配置文件读取 LLM 可编辑字段
 * @param[in] configPath 配置文件路径
 * @param[out] fields 输出字段；文件缺失时为默认值
 * @param[out] errorMessage 读取失败描述
 * @return 文件缺失或读取成功返回 true；文件存在但 JSON 非法返回 false
 */
bool LoadLlmFields(const QString &configPath,
                   LlmSettingsFields &fields,
                   QString &errorMessage);

/**
 * @brief 从配置文件读取记忆可编辑字段
 * @param[in] configPath 配置文件路径
 * @param[out] fields 输出字段；文件缺失时为默认值
 * @param[out] errorMessage 读取失败描述
 * @return 文件缺失或读取成功返回 true；文件存在但 JSON 非法返回 false
 */
bool LoadMemoryFields(const QString &configPath,
                      MemorySettingsFields &fields,
                      QString &errorMessage);

/**
 * @brief 将 LLM 字段写回配置文件（保留未编辑字段）
 * @param[in] configPath 配置文件路径
 * @param[in] fields 待写入字段
 * @param[out] errorMessage 写入失败描述
 * @return 保存成功返回 true
 *
 * 文件缺失时按默认结构创建；文件存在但 JSON 非法时拒绝写入，
 * 避免悄悄覆盖用户手写内容。写入使用 QSaveFile 原子替换。
 */
bool SaveLlmFields(const QString &configPath,
                   const LlmSettingsFields &fields,
                   QString &errorMessage);

/**
 * @brief 将记忆字段写回配置文件（保留未编辑字段）
 * @param[in] configPath 配置文件路径
 * @param[in] fields 待写入字段
 * @param[out] errorMessage 写入失败描述
 * @return 保存成功返回 true
 */
bool SaveMemoryFields(const QString &configPath,
                      const MemorySettingsFields &fields,
                      QString &errorMessage);

} // namespace vpet

#endif // VPET_SETTINGS_CONFIG_H
