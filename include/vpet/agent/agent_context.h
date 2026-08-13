#ifndef VPET_AGENT_AGENT_CONTEXT_H
#define VPET_AGENT_AGENT_CONTEXT_H

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace vpet
{

/**
 * @brief Agent 运行时统一上下文对象
 *
 * 用于在 Agent 节点之间传递用户输入、节点输出和运行时状态。
 */
class AgentContext
{
public:
    /**
     * @brief 构造函数
     */
    AgentContext();

    /**
     * @brief 清空上下文数据
     */
    void Clear();

    /**
     * @brief 写入上下文值
     * @param[in] key 上下文键名
     * @param[in] value 上下文值
     * @return 写入成功返回 true
     */
    bool SetValue(const QString &key, const QVariant &value);

    /**
     * @brief 读取上下文值
     * @param[in] key 上下文键名
     * @param[out] value 上下文值
     * @return 读取成功返回 true
     */
    bool GetValue(const QString &key, QVariant &value) const;

    /**
     * @brief 判断上下文键是否存在
     * @param[in] key 上下文键名
     * @return 存在返回 true
     */
    bool Contains(const QString &key) const;

    /**
     * @brief 删除上下文值
     * @param[in] key 上下文键名
     * @return 删除成功返回 true
     */
    bool RemoveValue(const QString &key);

    /**
     * @brief 获取所有上下文键名
     * @return 上下文键名列表
     */
    QStringList GetKeys() const;

    /**
     * @brief 创建当前上下文的独立快照
     * @return 可独立读写的上下文副本
     */
    AgentContext Snapshot() const;

    /**
     * @brief 将另一个上下文中的键值覆盖写入当前上下文
     * @param[in] overlay 用于覆盖的上下文
     * @return 覆盖成功返回 true
     */
    bool Overlay(const AgentContext &overlay);

    /**
     * @brief 提取相对基础上下文的覆盖增量和删除键
     * @param[in] base 用于比较的基础上下文
     * @param[out] delta 当前上下文相对基础上下文的新增或变更键
     * @param[out] removedKeys 当前上下文相对基础上下文删除的键
     * @return 提取成功返回 true
     */
    bool BuildDelta(const AgentContext &base,
                    AgentContext &delta,
                    QSet<QString> &removedKeys) const;

    /**
     * @brief 设置用户输入文本
     * @param[in] userInput 用户输入文本
     * @return 设置成功返回 true
     */
    bool SetUserInput(const QString &userInput);

    /**
     * @brief 获取用户输入文本
     * @return 用户输入文本
     */
    QString GetUserInput() const;

    /**
     * @brief 记录已执行节点
     * @param[in] nodeName 节点名称
     * @return 记录成功返回 true
     */
    bool AppendExecutedNode(const QString &nodeName);

    /**
     * @brief 获取已执行节点列表
     * @return 已执行节点列表
     */
    QStringList GetExecutedNodes() const;

private:
    QHash<QString, QVariant> m_values; ///< 统一上下文键值表
};

} // namespace vpet

#endif // VPET_AGENT_AGENT_CONTEXT_H
