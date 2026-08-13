#ifndef VPET_AGENT_AGENT_OUTPUT_POLICY_H
#define VPET_AGENT_AGENT_OUTPUT_POLICY_H

#include "vpet/agent/agent_context.h"

#include <QString>

namespace vpet
{

/**
 * @brief Agent 输出策略辅助模块。
 *
 * 负责记录主动发话的持久化节流状态，Runtime 负责调度和信号发送。
 */
class AgentOutputPolicy
{
public:
    /**
     * @brief 记录视觉主动发话已经成功产出
     * @param[in,out] context 当前 invocation 上下文
     * @param[out] errorMessage 错误描述
     * @return 记录成功返回 true
     */
    static bool RecordVisionSpeech(AgentContext &context, QString &errorMessage);
};

} // namespace vpet

#endif // VPET_AGENT_AGENT_OUTPUT_POLICY_H
