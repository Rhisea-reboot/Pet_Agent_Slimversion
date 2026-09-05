#ifndef VPET_AGENT_TOOLS_ITOOL_H
#define VPET_AGENT_TOOLS_ITOOL_H

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace vpet
{

/**
 * @brief 工具信任分级（v1 权限分级依据）
 */
enum class ToolTrustTier
{
    ReadOnly,  ///< 只读工具：查询外部状态，无副作用
    SideEffect ///< 副作用工具：会改变外部状态
};

/**
 * @brief 工具执行模式（v1 只使用 Sequential 单飞）
 */
enum class ToolExecutionMode
{
    Sequential, ///< 顺序单飞执行
    Parallel    ///< 并行执行（v1 预留，不实现）
};

/**
 * @brief 单个工具参数的 JSON Schema 子集描述。
 *
 * v1 只支持 type/required/enum/description 子集，超范围属性由编辑器提示。
 */
struct _tagToolParameterSchema
{
    QString name;           ///< 参数名
    QString type;           ///< 类型："string"|"integer"|"boolean"|"array"
    QString description;    ///< 参数说明，喂给 LLM
    bool required = false;  ///< 是否必填
    QStringList enumValues; ///< 可选枚举值（仅字符串参数）
};

/**
 * @brief 工具规格声明。
 *
 * name 全局唯一（如 "web.search"）；description 与 parameters 喂给 LLM，
 * label 供 DAG 编辑器展示。
 */
struct _tagToolSpec
{
    QString name;                                   ///< 工具名，全局唯一
    QString label;                                  ///< UI 展示名
    QString description;                            ///< 能力描述，喂给 LLM
    ToolTrustTier trustTier = ToolTrustTier::ReadOnly; ///< 信任分级
    QVector<_tagToolParameterSchema> parameters;    ///< 参数 Schema 列表
};

/**
 * @brief 一次工具调用的请求。
 *
 * callId 来自 LLM 返回的 tool_call.id；arguments 为 LLM 提供的参数对象。
 */
struct _tagToolCall
{
    QString callId;        ///< 工具调用 ID
    QString toolName;      ///< 工具名
    QJsonObject arguments; ///< LLM 提供的参数
    quint64 executionId = 0; ///< Scheduler-assigned identity; copy to completion.
};

/**
 * @brief 工具执行结果。
 *
 * 错误不抛异常：ok=false 且 textOutput 为脱敏错误描述，由循环转为
 * isError 工具结果回喂 LLM。details 只用于审计，不进 LLM。
 */
struct _tagToolExecutionResult
{
    bool ok = false;            ///< 是否成功
    QString textOutput;         ///< 回喂 LLM 的文本（成功内容或脱敏错误）
    QJsonObject details;        ///< 审计信息，不进 LLM
    bool terminateHint = false; ///< 工具请求提前收束的提示
    quint64 executionId = 0; ///< Must match the supplied call.executionId.
};

/**
 * @brief 工具抽象接口。
 *
 * 完全对齐 WebSearchTool 的工具层形态：结构化输入输出、忙态、可取消、
 * 不碰提示词与上下文。Execute 异步启动，结果通过 Completed 返回。
 */
class ITool : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造工具。
     * @param[in] parent QObject 父对象。
     */
    explicit ITool(QObject *parent = nullptr);

    /**
     * @brief 析构工具。
     */
    ~ITool() override = default;

    /**
     * @brief 返回工具规格声明。
     * @return 工具规格。
     */
    virtual _tagToolSpec Spec() const = 0;

    /**
     * @brief 校验 LLM 提供的参数。
     * @param[in] arguments 参数对象。
     * @param[out] errorMessage 错误描述。
     * @return 参数有效返回 true。
     */
    virtual bool ValidateArguments(const QJsonObject &arguments,
                                   QString &errorMessage) const = 0;

    /**
     * @brief 异步启动一次工具执行。
     * @param[in] call 工具调用请求。
     */
    virtual void Execute(const _tagToolCall &call) = 0;

    /**
     * @brief 取消当前工具执行。
     */
    virtual void Cancel() = 0;

    /**
     * @brief 判断工具是否有活动执行。
     * @return 有活动执行返回 true。
     */
    virtual bool IsBusy() const = 0;

signals:
    /**
     * @brief 工具执行完成（成功、失败或被取消均通过该信号返回）。
     * @param[in] result 结构化执行结果。
     */
    void Completed(const vpet::_tagToolExecutionResult &result);
};

/**
 * @brief 按工具规格的 JSON Schema 子集校验参数。
 *
 * 校验 required 缺失、type 类型匹配与 enum 枚举值；未声明的参数忽略。
 * @param[in] spec 工具规格。
 * @param[in] arguments 参数对象。
 * @param[out] errorMessage 错误描述。
 * @return 参数有效返回 true。
 */
bool ValidateToolArguments(const _tagToolSpec &spec,
                           const QJsonObject &arguments,
                           QString &errorMessage);

/**
 * @brief 生成参数摘要文本用于审计。
 * @param[in] arguments 参数对象。
 * @param[in] maxChars 最大字符数，超长截断。
 * @return 紧凑 JSON 摘要。
 */
QString SummarizeToolArguments(const QJsonObject &arguments, int maxChars);

} // namespace vpet

Q_DECLARE_METATYPE(vpet::_tagToolCall)
Q_DECLARE_METATYPE(vpet::_tagToolExecutionResult)
Q_DECLARE_METATYPE(vpet::_tagToolSpec)

#endif // VPET_AGENT_TOOLS_ITOOL_H
