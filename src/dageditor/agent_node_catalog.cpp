#include "vpet/dageditor/agent_node_catalog.h"

#include "vpet/agent/agent_context_keys.h"

namespace vpet
{

namespace
{

using AgentContextKeys::NODE_TYPE_EMOTION_REWRITE;
using AgentContextKeys::NODE_TYPE_LLM_CHAT;
using AgentContextKeys::NODE_TYPE_MEMORY_RETRIEVE;
using AgentContextKeys::NODE_TYPE_MEMORY_STORE;
using AgentContextKeys::NODE_TYPE_OUTPUT_FORMAT;
using AgentContextKeys::NODE_TYPE_PROACTIVE_TOPIC;
using AgentContextKeys::NODE_TYPE_USER_INPUT;
using AgentContextKeys::NODE_TYPE_VISION_INPUT;
using AgentContextKeys::NODE_TYPE_VISION_LLM;
using AgentContextKeys::NODE_TYPE_WEB_RESEARCH;

const QColor CATEGORY_COLOR_INPUT = QColor(76, 175, 80);     // 输入源
const QColor CATEGORY_COLOR_PERCEPTION = QColor(255, 152, 0); // 感知
const QColor CATEGORY_COLOR_REASONING = QColor(33, 150, 243); // 推理
const QColor CATEGORY_COLOR_TOOL = QColor(156, 39, 176);      // 工具
const QColor CATEGORY_COLOR_MEMORY = QColor(0, 188, 212);     // 记忆
const QColor CATEGORY_COLOR_OUTPUT = QColor(233, 30, 99);     // 输出

/**
 * @brief 构造一个 config 输入声明（必填字段显式传入，其余走默认值）
 */
DagInputSpec MakeInput(const QString &key,
                       const QString &label,
                       DagWidgetType widget,
                       const QVariant &defaultValue,
                       const QString &tooltip)
{
    DagInputSpec spec;
    spec.key = key;
    spec.label = label;
    spec.widget = widget;
    spec.defaultValue = defaultValue;
    spec.tooltip = tooltip;
    return spec;
}

} // anonymous namespace

const AgentNodeCatalog &AgentNodeCatalog::Instance()
{
    static AgentNodeCatalog instance;

    if (instance.m_order.isEmpty())
    {
        RegisterDefaultNodeSpecs(instance);
    }

    return instance;
}

bool AgentNodeCatalog::Register(const DagNodeSpec &spec)
{
    const QString normalizedType = spec.type.trimmed();

    if (normalizedType.isEmpty() || m_specs.contains(normalizedType))
    {
        return false;
    }

    // Enum 声明必须携带选项；数值声明上下界不能颠倒。
    for (const DagInputSpec &input : spec.inputs)
    {
        if (input.key.trimmed().isEmpty())
        {
            return false;
        }

        if ((input.widget == DagWidgetType::Enum) && input.enumValues.isEmpty())
        {
            return false;
        }

        if (input.minValue.isValid() && input.maxValue.isValid()
            && input.minValue.canConvert<double>() && input.maxValue.canConvert<double>()
            && input.minValue.toDouble() > input.maxValue.toDouble())
        {
            return false;
        }
    }

    m_specs.insert(normalizedType, spec);
    m_order.append(normalizedType);
    return true;
}

bool AgentNodeCatalog::Contains(const QString &type) const
{
    return m_specs.contains(type.trimmed());
}

bool AgentNodeCatalog::Find(const QString &type, DagNodeSpec &out) const
{
    const auto iterator = m_specs.constFind(type.trimmed());

    if (iterator == m_specs.constEnd())
    {
        return false;
    }

    out = iterator.value();
    return true;
}

QVector<DagNodeSpec> AgentNodeCatalog::All() const
{
    QVector<DagNodeSpec> specs;
    specs.reserve(m_order.size());

    for (const QString &type : m_order)
    {
        specs.append(m_specs.value(type));
    }

    return specs;
}

QStringList AgentNodeCatalog::Types() const
{
    return m_order;
}

void AgentNodeCatalog::Clear()
{
    m_order.clear();
    m_specs.clear();
}

bool RegisterDefaultNodeSpecs(AgentNodeCatalog &catalog)
{
    bool allRegistered = true;
    const auto registerSpec = [&catalog, &allRegistered](const DagNodeSpec &spec)
    {
        if (!catalog.Contains(spec.type))
        {
            allRegistered = catalog.Register(spec) && allRegistered;
        }
    };

    // ---- user.input：用户文本触发源，透传输入 --------------------------------
    {
        DagNodeSpec spec;
        spec.type = NODE_TYPE_USER_INPUT;
        spec.displayName = QStringLiteral("用户输入");
        spec.category = QStringLiteral("输入源");
        spec.accentColor = CATEGORY_COLOR_INPUT;
        spec.description = QStringLiteral("接收用户文本输入并触发执行链，输入经分支透传给下游节点。");
        spec.reads = QStringList{QStringLiteral("user.input")};
        spec.writes = QStringList{QStringLiteral("node.output.prompt")};
        spec.triggerSource = QStringLiteral("user");
        spec.isSource = true;

        DagInputSpec trigger = MakeInput(QStringLiteral("trigger"),
                                         QStringLiteral("触发来源"),
                                         DagWidgetType::Enum,
                                         QStringLiteral("user"),
                                         QStringLiteral("声明该源节点响应的触发类型；留空表示响应全部触发。"));
        trigger.enumValues = QStringList{QStringLiteral("user"), QStringLiteral("vision")};
        spec.inputs.append(trigger);

        registerSpec(spec);
    }

    // ---- vision.input：视觉帧就绪检查源 --------------------------------------
    {
        DagNodeSpec spec;
        spec.type = NODE_TYPE_VISION_INPUT;
        spec.displayName = QStringLiteral("视觉输入");
        spec.category = QStringLiteral("输入源");
        spec.accentColor = CATEGORY_COLOR_INPUT;
        spec.description = QStringLiteral("检查最新感知帧是否完整可用，写入视觉就绪状态并触发视觉链路。");
        spec.reads = QStringList{QStringLiteral("semantic.image.base64"),
                                 QStringLiteral("semantic.vision.frame_id"),
                                 QStringLiteral("vision.latest_base64")};
        spec.writes = QStringList{QStringLiteral("vision.input_ready"),
                                  QStringLiteral("semantic.vision.state")};
        spec.triggerSource = QStringLiteral("vision");
        spec.isSource = true;

        DagInputSpec trigger = MakeInput(QStringLiteral("trigger"),
                                         QStringLiteral("触发来源"),
                                         DagWidgetType::Enum,
                                         QStringLiteral("vision"),
                                         QStringLiteral("声明该源节点响应的触发类型；留空表示响应全部触发。"));
        trigger.enumValues = QStringList{QStringLiteral("user"), QStringLiteral("vision")};
        spec.inputs.append(trigger);

        registerSpec(spec);
    }

    // ---- vision.llm：屏幕画面识别 --------------------------------------------
    {
        DagNodeSpec spec;
        spec.type = NODE_TYPE_VISION_LLM;
        spec.displayName = QStringLiteral("视觉识别");
        spec.category = QStringLiteral("感知");
        spec.accentColor = CATEGORY_COLOR_PERCEPTION;
        spec.description = QStringLiteral("调用视觉 LLM 描述当前屏幕画面，产出视觉摘要。");
        spec.reads = QStringList{QStringLiteral("semantic.image.base64"),
                                 QStringLiteral("semantic.image.media_type")};
        spec.writes = QStringList{QStringLiteral("semantic.vision.summary"),
                                  QStringLiteral("semantic.vision.state")};

        DagInputSpec prompt = MakeInput(QStringLiteral("prompt"),
                                        QStringLiteral("识别提示词"),
                                        DagWidgetType::MultilineText,
                                        QVariant(QString()),
                                        QStringLiteral("留空时使用内置提示词：请简洁准确地描述画面中与用户交互相关的内容。"));
        spec.inputs.append(prompt);

        DagInputSpec detail = MakeInput(QStringLiteral("detail"),
                                        QStringLiteral("识别精度"),
                                        DagWidgetType::Enum,
                                        QStringLiteral("auto"),
                                        QStringLiteral("视觉请求的 detail 档位。"));
        detail.enumValues = QStringList{QStringLiteral("auto"),
                                        QStringLiteral("low"),
                                        QStringLiteral("high")};
        spec.inputs.append(detail);

        DagInputSpec maxTokens = MakeInput(QStringLiteral("max_tokens"),
                                           QStringLiteral("最大输出 Token"),
                                           DagWidgetType::Int,
                                           256,
                                           QStringLiteral("限制识别结果长度，最小 1。"));
        maxTokens.minValue = 1;
        spec.inputs.append(maxTokens);

        DagInputSpec asyncTimeout = MakeInput(QStringLiteral("async_timeout_ms"),
                                              QStringLiteral("异步超时（毫秒）"),
                                              DagWidgetType::Int,
                                              120000,
                                              QStringLiteral("等待识别回调的超时时间，上限 600000。"));
        asyncTimeout.minValue = 1;
        asyncTimeout.maxValue = 600000;
        spec.inputs.append(asyncTimeout);

        registerSpec(spec);
    }

    // ---- proactive.topic：主动话题决策 ---------------------------------------
    {
        DagNodeSpec spec;
        spec.type = NODE_TYPE_PROACTIVE_TOPIC;
        spec.displayName = QStringLiteral("主动话题");
        spec.category = QStringLiteral("推理");
        spec.accentColor = CATEGORY_COLOR_REASONING;
        spec.description = QStringLiteral("根据视觉摘要决定是否主动发话，并组装发话提示词。");
        spec.reads = QStringList{QStringLiteral("semantic.vision.summary"),
                                 QStringLiteral("semantic.proactive.summary_hash")};
        spec.writes = QStringList{QStringLiteral("semantic.proactive.should_speak"),
                                  QStringLiteral("semantic.proactive.topic"),
                                  QStringLiteral("semantic.proactive.reason"),
                                  QStringLiteral("semantic.text.prompt")};

        spec.inputs.append(MakeInput(QStringLiteral("enabled"),
                                     QStringLiteral("启用主动发话"),
                                     DagWidgetType::Bool,
                                     true,
                                     QStringLiteral("关闭后节点仅透传，不生成主动话题。")));

        spec.inputs.append(MakeInput(QStringLiteral("instruction"),
                                     QStringLiteral("发话指令"),
                                     DagWidgetType::MultilineText,
                                     QVariant(QString()),
                                     QStringLiteral("约束文本 LLM 如何根据视觉摘要生成话语；留空使用内置要求。")));

        DagInputSpec minInterval = MakeInput(QStringLiteral("min_interval_ms"),
                                             QStringLiteral("最小间隔（毫秒）"),
                                             DagWidgetType::Int,
                                             30000,
                                             QStringLiteral("两次主动发话之间的最小时间间隔。"));
        minInterval.minValue = 0;
        spec.inputs.append(minInterval);

        DagInputSpec dedupWindow = MakeInput(QStringLiteral("dedup_window_ms"),
                                             QStringLiteral("去重窗口（毫秒）"),
                                             DagWidgetType::Int,
                                             300000,
                                             QStringLiteral("相同摘要指纹在该窗口内不会重复发话。"));
        dedupWindow.minValue = 0;
        spec.inputs.append(dedupWindow);

        registerSpec(spec);
    }

    // ---- web.research：联网研究 ----------------------------------------------
    {
        DagNodeSpec spec;
        spec.type = NODE_TYPE_WEB_RESEARCH;
        spec.displayName = QStringLiteral("联网研究");
        spec.category = QStringLiteral("工具");
        spec.accentColor = CATEGORY_COLOR_TOOL;
        spec.description = QStringLiteral("对输入提示词执行多轮联网检索与事实核验，产出证据与引用。");
        spec.reads = QStringList{QStringLiteral("semantic.text.prompt"),
                                 QStringLiteral("node.input.prompt")};
        spec.writes = QStringList{QStringLiteral("semantic.web.research.evidence"),
                                  QStringLiteral("semantic.web.research.citations"),
                                  QStringLiteral("semantic.web.research.status"),
                                  QStringLiteral("semantic.web.research.queries"),
                                  QStringLiteral("semantic.web.research.round_count")};

        DagInputSpec mode = MakeInput(QStringLiteral("mode"),
                                      QStringLiteral("检索模式"),
                                      DagWidgetType::Enum,
                                      QStringLiteral("auto"),
                                      QStringLiteral("auto 由研究引擎自行规划，explicit 使用显式查询。"));
        mode.enumValues = QStringList{QStringLiteral("auto"), QStringLiteral("explicit")};
        spec.inputs.append(mode);

        DagInputSpec failurePolicy = MakeInput(QStringLiteral("failure_policy"),
                                               QStringLiteral("失败策略"),
                                               DagWidgetType::Enum,
                                               QStringLiteral("continue"),
                                               QStringLiteral("continue 研究失败时继续下游节点，fail 直接终止本轮。"));
        failurePolicy.enumValues = QStringList{QStringLiteral("continue"), QStringLiteral("fail")};
        spec.inputs.append(failurePolicy);

        const auto boundedInt = [&spec](const QString &key,
                                        const QString &label,
                                        int defaultValue,
                                        int minValue,
                                        int maxValue,
                                        const QString &tooltip)
        {
            DagInputSpec input = MakeInput(key, label, DagWidgetType::Int, defaultValue, tooltip);
            input.minValue = minValue;
            input.maxValue = maxValue;
            spec.inputs.append(input);
        };

        boundedInt(QStringLiteral("max_search_rounds"),
                   QStringLiteral("最大检索轮数"), 3, 1, 3,
                   QStringLiteral("单次研究的最大搜索轮数。"));
        boundedInt(QStringLiteral("max_queries_per_round"),
                   QStringLiteral("每轮最大查询数"), 2, 1, 2,
                   QStringLiteral("每一轮生成的搜索 query 数上限。"));
        boundedInt(QStringLiteral("max_total_results"),
                   QStringLiteral("最大结果总数"), 8, 1, 8,
                   QStringLiteral("全部轮次累计保留的搜索结果条数上限。"));
        boundedInt(QStringLiteral("max_context_chars"),
                   QStringLiteral("上下文字符上限"), 6000, 1, 6000,
                   QStringLiteral("注入 LLM 上下文的研究摘要字符数上限。"));
        boundedInt(QStringLiteral("total_deadline_ms"),
                   QStringLiteral("总时限（毫秒）"), 15000, 1, 15000,
                   QStringLiteral("整个研究流程的墙钟时间预算。"));

        DagInputSpec asyncTimeout = MakeInput(QStringLiteral("async_timeout_ms"),
                                              QStringLiteral("异步超时（毫秒）"),
                                              DagWidgetType::Int,
                                              17000,
                                              QStringLiteral("等待研究回调的超时时间，上限 600000。"));
        asyncTimeout.minValue = 1;
        asyncTimeout.maxValue = 600000;
        spec.inputs.append(asyncTimeout);

        spec.inputs.append(MakeInput(QStringLiteral("require_citations_for_realtime_claims"),
                                     QStringLiteral("实时论断需引用"),
                                     DagWidgetType::Bool,
                                     true,
                                     QStringLiteral("实时性论断缺少引用时标记为未获支持。")));

        spec.inputs.append(MakeInput(QStringLiteral("require_independent_sources_for_high_impact_claims"),
                                     QStringLiteral("高影响论断需独立来源"),
                                     DagWidgetType::Bool,
                                     true,
                                     QStringLiteral("高影响论断需要多个独立来源支持。")));

        DagInputSpec engines = MakeInput(QStringLiteral("engines"),
                                         QStringLiteral("搜索引擎"),
                                         DagWidgetType::StringList,
                                         QVariant(QStringList{QStringLiteral("bing")}),
                                         QStringLiteral("启用的搜索引擎列表；留空回退 bing。"));
        engines.defaultValue = QVariant(QStringList{QStringLiteral("bing")});
        spec.inputs.append(engines);

        registerSpec(spec);
    }

    // ---- llm.chat：文本对话 ---------------------------------------------------
    {
        DagNodeSpec spec;
        spec.type = NODE_TYPE_LLM_CHAT;
        spec.displayName = QStringLiteral("LLM 对话");
        spec.category = QStringLiteral("推理");
        spec.accentColor = CATEGORY_COLOR_REASONING;
        spec.description = QStringLiteral("调用文本 LLM 生成回复，是主对话链路的核心节点。");
        spec.reads = QStringList{QStringLiteral("node.input.prompt"),
                                 QStringLiteral("semantic.text.prompt"),
                                 QStringLiteral("conversation.history")};
        spec.writes = QStringList{QStringLiteral("semantic.text.response"),
                                  QStringLiteral("llm.last_response")};

        DagInputSpec temperature = MakeInput(QStringLiteral("temperature"),
                                             QStringLiteral("温度"),
                                             DagWidgetType::Float,
                                             0.7,
                                             QStringLiteral("采样温度，范围 0 到 2；越高越发散。"));
        temperature.minValue = 0.0;
        temperature.maxValue = 2.0;
        temperature.step = 0.05;
        spec.inputs.append(temperature);

        DagInputSpec topP = MakeInput(QStringLiteral("top_p"),
                                      QStringLiteral("核采样"),
                                      DagWidgetType::Float,
                                      1.0,
                                      QStringLiteral("核采样阈值，范围 0 到 1。"));
        topP.minValue = 0.0;
        topP.maxValue = 1.0;
        topP.step = 0.01;
        spec.inputs.append(topP);

        DagInputSpec frequencyPenalty = MakeInput(QStringLiteral("frequency_penalty"),
                                                  QStringLiteral("频率惩罚"),
                                                  DagWidgetType::Float,
                                                  0.0,
                                                  QStringLiteral("频率惩罚，范围 -2 到 2。"));
        frequencyPenalty.minValue = -2.0;
        frequencyPenalty.maxValue = 2.0;
        frequencyPenalty.step = 0.1;
        spec.inputs.append(frequencyPenalty);

        DagInputSpec presencePenalty = MakeInput(QStringLiteral("presence_penalty"),
                                                 QStringLiteral("存在惩罚"),
                                                 DagWidgetType::Float,
                                                 0.0,
                                                 QStringLiteral("存在惩罚，范围 -2 到 2。"));
        presencePenalty.minValue = -2.0;
        presencePenalty.maxValue = 2.0;
        presencePenalty.step = 0.1;
        spec.inputs.append(presencePenalty);

        DagInputSpec maxTokens = MakeInput(QStringLiteral("max_tokens"),
                                           QStringLiteral("最大输出 Token"),
                                           DagWidgetType::Int,
                                           2048,
                                           QStringLiteral("限制回复长度，范围 1 到 32768。"));
        maxTokens.minValue = 1;
        maxTokens.maxValue = 32768;
        spec.inputs.append(maxTokens);

        spec.inputs.append(MakeInput(QStringLiteral("stream"),
                                     QStringLiteral("流式输出"),
                                     DagWidgetType::Bool,
                                     false,
                                     QStringLiteral("启用后回复按句流式推送 TTS 与气泡。")));

        DagInputSpec asyncTimeout = MakeInput(QStringLiteral("async_timeout_ms"),
                                              QStringLiteral("异步超时（毫秒）"),
                                              DagWidgetType::Int,
                                              120000,
                                              QStringLiteral("等待回复回调的超时时间，上限 600000。"));
        asyncTimeout.minValue = 1;
        asyncTimeout.maxValue = 600000;
        spec.inputs.append(asyncTimeout);

        registerSpec(spec);
    }

    // ---- memory.retrieve：记忆检索 -------------------------------------------
    {
        DagNodeSpec spec;
        spec.type = NODE_TYPE_MEMORY_RETRIEVE;
        spec.displayName = QStringLiteral("记忆检索");
        spec.category = QStringLiteral("记忆");
        spec.accentColor = CATEGORY_COLOR_MEMORY;
        spec.description = QStringLiteral("按当前提示词检索长期记忆，并把浮现记忆注入回答上下文。");
        spec.reads = QStringList{QStringLiteral("semantic.text.prompt"),
                                 QStringLiteral("node.input.prompt")};
        spec.writes = QStringList{QStringLiteral("semantic.memory.entries"),
                                  QStringLiteral("semantic.memory.prompt")};
        registerSpec(spec);
    }

    // ---- memory.store：记忆存储 ----------------------------------------------
    {
        DagNodeSpec spec;
        spec.type = NODE_TYPE_MEMORY_STORE;
        spec.displayName = QStringLiteral("记忆存储");
        spec.category = QStringLiteral("记忆");
        spec.accentColor = CATEGORY_COLOR_MEMORY;
        spec.description = QStringLiteral("从最终回复提炼可记忆内容并写入长期记忆库。");
        spec.reads = QStringList{QStringLiteral("semantic.text.final"),
                                 QStringLiteral("memory.store.intent")};
        spec.writes = QStringList{QStringLiteral("memory.store.intent")};
        registerSpec(spec);
    }

    // ---- emotion.rewrite：情感改写 -------------------------------------------
    {
        DagNodeSpec spec;
        spec.type = NODE_TYPE_EMOTION_REWRITE;
        spec.displayName = QStringLiteral("情感改写");
        spec.category = QStringLiteral("输出");
        spec.accentColor = CATEGORY_COLOR_OUTPUT;
        spec.description = QStringLiteral("结合用户与桌宠情感状态改写回复文本。");
        spec.reads = QStringList{QStringLiteral("semantic.text.response"),
                                 QStringLiteral("emotion.user"),
                                 QStringLiteral("emotion.pet")};
        spec.writes = QStringList{QStringLiteral("emotion.output_text"),
                                  QStringLiteral("semantic.text.final")};
        registerSpec(spec);
    }

    // ---- output.format：最终输出 ---------------------------------------------
    {
        DagNodeSpec spec;
        spec.type = NODE_TYPE_OUTPUT_FORMAT;
        spec.displayName = QStringLiteral("格式化输出");
        spec.category = QStringLiteral("输出");
        spec.accentColor = CATEGORY_COLOR_OUTPUT;
        spec.description = QStringLiteral("汇总链路产出的最终文本，写入输出来源与输出文本键。");
        spec.reads = QStringList{QStringLiteral("semantic.text.final"),
                                 QStringLiteral("semantic.text.response")};
        spec.writes = QStringList{QStringLiteral("output.text"),
                                  QStringLiteral("semantic.output.source"),
                                  QStringLiteral("semantic.text.final")};
        registerSpec(spec);
    }

    return allRegistered;
}

} // namespace vpet
