#ifndef VPET_AGENT_TOOLS_TOOL_REGISTRY_H
#define VPET_AGENT_TOOLS_TOOL_REGISTRY_H

#include "vpet/agent/tools/itool.h"

#include <QJsonArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

namespace vpet
{

/**
 * @brief 工具注册表。
 *
 * 持有 ITool 实例并按名称查找；重名注册报错。未知工具名由循环转成
 * isError 结果回喂 LLM。BuildLlmToolsArray 生成 OpenAI tools 声明。
 */
class ToolRegistry
{
public:
    /**
     * @brief 注册一个工具。
     * @param[in] tool 工具实例；不得为空。
     * @param[out] errorMessage 错误描述。
     * @return 注册成功返回 true；工具为空或重名返回 false。
     */
    bool Register(const std::shared_ptr<ITool> &tool, QString &errorMessage);

    /**
     * @brief 按名称查找工具。
     * @param[in] toolName 工具名。
     * @return 工具实例；未注册返回空指针。
     */
    std::shared_ptr<ITool> Find(const QString &toolName) const;

    /**
     * @brief 判断工具是否已注册。
     * @param[in] toolName 工具名。
     * @return 已注册返回 true。
     */
    bool Contains(const QString &toolName) const;

    /**
     * @brief 返回已注册工具名列表（按注册顺序）。
     * @return 工具名列表。
     */
    QStringList ToolNames() const;

    /**
     * @brief 汇总全部工具规格。
     * @return 工具规格列表。
     */
    QVector<_tagToolSpec> Specs() const;

    /**
     * @brief 生成 OpenAI 兼容的 tools 声明数组，喂给 LLM。
     * @return tools JSON 数组（全部已注册工具）。
     */
    QJsonArray BuildLlmToolsArray() const;

    /**
     * @brief 生成 OpenAI 兼容的 tools 声明数组，仅包含指定工具。
     * @param[in] toolNames 允许的工具名；空列表表示全部已注册工具。
     * @return tools JSON 数组。
     */
    QJsonArray BuildLlmToolsArray(const QStringList &toolNames) const;

    /**
     * @brief 返回已注册工具数量。
     * @return 工具数量。
     */
    int Count() const;

    /**
     * @brief 清空注册表。
     */
    void Clear();

private:
    QVector<std::shared_ptr<ITool>> m_tools; ///< 按注册顺序持有的工具
};

} // namespace vpet

#endif // VPET_AGENT_TOOLS_TOOL_REGISTRY_H
