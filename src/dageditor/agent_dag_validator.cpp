#include "vpet/dageditor/agent_dag_validator.h"

#include <QHash>
#include <QJsonArray>
#include <QQueue>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace vpet
{

AgentDagValidator::AgentDagValidator(const AgentNodeCatalog *catalog)
    : m_catalog((catalog != nullptr) ? catalog : &AgentNodeCatalog::Instance())
{
}

bool AgentDagValidator::HasErrors(const QVector<DagValidationIssue> &issues)
{
    for (const DagValidationIssue &issue : issues)
    {
        if (issue.severity == DagValidationSeverity::Error)
        {
            return true;
        }
    }

    return false;
}

QVector<DagValidationIssue> AgentDagValidator::Validate(const DagDocument &document) const
{
    QVector<DagValidationIssue> issues;
    const auto addError = [&issues](const QString &nodeId, const QString &code,
                                    const QString &message)
    {
        DagValidationIssue issue;
        issue.severity = DagValidationSeverity::Error;
        issue.nodeId = nodeId;
        issue.code = code;
        issue.message = message;
        issues.append(issue);
    };
    const auto addWarning = [&issues](const QString &nodeId, const QString &code,
                                      const QString &message)
    {
        DagValidationIssue issue;
        issue.severity = DagValidationSeverity::Warning;
        issue.nodeId = nodeId;
        issue.code = code;
        issue.message = message;
        issues.append(issue);
    };

    const QVector<_tagDagDocumentNode> &nodes = document.GetNodes();
    const QVector<_tagDagDocumentEdge> &edges = document.GetEdges();

    // ---- 图级结构规则 --------------------------------------------------------
    if (nodes.isEmpty())
    {
        addError(QString(), QStringLiteral("empty_graph"),
                 QStringLiteral("节点表为空，至少需要一个触发源节点。"));
        return issues;
    }

    QSet<QString> seenIds;
    QSet<QString> validIds; // 非空且未重复的节点 id 才参与后续规则

    for (const _tagDagDocumentNode &node : nodes)
    {
        const QString nodeId = node.id.trimmed();

        if (nodeId.isEmpty())
        {
            addError(QString(), QStringLiteral("missing_node_id"),
                     QStringLiteral("存在缺少 id 的节点。"));
            continue;
        }

        if (seenIds.contains(nodeId))
        {
            addError(nodeId, QStringLiteral("duplicate_node_id"),
                     QStringLiteral("节点 id 重复：%1").arg(nodeId));
            validIds.remove(nodeId);
            continue;
        }

        seenIds.insert(nodeId);
        validIds.insert(nodeId);
    }

    for (const _tagDagDocumentNode &node : nodes)
    {
        const QString nodeId = node.id.trimmed();

        if (!validIds.contains(nodeId))
        {
            continue; // id 缺失或重复的问题已单独上报，避免重复噪音
        }

        if (node.type.trimmed().isEmpty())
        {
            addError(nodeId, QStringLiteral("missing_node_type"),
                     QStringLiteral("节点缺少 type 字段。"));
            continue;
        }

        DagNodeSpec spec;

        if (!m_catalog->Find(node.type, spec))
        {
            addError(nodeId, QStringLiteral("unknown_type"),
                     QStringLiteral("未知节点类型：%1").arg(node.type));
            continue;
        }

        ValidateNodeConfig(node, spec, issues);
        if (node.type == QStringLiteral("tool.loop")) {
            const QStringList builtins{QStringLiteral("web.search"), QStringLiteral("memory.search"), QStringLiteral("screen.describe")};
            for (const QJsonValue &tool : node.config.value(QStringLiteral("tools")).toArray()) {
                if (!tool.isString() || !builtins.contains(tool.toString()))
                    addError(nodeId, QStringLiteral("unknown_tool"), QStringLiteral("Unknown built-in tool: %1").arg(tool.toString()));
            }
        }

        // 非 source 节点声明 trigger 是无效配置；source 节点的 trigger 取值
        // 必须是运行时可派发的事件来源。
        if (!node.config.value(QStringLiteral("trigger")).toString().trimmed().isEmpty()
            && !spec.isSource)
        {
            addWarning(nodeId, QStringLiteral("trigger_on_non_source"),
                       QStringLiteral("%1 不是触发源节点，config.trigger 不会生效。").arg(nodeId));
        }
    }

    // ---- 边规则 --------------------------------------------------------------
    QSet<QString> seenEdges;

    for (const _tagDagDocumentEdge &edge : edges)
    {
        const QString edgeKey = edge.from + QStringLiteral("->") + edge.to;

        if (edge.from.isEmpty() || edge.to.isEmpty())
        {
            addError(edge.from, QStringLiteral("edge_endpoint_empty"),
                     QStringLiteral("存在端点为空的边。"));
            continue;
        }

        if (!seenIds.contains(edge.from))
        {
            addError(edge.from, QStringLiteral("edge_references_missing_node"),
                     QStringLiteral("边的起点不存在：%1").arg(edge.from));
        }

        if (!seenIds.contains(edge.to))
        {
            addError(edge.to, QStringLiteral("edge_references_missing_node"),
                     QStringLiteral("边的终点不存在：%1").arg(edge.to));
        }

        if (edge.from == edge.to && seenIds.contains(edge.from))
        {
            addError(edge.from, QStringLiteral("self_loop"),
                     QStringLiteral("节点不允许自环：%1").arg(edge.from));
        }

        if (seenEdges.contains(edgeKey))
        {
            addError(edge.from, QStringLiteral("duplicate_edge"),
                     QStringLiteral("重复边：%1 -> %2").arg(edge.from, edge.to));
        }

        seenEdges.insert(edgeKey);
    }

    // ---- 环检测（Kahn 拓扑排序）----------------------------------------------
    QHash<QString, int> inDegree;

    for (const QString &nodeId : seenIds)
    {
        inDegree.insert(nodeId, 0);
    }

    for (const _tagDagDocumentEdge &edge : edges)
    {
        if (inDegree.contains(edge.from) && inDegree.contains(edge.to)
            && (edge.from != edge.to))
        {
            inDegree[edge.to] += 1;
        }
    }

    QQueue<QString> readyQueue;

    for (auto iterator = inDegree.constBegin(); iterator != inDegree.constEnd(); ++iterator)
    {
        if (iterator.value() == 0)
        {
            readyQueue.enqueue(iterator.key());
        }
    }

    int visitedCount = 0;

    while (!readyQueue.isEmpty())
    {
        const QString nodeId = readyQueue.dequeue();
        ++visitedCount;

        for (const _tagDagDocumentEdge &edge : edges)
        {
            if ((edge.from == nodeId) && (edge.from != edge.to)
                && inDegree.contains(edge.to))
            {
                const int remaining = --inDegree[edge.to];

                if (remaining == 0)
                {
                    readyQueue.enqueue(edge.to);
                }
            }
        }
    }

    if (visitedCount != inDegree.size())
    {
        QStringList cycleNodes;

        for (auto iterator = inDegree.constBegin(); iterator != inDegree.constEnd(); ++iterator)
        {
            if (iterator.value() > 0)
            {
                cycleNodes.append(iterator.key());
            }
        }

        std::sort(cycleNodes.begin(), cycleNodes.end());
        addError(cycleNodes.value(0), QStringLiteral("cycle_detected"),
                 QStringLiteral("图中存在环，涉及节点（部分）：%1").arg(cycleNodes.join(QStringLiteral(", "))));
    }

    // ---- 提示级规则 ----------------------------------------------------------
    for (const _tagDagDocumentNode &node : nodes)
    {
        const QString nodeId = node.id.trimmed();

        if (!validIds.contains(nodeId))
        {
            continue;
        }

        int incoming = 0;
        int outgoing = 0;

        for (const _tagDagDocumentEdge &edge : edges)
        {
            if (edge.to == nodeId)
            {
                ++incoming;
            }

            if (edge.from == nodeId)
            {
                ++outgoing;
            }
        }

        if ((incoming == 0) && (outgoing == 0))
        {
            addWarning(nodeId, QStringLiteral("orphan_node"),
                       QStringLiteral("节点 %1 没有任何连线，运行时不会被执行。").arg(nodeId));
        }

        if (incoming > 1
            && !node.config.contains(QStringLiteral("merge")))
        {
            addWarning(nodeId, QStringLiteral("join_without_merge_policy"),
                       QStringLiteral("节点 %1 有多条入边且未声明 config.merge 合并策略，将按运行时默认规则合并。").arg(nodeId));
        }
    }

    return issues;
}

void AgentDagValidator::ValidateNodeConfig(const _tagDagDocumentNode &node,
                                           const DagNodeSpec &spec,
                                           QVector<DagValidationIssue> &issues) const
{
    const auto appendIssue = [&issues, &node](DagValidationSeverity severity,
                                              const QString &code,
                                              const QString &message)
    {
        DagValidationIssue issue;
        issue.severity = severity;
        issue.nodeId = node.id.trimmed();
        issue.code = code;
        issue.message = message;
        issues.append(issue);
    };

    for (const DagInputSpec &input : spec.inputs)
    {
        const QJsonValue configuredValue = node.config.value(input.key);

        // 必填键缺失：undefined 或显式 null 都视为缺失。
        if (input.required
            && (configuredValue.isUndefined() || configuredValue.isNull()))
        {
            appendIssue(DagValidationSeverity::Error,
                        QStringLiteral("missing_required_config"),
                        QStringLiteral("节点 %1 缺少必填配置 %2。").arg(node.id, input.label));
            continue;
        }

        if (configuredValue.isUndefined() || configuredValue.isNull())
        {
            continue;
        }

        switch (input.widget)
        {
        case DagWidgetType::Int:
        case DagWidgetType::Float:
        {
            if (!configuredValue.isDouble())
            {
                appendIssue(DagValidationSeverity::Error,
                            QStringLiteral("invalid_value_type"),
                            QStringLiteral("节点 %1 的 %2 必须是数字。").arg(node.id, input.label));
                break;
            }

            const double numericValue = configuredValue.toDouble();

            if (input.minValue.isValid() && (numericValue < input.minValue.toDouble()))
            {
                appendIssue(DagValidationSeverity::Error,
                            QStringLiteral("value_out_of_range"),
                            QStringLiteral("节点 %1 的 %2 不能小于 %3。")
                                .arg(node.id, input.label, input.minValue.toString()));
            }

            if (input.maxValue.isValid() && (numericValue > input.maxValue.toDouble()))
            {
                appendIssue(DagValidationSeverity::Error,
                            QStringLiteral("value_out_of_range"),
                            QStringLiteral("节点 %1 的 %2 不能大于 %3。")
                                .arg(node.id, input.label, input.maxValue.toString()));
            }

            break;
        }
        case DagWidgetType::Bool:
        {
            if (!configuredValue.isBool())
            {
                appendIssue(DagValidationSeverity::Error,
                            QStringLiteral("invalid_value_type"),
                            QStringLiteral("节点 %1 的 %2 必须是布尔值。").arg(node.id, input.label));
            }

            break;
        }
        case DagWidgetType::Enum:
        {
            const QString textValue = configuredValue.toString().trimmed().toLower();

            bool matched = false;

            for (const QString &enumValue : input.enumValues)
            {
                if (enumValue.compare(textValue, Qt::CaseInsensitive) == 0)
                {
                    matched = true;
                    break;
                }
            }

            if (!matched)
            {
                appendIssue(DagValidationSeverity::Error,
                            QStringLiteral("invalid_enum_value"),
                            QStringLiteral("节点 %1 的 %2 只允许：%3。")
                                .arg(node.id, input.label, input.enumValues.join(QStringLiteral(" / "))));
            }

            break;
        }
        case DagWidgetType::StringList:
        {
            if (!configuredValue.isArray() && !configuredValue.isString())
            {
                appendIssue(DagValidationSeverity::Error,
                            QStringLiteral("invalid_value_type"),
                            QStringLiteral("节点 %1 的 %2 必须是字符串数组。").arg(node.id, input.label));
            }

            break;
        }
        case DagWidgetType::Text:
        case DagWidgetType::MultilineText:
        {
            if (!configuredValue.isString())
            {
                appendIssue(DagValidationSeverity::Error,
                            QStringLiteral("invalid_value_type"),
                            QStringLiteral("节点 %1 的 %2 必须是字符串。").arg(node.id, input.label));
            }

            break;
        }
        }
    }
}

} // namespace vpet
