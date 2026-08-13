#ifndef VPET_MEMORY_MEMORY_CONSOLIDATOR_H
#define VPET_MEMORY_MEMORY_CONSOLIDATOR_H

#include "vpet/memory/memory_graph.h"

#include <QHash>
#include <QPointer>
#include <QString>
#include <QVector>

namespace vpet
{

class LlmClient;
class MemoryService;

/**
 * @brief 非阻塞 LLM 记忆巩固协调器
 *
 * 将单轮增量送至伴生 LLM，严格解析其 JSON 输出，并将通过校验的候选交给
 * MemoryService。该类只在主线程维护请求关联，不参与 AgentRuntime 的 DAG 等待状态。
 */
class MemoryConsolidator
{
public:
    /**
     * @brief 提交单轮增量给伴生 LLM
     * @param[in] client 已配置的文本 LLM 客户端
     * @param[in] service 正在运行的记忆服务
     * @param[in] userInput 当前用户输入
     * @param[in] assistantOutput 当前最终回答
     * @param[in] petId 当前宠物 ID
     * @param[in] knownEntries 本轮已注入的可引用记忆
     * @param[in] maxCandidates 候选数量上限
     * @return 请求成功发出返回 true；不满足安全条件或发送失败返回 false
     */
    bool SubmitTurn(LlmClient &client,
                    MemoryService &service,
                    const QString &userInput,
                    const QString &assistantOutput,
                    const QString &petId,
                    const QVector<MemoryEntry> &knownEntries,
                    int maxCandidates);

    /**
     * @brief 处理伴生 LLM 成功响应
     * @param[in] requestId LLM 请求 ID
     * @param[in] response LLM 原始响应
     * @return 此 ID 属于巩固请求时返回 true
     */
    bool HandleLlmCompleted(int requestId, const QString &response);

    /**
     * @brief 丢弃伴生 LLM 失败请求
     * @param[in] requestId LLM 请求 ID
     * @return 此 ID 属于巩固请求时返回 true
     */
    bool HandleLlmFailed(int requestId);

    /**
     * @brief 严格解析并校验 LLM 巩固 JSON
     * @param[in] response LLM 原始响应（必须为 JSON 对象）
     * @param[in] petId 当前宠物 ID
     * @param[in] knownEntryIds 可以引用的既有记忆 ID
     * @param[in] maxCandidates 最大候选数量
     * @param[out] candidates 校验后的候选
     * @param[out] errorCategory 失败类别，不包含模型原文
     * @return 全部候选有效返回 true
     */
    static bool ParseCandidates(const QString &response,
                                const QString &petId,
                                const QVector<QString> &knownEntryIds,
                                int maxCandidates,
                                QVector<_tagMemoryConsolidationCandidate> &candidates,
                                QString &errorCategory);

private:
    /**
     * @brief 单个未完成巩固请求的关联数据
     */
    struct _tagPendingConsolidation
    {
        QPointer<MemoryService> service;   ///< 请求期间由 AgentRuntime 持有
        QString petId;                     ///< 当前宠物 ID
        QString triggerType;               ///< 固定为 user
        QVector<QString> knownEntryIds;    ///< 可被模型引用的记忆 ID
        int maxCandidates = 0;             ///< 输出候选上限
    };

    QHash<int, _tagPendingConsolidation> m_pendingRequests; ///< LLM 请求 ID -> 请求上下文
};

} // namespace vpet

#endif // VPET_MEMORY_MEMORY_CONSOLIDATOR_H
