#ifndef VPET_AGENT_NATIVE_TOOLS_H
#define VPET_AGENT_NATIVE_TOOLS_H

#include "vpet/agent/tools/itool.h"
#include "vpet/web/web_search_tool.h"
#include "vpet/memory/memory_service.h"
#include "vpet/llm/vision_llm_client.h"
#include <QPointer>
#include <QTimer>
#include <functional>

namespace vpet {
// Native adapters are invocation-only: construction never captures or sends data.
class NativeTool final : public ITool {
public:
    enum class Kind { Web, Memory, Screen };
    NativeTool(Kind kind, MemoryService *memory, VisionLlmClient *vision,
               std::function<QString()> petId);
    ~NativeTool() override;
    _tagToolSpec Spec() const override;
    bool ValidateArguments(const QJsonObject &, QString &) const override;
    void Execute(const _tagToolCall &) override;
    void Cancel() override;
    bool IsBusy() const override { return m_busy; }
    bool LoadWebConfig(const QString &path, QString &error);
private:
    void Complete(bool ok, const QString &text);
    Kind m_kind;
    QPointer<MemoryService> m_memory;
    QPointer<VisionLlmClient> m_vision;
    std::function<QString()> m_petId;
    WebSearchTool m_web;
    QTimer m_poll;
    _tagToolCall m_call;
    bool m_busy = false;
    int m_networkId = -1;
    quint64 m_memoryId = 0;
    QString m_activePet;
};
}
#endif
