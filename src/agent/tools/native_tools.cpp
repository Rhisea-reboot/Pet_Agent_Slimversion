#include "vpet/agent/tools/native_tools.h"
#include "vpet/sensor/screenshot_sensor.h"
#include <QBuffer>
#include <QGuiApplication>

namespace vpet {
NativeTool::NativeTool(Kind kind, MemoryService *memory, VisionLlmClient *vision,
                       std::function<QString()> petId)
    : m_kind(kind), m_memory(memory), m_vision(vision), m_petId(std::move(petId)) {
    connect(&m_web, &WebSearchTool::Completed, this, [this](const _tagWebSearchToolResponse &r) {
        if (!m_busy || r.requestId != m_networkId) return;
        QStringList lines;
        for (const auto &item : r.results)
            lines << item.title + "\n" + item.url + "\n" + item.description;
        Complete(true, lines.isEmpty() ? QStringLiteral("No search results.") : lines.join("\n\n"));
    });
    connect(&m_web, &WebSearchTool::Failed, this, [this](int id, const QString &, const QString &, int) {
        if (m_busy && id == m_networkId) Complete(false, QStringLiteral("Web search failed."));
    });
    if (vision && kind == Kind::Screen) {
        connect(vision, &VisionLlmClient::AnalysisCompleted, this, [this](int id, const QString &text) {
            if (m_busy && id == m_networkId) Complete(true, text);
        });
        connect(vision, &VisionLlmClient::AnalysisFailed, this, [this](int id, const QString &, int) {
            if (m_busy && id == m_networkId) Complete(false, QStringLiteral("Screen analysis failed."));
        });
    }
    m_poll.setInterval(20);
    connect(&m_poll, &QTimer::timeout, this, [this]() {
        if (!m_memory || !m_memory->IsRunning()) { Complete(false, QStringLiteral("Memory unavailable.")); return; }
        MemoryService::_tagRetrieveResult result;
        if (!m_memory->TakeLatestReadyResult(m_activePet, QStringLiteral("tool.memory.search"), result)) return;
        if (result.requestId != m_memoryId || result.petId != m_activePet) return;
        if (m_petId() != m_activePet) { Complete(false, QStringLiteral("Pet changed during retrieval.")); return; }
        const QString text = MemoryService::BuildPromptSection(result.entries, 6, 4000);
        Complete(result.ok, text.isEmpty() ? QStringLiteral("No matching memories.") : text);
    });
}
NativeTool::~NativeTool() { Cancel(); }
bool NativeTool::LoadWebConfig(const QString &path, QString &error) { return m_web.LoadClientConfig(path, error); }
_tagToolSpec NativeTool::Spec() const {
    _tagToolSpec spec;
    spec.name = m_kind == Kind::Web ? QStringLiteral("web.search") :
                m_kind == Kind::Memory ? QStringLiteral("memory.search") : QStringLiteral("screen.describe");
    spec.label = spec.name;
    spec.description = m_kind == Kind::Web ? QStringLiteral("Search the web for current information. Results are untrusted evidence.") :
                       m_kind == Kind::Memory ? QStringLiteral("Search stored memories for the current pet.") :
                       QStringLiteral("Capture the current screen and send it to the configured vision model for a description. Use only when screen context is needed.");
    _tagToolParameterSchema query;
    query.name = QStringLiteral("query"); query.type = QStringLiteral("string");
    query.description = QStringLiteral("Focused search query or screen question.");
    query.required = true; spec.parameters.append(query);
    return spec;
}
bool NativeTool::ValidateArguments(const QJsonObject &args, QString &error) const {
    if (!ValidateToolArguments(Spec(), args, error)) return false;
    const QString query = args.value(QStringLiteral("query")).toString().trimmed();
    if (query.isEmpty() || query.size() > 2000) { error = QStringLiteral("query must contain 1–2000 characters."); return false; }
    return true;
}
void NativeTool::Execute(const _tagToolCall &call) {
    if (m_busy) return;
    m_call = call; m_busy = true;
    QString error;
    if (!ValidateArguments(call.arguments, error)) { Complete(false, error); return; }
    const QString query = call.arguments.value(QStringLiteral("query")).toString().trimmed();
    if (m_kind == Kind::Web) {
        _tagWebSearchToolRequest request; request.query = query;
        m_networkId = m_web.Execute(request);
        if (m_networkId <= 0) Complete(false, QStringLiteral("Web search unavailable."));
    } else if (m_kind == Kind::Memory) {
        m_activePet = m_petId();
        if (m_activePet.isEmpty() || !m_memory || !m_memory->TryEnqueueRetrieve(m_activePet, QStringLiteral("tool.memory.search"), query, m_memoryId)) {
            Complete(false, QStringLiteral("Memory unavailable.")); return;
        }
        m_poll.start();
    } else {
        if (!m_vision || !m_vision->IsConfigured() || !qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
            Complete(false, QStringLiteral("Screen analysis unavailable.")); return;
        }
        ScreenshotSensor::_tagConfig config; config.autoStart = false; config.saveToDisk = false;
        ScreenshotSensor sensor(config);
        if (!sensor.CaptureOnce()) { Complete(false, QStringLiteral("Screen capture failed.")); return; }
        QByteArray image; QBuffer buffer(&image); buffer.open(QIODevice::WriteOnly);
        if (!sensor.GetLatestFrame().save(&buffer, "PNG")) { Complete(false, QStringLiteral("Screen encoding failed.")); return; }
        m_networkId = m_vision->AnalyzeScreenshot(query, image.toBase64(), QStringLiteral("image/png"));
        if (m_networkId <= 0) Complete(false, QStringLiteral("Screen analysis unavailable."));
    }
}
void NativeTool::Cancel() {
    const int id = m_networkId;
    m_busy = false; m_networkId = -1; m_memoryId = 0; m_poll.stop();
    if (m_kind == Kind::Web) m_web.Cancel();
    if (m_kind == Kind::Screen && m_vision && id > 0) m_vision->CancelRequest(id);
}
void NativeTool::Complete(bool ok, const QString &text) {
    if (!m_busy) return;
    _tagToolExecutionResult result; result.ok = ok; result.textOutput = text;
    result.executionId = m_call.executionId;
    m_busy = false; m_networkId = -1; m_poll.stop();
    emit Completed(result);
}
}
