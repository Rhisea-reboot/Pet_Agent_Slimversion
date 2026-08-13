#include "vpet/agent/agent_dag_graph.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QQueue>

#include <algorithm>

namespace vpet
{

AgentDagGraph::AgentDagGraph()
    : m_adjacentList()
    , m_predecessorList()
    , m_inDegree()
    , m_nodes()
    , m_nodeIndexMap()
{
}

bool AgentDagGraph::LoadFromJsonFile(const QString &configPath, QString &errorMessage)
{
    if (configPath.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Agent DAG config path is empty.");
        return false;
    }

    QFile file(configPath);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        errorMessage = QStringLiteral("Failed to open Agent DAG config file.");
        return false;
    }

    const QByteArray jsonData = file.readAll();
    file.close();

    return LoadFromJsonData(jsonData, errorMessage);
}

bool AgentDagGraph::LoadFromJsonData(const QByteArray &jsonData, QString &errorMessage)
{
    if (jsonData.isEmpty())
    {
        errorMessage = QStringLiteral("Agent DAG config data is empty.");
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(jsonData, &parseError);

    if (parseError.error != QJsonParseError::NoError)
    {
        errorMessage = QStringLiteral("Agent DAG JSON parse error: %1").arg(
                           parseError.errorString());
        return false;
    }

    if (!document.isObject())
    {
        errorMessage = QStringLiteral("Agent DAG root is not a JSON object.");
        return false;
    }

    const QJsonObject rootObject = document.object();
    const QJsonValue nodesValue = rootObject.value(QStringLiteral("nodes"));
    const QJsonValue edgesValue = rootObject.value(QStringLiteral("edges"));

    if (!nodesValue.isArray())
    {
        errorMessage = QStringLiteral("Agent DAG nodes field is missing or invalid.");
        return false;
    }

    if (!edgesValue.isArray())
    {
        errorMessage = QStringLiteral("Agent DAG edges field is missing or invalid.");
        return false;
    }

    Clear();

    const QJsonArray nodesArray = nodesValue.toArray();

    if (nodesArray.isEmpty())
    {
        errorMessage = QStringLiteral("Agent DAG node list is empty.");
        return false;
    }

    Reset(nodesArray.size());

    for (const QJsonValue &nodeValue : nodesArray)
    {
        _tagAgentDagNode node;

        if (!ParseNode(nodeValue, node, errorMessage))
        {
            Clear();
            return false;
        }

        if (!AddNode(node, errorMessage))
        {
            Clear();
            return false;
        }
    }

    const QJsonArray edgesArray = edgesValue.toArray();

    for (const QJsonValue &edgeValue : edgesArray)
    {
        if (!edgeValue.isObject())
        {
            Clear();
            errorMessage = QStringLiteral("Agent DAG edge entry is not an object.");
            return false;
        }

        const QJsonObject edgeObject = edgeValue.toObject();
        const QString fromNode = edgeObject.value(QStringLiteral("from")).toString();
        const QString toNode = edgeObject.value(QStringLiteral("to")).toString();

        if (!AddEdge(fromNode, toNode, errorMessage))
        {
            Clear();
            return false;
        }
    }

    QVector<QString> order;

    if (!TopologicalSort(order, errorMessage))
    {
        Clear();
        return false;
    }

    return true;
}

bool AgentDagGraph::TopologicalSort(QVector<QString> &order, QString &errorMessage) const
{
    order.clear();

    if (m_nodes.isEmpty())
    {
        errorMessage = QStringLiteral("Agent DAG graph is empty.");
        return false;
    }

    QVector<int> inDegree = m_inDegree;
    QQueue<int> zeroInDegreeQueue;

    for (int index = 0; index < inDegree.size(); index += 1)
    {
        if (inDegree.at(index) == 0)
        {
            zeroInDegreeQueue.enqueue(index);
        }
    }

    while (!zeroInDegreeQueue.isEmpty())
    {
        const int currentIndex = zeroInDegreeQueue.dequeue();
        order.append(m_nodes.at(currentIndex).id);

        for (const _tagAgentDagEdge &edge : m_adjacentList.at(currentIndex))
        {
            const int targetIndex = edge.targetIndex;

            if ((targetIndex < 0) || (targetIndex >= inDegree.size()))
            {
                errorMessage = QStringLiteral("Agent DAG edge target index is invalid.");
                order.clear();
                return false;
            }

            inDegree[targetIndex] -= 1;

            if (inDegree.at(targetIndex) == 0)
            {
                zeroInDegreeQueue.enqueue(targetIndex);
            }
        }
    }

    if (order.size() != m_nodes.size())
    {
        errorMessage = QStringLiteral("Agent DAG contains a cycle.");
        order.clear();
        return false;
    }

    return true;
}

bool AgentDagGraph::IsEmpty() const
{
    return m_nodes.isEmpty();
}

int AgentDagGraph::GetNodeCount() const
{
    return m_nodes.size();
}

QVector<QString> AgentDagGraph::GetNodeNames() const
{
    QVector<QString> nodeNames;
    nodeNames.reserve(m_nodes.size());

    for (const _tagAgentDagNode &node : m_nodes)
    {
        nodeNames.append(node.id);
    }

    return nodeNames;
}

bool AgentDagGraph::GetNode(const QString &nodeId, _tagAgentDagNode &node) const
{
    const QString normalizedNodeId = nodeId.trimmed();

    if (normalizedNodeId.isEmpty() || !m_nodeIndexMap.contains(normalizedNodeId))
    {
        node = _tagAgentDagNode();
        return false;
    }

    const int nodeIndex = m_nodeIndexMap.value(normalizedNodeId);

    if ((nodeIndex < 0) || (nodeIndex >= m_nodes.size()))
    {
        node = _tagAgentDagNode();
        return false;
    }

    node = m_nodes.at(nodeIndex);

    return true;
}

bool AgentDagGraph::GetSuccessors(const QString &nodeId,
                                  QVector<QString> &successors) const
{
    successors.clear();
    const QString normalizedNodeId = nodeId.trimmed();

    if (normalizedNodeId.isEmpty() || !m_nodeIndexMap.contains(normalizedNodeId))
    {
        return false;
    }

    const int nodeIndex = m_nodeIndexMap.value(normalizedNodeId);

    if ((nodeIndex < 0) || (nodeIndex >= m_adjacentList.size()))
    {
        return false;
    }

    QVector<int> successorIndices;
    successorIndices.reserve(m_adjacentList.at(nodeIndex).size());

    for (const _tagAgentDagEdge &edge : m_adjacentList.at(nodeIndex))
    {
        successorIndices.append(edge.targetIndex);
    }

    std::sort(successorIndices.begin(), successorIndices.end());
    successors.reserve(successorIndices.size());

    for (const int successorIndex : successorIndices)
    {
        if ((successorIndex < 0) || (successorIndex >= m_nodes.size()))
        {
            successors.clear();
            return false;
        }

        successors.append(m_nodes.at(successorIndex).id);
    }

    return true;
}

bool AgentDagGraph::GetPredecessors(const QString &nodeId,
                                    QVector<QString> &predecessors) const
{
    predecessors.clear();
    const QString normalizedNodeId = nodeId.trimmed();

    if (normalizedNodeId.isEmpty() || !m_nodeIndexMap.contains(normalizedNodeId))
    {
        return false;
    }

    const int nodeIndex = m_nodeIndexMap.value(normalizedNodeId);

    if ((nodeIndex < 0) || (nodeIndex >= m_predecessorList.size()))
    {
        return false;
    }

    QVector<int> predecessorIndices = m_predecessorList.at(nodeIndex);
    std::sort(predecessorIndices.begin(), predecessorIndices.end());
    predecessors.reserve(predecessorIndices.size());

    for (const int predecessorIndex : predecessorIndices)
    {
        if ((predecessorIndex < 0) || (predecessorIndex >= m_nodes.size()))
        {
            predecessors.clear();
            return false;
        }

        predecessors.append(m_nodes.at(predecessorIndex).id);
    }

    return true;
}

QVector<QString> AgentDagGraph::GetSourceNodes() const
{
    QVector<QString> sourceNodes;

    for (int nodeIndex = 0; nodeIndex < m_nodes.size(); nodeIndex += 1)
    {
        if (m_inDegree.at(nodeIndex) == 0)
        {
            sourceNodes.append(m_nodes.at(nodeIndex).id);
        }
    }

    return sourceNodes;
}

bool AgentDagGraph::GetInDegree(const QString &nodeId, int &inDegree) const
{
    inDegree = 0;
    const QString normalizedNodeId = nodeId.trimmed();

    if (normalizedNodeId.isEmpty() || !m_nodeIndexMap.contains(normalizedNodeId))
    {
        return false;
    }

    const int nodeIndex = m_nodeIndexMap.value(normalizedNodeId);

    if ((nodeIndex < 0) || (nodeIndex >= m_inDegree.size()))
    {
        return false;
    }

    inDegree = m_inDegree.at(nodeIndex);
    return true;
}

QHash<QString, int> AgentDagGraph::GetInDegreeMap() const
{
    QHash<QString, int> inDegreeMap;
    inDegreeMap.reserve(m_nodes.size());

    for (int nodeIndex = 0; nodeIndex < m_nodes.size(); nodeIndex += 1)
    {
        inDegreeMap.insert(m_nodes.at(nodeIndex).id, m_inDegree.at(nodeIndex));
    }

    return inDegreeMap;
}

void AgentDagGraph::Clear()
{
    m_adjacentList.clear();
    m_predecessorList.clear();
    m_inDegree.clear();
    m_nodes.clear();
    m_nodeIndexMap.clear();
}

void AgentDagGraph::Reset(int nodeCount)
{
    if (nodeCount <= 0)
    {
        Clear();
        return;
    }

    m_adjacentList.clear();
    m_adjacentList.resize(nodeCount);
    m_predecessorList.clear();
    m_predecessorList.resize(nodeCount);
    m_inDegree.clear();
    m_inDegree.resize(nodeCount);
    m_nodes.clear();
    m_nodes.reserve(nodeCount);
    m_nodeIndexMap.clear();
}

bool AgentDagGraph::AddNode(const _tagAgentDagNode &node, QString &errorMessage)
{
    _tagAgentDagNode normalizedNode = node;
    normalizedNode.id = node.id.trimmed();
    normalizedNode.type = node.type.trimmed();

    if (normalizedNode.id.isEmpty())
    {
        errorMessage = QStringLiteral("Agent DAG node id is empty.");
        return false;
    }

    if (normalizedNode.type.isEmpty())
    {
        errorMessage = QStringLiteral("Agent DAG node type is empty: %1").arg(
                           normalizedNode.id);
        return false;
    }

    if (m_nodeIndexMap.contains(normalizedNode.id))
    {
        errorMessage = QStringLiteral("Agent DAG contains duplicate node: %1").arg(
                           normalizedNode.id);
        return false;
    }

    const int nodeIndex = m_nodes.size();

    if (nodeIndex >= m_adjacentList.size())
    {
        errorMessage = QStringLiteral("Agent DAG node count exceeds declared size.");
        return false;
    }

    m_nodes.append(normalizedNode);
    m_nodeIndexMap.insert(normalizedNode.id, nodeIndex);

    return true;
}

bool AgentDagGraph::ParseNode(const QJsonValue &nodeValue,
                              _tagAgentDagNode &node,
                              QString &errorMessage) const
{
    node = _tagAgentDagNode();

    if (nodeValue.isString())
    {
        node.id = nodeValue.toString().trimmed();
        node.type = node.id;
        return true;
    }

    if (!nodeValue.isObject())
    {
        errorMessage = QStringLiteral("Agent DAG node entry is not a string or object.");
        return false;
    }

    const QJsonObject nodeObject = nodeValue.toObject();
    node.id = nodeObject.value(QStringLiteral("id")).toString().trimmed();
    node.type = nodeObject.value(QStringLiteral("type")).toString().trimmed();

    if (nodeObject.contains(QStringLiteral("config")))
    {
        const QJsonValue configValue = nodeObject.value(QStringLiteral("config"));

        if (!configValue.isObject())
        {
            errorMessage = QStringLiteral("Agent DAG node config is not an object: %1").arg(
                               node.id);
            node = _tagAgentDagNode();
            return false;
        }

        node.config = configValue.toObject();
    }

    return true;
}

bool AgentDagGraph::AddEdge(const QString &fromNode,
                            const QString &toNode,
                            QString &errorMessage)
{
    const QString normalizedFromNode = fromNode.trimmed();
    const QString normalizedToNode = toNode.trimmed();

    if (normalizedFromNode.isEmpty() || normalizedToNode.isEmpty())
    {
        errorMessage = QStringLiteral("Agent DAG edge endpoint is empty.");
        return false;
    }

    if (!m_nodeIndexMap.contains(normalizedFromNode))
    {
        errorMessage = QStringLiteral("Agent DAG edge source node is unknown: %1").arg(
                           normalizedFromNode);
        return false;
    }

    if (!m_nodeIndexMap.contains(normalizedToNode))
    {
        errorMessage = QStringLiteral("Agent DAG edge target node is unknown: %1").arg(
                           normalizedToNode);
        return false;
    }

    const int fromIndex = m_nodeIndexMap.value(normalizedFromNode);
    const int toIndex = m_nodeIndexMap.value(normalizedToNode);

    if (fromIndex == toIndex)
    {
        errorMessage = QStringLiteral("Agent DAG self edge is not allowed: %1").arg(
                           normalizedFromNode);
        return false;
    }

    for (const _tagAgentDagEdge &edge : m_adjacentList.at(fromIndex))
    {
        if (edge.targetIndex == toIndex)
        {
            errorMessage = QStringLiteral("Agent DAG contains duplicate edge: %1 -> %2").arg(
                               normalizedFromNode,
                               normalizedToNode);
            return false;
        }
    }

    _tagAgentDagEdge edge;
    edge.targetIndex = toIndex;
    m_adjacentList[fromIndex].append(edge);
    m_predecessorList[toIndex].append(fromIndex);
    m_inDegree[toIndex] += 1;

    return true;
}

} // namespace vpet
