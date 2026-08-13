#ifndef VPET_AGENT_RUNTIME_INTERNAL_H
#define VPET_AGENT_RUNTIME_INTERNAL_H

#include "vpet/agent/agent_context_keys.h"

#include <QString>

namespace vpet
{

namespace AgentRuntimeInternal
{

inline const QString LLM_CONFIG_FILE_NAME = QStringLiteral("llm_config.json");
inline const QString VISION_LLM_CONFIG_FILE_NAME = QStringLiteral("vision_llm_config.json");
inline const QString WEB_SEARCH_CONFIG_FILE_NAME = QStringLiteral("web_search_config.json");
inline const QString MEMORY_CONFIG_FILE_NAME = QStringLiteral("memory_config.json");

inline const QString &NODE_TYPE_VISION_INPUT = AgentContextKeys::NODE_TYPE_VISION_INPUT;
inline const QString &NODE_TYPE_VISION_LLM = AgentContextKeys::NODE_TYPE_VISION_LLM;
inline const QString &NODE_TYPE_LLM_CHAT = AgentContextKeys::NODE_TYPE_LLM_CHAT;
inline const QString &NODE_TYPE_EMOTION_REWRITE = AgentContextKeys::NODE_TYPE_EMOTION_REWRITE;
inline const QString &NODE_TYPE_OUTPUT_FORMAT = AgentContextKeys::NODE_TYPE_OUTPUT_FORMAT;
inline const QString &NODE_TYPE_PROACTIVE_TOPIC = AgentContextKeys::NODE_TYPE_PROACTIVE_TOPIC;
inline const QString &NODE_TYPE_USER_INPUT = AgentContextKeys::NODE_TYPE_USER_INPUT;
inline const QString &NODE_TYPE_WEB_RESEARCH = AgentContextKeys::NODE_TYPE_WEB_RESEARCH;
inline const QString &NODE_TYPE_MEMORY_RETRIEVE = AgentContextKeys::NODE_TYPE_MEMORY_RETRIEVE;
inline const QString &NODE_TYPE_MEMORY_STORE = AgentContextKeys::NODE_TYPE_MEMORY_STORE;

inline const QString DEFAULT_PET_ID = QStringLiteral("default");

inline const QString &CONTEXT_KEY_CONVERSATION_HISTORY = AgentContextKeys::CONVERSATION_HISTORY;
inline const QString &CONTEXT_KEY_EMOTION_OUTPUT_TEXT = AgentContextKeys::EMOTION_OUTPUT_TEXT;
inline const QString &CONTEXT_KEY_INPUT_AVAILABLE = AgentContextKeys::INPUT_AVAILABLE;
inline const QString &CONTEXT_KEY_NODE_INPUT_PROMPT = AgentContextKeys::NODE_INPUT_PROMPT;
inline const QString &CONTEXT_KEY_NODE_INPUT_TEXT_RESPONSE = AgentContextKeys::NODE_INPUT_TEXT_RESPONSE;
inline const QString &CONTEXT_KEY_NODE_OUTPUT_TEXT_FINAL = AgentContextKeys::NODE_OUTPUT_TEXT_FINAL;
inline const QString &CONTEXT_KEY_NODE_OUTPUT_TEXT_RESPONSE = AgentContextKeys::NODE_OUTPUT_TEXT_RESPONSE;
inline const QString &CONTEXT_KEY_VISION_INPUT_READY = AgentContextKeys::VISION_INPUT_READY;
inline const QString &CONTEXT_KEY_PROMPT_TEXT = AgentContextKeys::PROMPT_TEXT;
inline const QString &CONTEXT_KEY_SEMANTIC_OUTPUT_SOURCE = AgentContextKeys::SEMANTIC_OUTPUT_SOURCE;
inline const QString &CONTEXT_KEY_SEMANTIC_TEXT_FINAL = AgentContextKeys::SEMANTIC_TEXT_FINAL;
inline const QString &CONTEXT_KEY_SEMANTIC_TEXT_PROMPT = AgentContextKeys::SEMANTIC_TEXT_PROMPT;
inline const QString &CONTEXT_KEY_SEMANTIC_TEXT_RESPONSE = AgentContextKeys::SEMANTIC_TEXT_RESPONSE;
inline const QString &CONTEXT_KEY_SEMANTIC_VISION_SUMMARY = AgentContextKeys::SEMANTIC_VISION_SUMMARY;
inline const QString &CONTEXT_KEY_MEMORY_STORE_INTENT = AgentContextKeys::MEMORY_STORE_INTENT;
inline const QString &CONTEXT_KEY_SEMANTIC_MEMORY_ENTRIES = AgentContextKeys::SEMANTIC_MEMORY_ENTRIES;
inline const QString &CONTEXT_KEY_SEMANTIC_MEMORY_PROMPT = AgentContextKeys::SEMANTIC_MEMORY_PROMPT;
inline const QString &CONTEXT_KEY_PET_ID = AgentContextKeys::PET_ID;
inline const QString &CONTEXT_KEY_LLM_LAST_REQUEST_ID = AgentContextKeys::LLM_LAST_REQUEST_ID;
inline const QString &CONTEXT_KEY_LLM_PENDING = AgentContextKeys::LLM_PENDING;
inline const QString &CONTEXT_KEY_OUTPUT_PENDING = AgentContextKeys::OUTPUT_PENDING;
inline const QString &CONTEXT_KEY_OUTPUT_TEXT = AgentContextKeys::OUTPUT_TEXT;
inline const QString &CONTEXT_KEY_RUNTIME_PENDING = AgentContextKeys::RUNTIME_PENDING;
inline const QString &CONTEXT_KEY_RUNTIME_PENDING_NODE_ID = AgentContextKeys::RUNTIME_PENDING_NODE_ID;
inline const QString &CONTEXT_KEY_RUNTIME_PENDING_NODE_TYPE = AgentContextKeys::RUNTIME_PENDING_NODE_TYPE;
inline const QString &CONTEXT_KEY_RUNTIME_PENDING_REQUEST_ID = AgentContextKeys::RUNTIME_PENDING_REQUEST_ID;
inline const QString &CONTEXT_KEY_RUNTIME_TRIGGER_TYPE = AgentContextKeys::RUNTIME_TRIGGER_TYPE;
inline const QString &CONTEXT_KEY_RUNTIME_PASS_THROUGH_PREFIX = AgentContextKeys::RUNTIME_PASS_THROUGH_PREFIX;
inline const QString &CONTEXT_KEY_SEMANTIC_IMAGE_BASE64 = AgentContextKeys::SEMANTIC_IMAGE_BASE64;
inline const QString &CONTEXT_KEY_SEMANTIC_IMAGE_HEIGHT = AgentContextKeys::SEMANTIC_IMAGE_HEIGHT;
inline const QString &CONTEXT_KEY_SEMANTIC_IMAGE_MEDIA_TYPE = AgentContextKeys::SEMANTIC_IMAGE_MEDIA_TYPE;
inline const QString &CONTEXT_KEY_SEMANTIC_IMAGE_WIDTH = AgentContextKeys::SEMANTIC_IMAGE_WIDTH;
inline const QString &CONTEXT_KEY_SEMANTIC_VISION_FRAME_ID = AgentContextKeys::SEMANTIC_VISION_FRAME_ID;
inline const QString &CONTEXT_KEY_SEMANTIC_VISION_STATE = AgentContextKeys::SEMANTIC_VISION_STATE;
inline const QString &CONTEXT_KEY_VISION_ANALYSIS = AgentContextKeys::VISION_ANALYSIS;
inline const QString &CONTEXT_KEY_VISION_AVAILABLE = AgentContextKeys::VISION_AVAILABLE;
inline const QString &CONTEXT_KEY_VISION_LATEST_BASE64 = AgentContextKeys::VISION_LATEST_BASE64;
inline const QString &CONTEXT_KEY_VISION_LATEST_FRAME_ID = AgentContextKeys::VISION_LATEST_FRAME_ID;
inline const QString &CONTEXT_KEY_VISION_LATEST_WIDTH = AgentContextKeys::VISION_LATEST_WIDTH;
inline const QString &CONTEXT_KEY_VISION_LATEST_HEIGHT = AgentContextKeys::VISION_LATEST_HEIGHT;
inline const QString &CONTEXT_KEY_VISION_LATEST_MODALITY = AgentContextKeys::VISION_LATEST_MODALITY;
inline const QString &CONTEXT_KEY_VISION_LLM_LAST_REQUEST_ID = AgentContextKeys::VISION_LLM_LAST_REQUEST_ID;
inline const QString &CONTEXT_KEY_VISION_LLM_PENDING = AgentContextKeys::VISION_LLM_PENDING;
inline const QString &CONTEXT_KEY_VISION_UPDATED_AT = AgentContextKeys::VISION_UPDATED_AT;

constexpr int MAX_CONVERSATION_HISTORY_ITEMS = 60;
constexpr int DEFAULT_ASYNC_TIMEOUT_MS = 120000;
constexpr int MAX_ASYNC_TIMEOUT_MS = 600000;
constexpr int MEMORY_SHUTDOWN_TIMEOUT_MS = 2000;
constexpr double LLM_TEMPERATURE_MIN = 0.0;
constexpr double LLM_TEMPERATURE_MAX = 2.0;
constexpr double LLM_TOP_P_MIN = 0.0;
constexpr double LLM_TOP_P_MAX = 1.0;
constexpr double LLM_FREQUENCY_PENALTY_MIN = -2.0;
constexpr double LLM_FREQUENCY_PENALTY_MAX = 2.0;
constexpr double LLM_PRESENCE_PENALTY_MIN = -2.0;
constexpr double LLM_PRESENCE_PENALTY_MAX = 2.0;
constexpr int LLM_MAX_TOKENS_MIN = 1;
constexpr int LLM_MAX_TOKENS_MAX = 32768;

inline const QString TRIGGER_TYPE_USER = QStringLiteral("user");
inline const QString TRIGGER_TYPE_VISION = QStringLiteral("vision");
inline const QString ASYNC_CLIENT_TEXT = QStringLiteral("text");
inline const QString ASYNC_CLIENT_VISION = QStringLiteral("vision");
inline const QString ASYNC_CLIENT_WEB = QStringLiteral("web");
inline const QString OUTPUT_SOURCE_USER_RESPONSE = QStringLiteral("user_response");
inline const QString OUTPUT_SOURCE_VISION_PROACTIVE = QStringLiteral("vision_proactive");

/**
 * @brief 将图像模态名称转换为视觉客户端使用的媒体类型。
 * @param[in] modality 图像模态名称。
 * @return 标准 MIME 媒体类型。
 */
inline QString ResolveVisionMediaType(const QString &modality)
{
    if (modality.contains(QStringLiteral("jpeg"), Qt::CaseInsensitive)
        || modality.contains(QStringLiteral("jpg"), Qt::CaseInsensitive))
    {
        return QStringLiteral("image/jpeg");
    }

    return QStringLiteral("image/png");
}

} // namespace AgentRuntimeInternal

} // namespace vpet

#endif // VPET_AGENT_RUNTIME_INTERNAL_H
