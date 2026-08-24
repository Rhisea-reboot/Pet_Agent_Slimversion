#include "vpet/dageditor/dag_document.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStringList>

namespace vpet
{

namespace
{

/**
 * @brief 解析单个 JSON 节点项
 */
bool ParseNode(const QJsonObject &nodeObject,
               _tagDagDocumentNode &node,
               QString &errorMessage)
{
    node = _tagDagDocumentNode();
    node.id = nodeObject.value(QStringLiteral("id")).toString().trimmed();
    node.type = nodeObject.value(QStringLiteral("type")).toString().trimmed();

    if (node.id.isEmpty() || node.type.isEmpty())
    {
        errorMessage = QStringLiteral("DAG document node requires non-empty id and type.");
        return false;
    }

    const QJsonValue configValue = nodeObject.value(QStringLiteral("config"));

    if (configValue.isObject())
    {
        node.config = configValue.toObject();
    }

    return true;
}

/**
 * @brief 将布局信息写入节点 JSON
 */
QJsonObject SerializeLayout(const _tagDagDocumentNode &node)
{
    QJsonObject layoutObject;
    QJsonArray positionArray;
    positionArray.append(node.layoutX);
    positionArray.append(node.layoutY);
    layoutObject.insert(QStringLiteral("pos"), positionArray);

    if ((node.layoutWidth > 0.0) && (node.layoutHeight > 0.0))
    {
        QJsonArray sizeArray;
        sizeArray.append(node.layoutWidth);
        sizeArray.append(node.layoutHeight);
        layoutObject.insert(QStringLiteral("size"), sizeArray);
    }

    if (!node.layoutTitle.trimmed().isEmpty())
    {
        layoutObject.insert(QStringLiteral("title"), node.layoutTitle);
    }

    if (node.layoutCollapsed)
    {
        layoutObject.insert(QStringLiteral("collapsed"), true);
    }

    return layoutObject;
}

} // anonymous namespace

bool DagDocument::FromJsonData(const QByteArray &jsonData,
                               DagDocument &out,
                               QString &errorMessage)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(jsonData, &parseError);

    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        errorMessage = QStringLiteral("DAG document JSON parse failed: %1")
                           .arg(parseError.errorString());
        return false;
    }

    return FromJsonObject(document.object(), out, errorMessage);
}

bool DagDocument::FromJsonObject(const QJsonObject &rootObject,
                                 DagDocument &out,
                                 QString &errorMessage)
{
    out = DagDocument();

    if (!rootObject.contains(QStringLiteral("nodes"))
        || !rootObject.value(QStringLiteral("nodes")).isArray())
    {
        errorMessage = QStringLiteral("DAG document requires a nodes array.");
        return false;
    }

    if (!rootObject.contains(QStringLiteral("edges")))
    {
        errorMessage = QStringLiteral("DAG document requires an edges array.");
        return false;
    }

    const QJsonArray nodeArray = rootObject.value(QStringLiteral("nodes")).toArray();
    QHash<QString, int> seenIds;

    for (const QJsonValue &nodeValue : nodeArray)
    {
        _tagDagDocumentNode node;

        if (!nodeValue.isObject()
            || !ParseNode(nodeValue.toObject(), node, errorMessage))
        {
            return false;
        }

        if (seenIds.contains(node.id))
        {
            errorMessage = QStringLiteral("DAG document contains duplicate node id: %1")
                               .arg(node.id);
            return false;
        }

        seenIds.insert(node.id, static_cast<int>(out.m_nodes.size()));
        out.m_nodes.append(node);
    }

    const QJsonValue edgesValue = rootObject.value(QStringLiteral("edges"));

    if (!edgesValue.isArray())
    {
        errorMessage = QStringLiteral("DAG document edges must be an array.");
        return false;
    }

    for (const QJsonValue &edgeValue : edgesValue.toArray())
    {
        if (!edgeValue.isObject())
        {
            errorMessage = QStringLiteral("DAG document edge must be an object.");
            return false;
        }

        _tagDagDocumentEdge edge;
        edge.from = edgeValue.toObject().value(QStringLiteral("from")).toString().trimmed();
        edge.to = edgeValue.toObject().value(QStringLiteral("to")).toString().trimmed();

        if (edge.from.isEmpty() || edge.to.isEmpty())
        {
            errorMessage = QStringLiteral("DAG document edge requires from and to fields.");
            return false;
        }

        out.m_edges.append(edge);
    }

    // editor 元数据整体可选；孤儿坐标（无对应节点 id）静默丢弃。
    const QJsonValue editorValue = rootObject.value(QStringLiteral("editor"));

    if (editorValue.isObject())
    {
        const QJsonObject editorObject = editorValue.toObject();
        const QJsonValue scrollValue = editorObject.value(QStringLiteral("scroll"));

        if (scrollValue.isArray() && (scrollValue.toArray().size() >= 2))
        {
            out.m_scrollX = scrollValue.toArray().at(0).toDouble(0.0);
            out.m_scrollY = scrollValue.toArray().at(1).toDouble(0.0);
        }

        out.m_zoom = editorObject.value(QStringLiteral("zoom")).toDouble(1.0);

        if (out.m_zoom <= 0.0)
        {
            out.m_zoom = 1.0;
        }

        out.m_revision = static_cast<quint64>(
            editorObject.value(QStringLiteral("revision")).toInteger(0));

        const QJsonValue layoutNodesValue = editorObject.value(QStringLiteral("nodes"));

        if (layoutNodesValue.isObject())
        {
            const QJsonObject layoutNodes = layoutNodesValue.toObject();

            for (_tagDagDocumentNode &node : out.m_nodes)
            {
                const QJsonValue layoutValue = layoutNodes.value(node.id);

                if (!layoutValue.isObject())
                {
                    continue;
                }

                const QJsonObject layoutObject = layoutValue.toObject();
                const QJsonValue posValue = layoutObject.value(QStringLiteral("pos"));

                if (posValue.isArray() && (posValue.toArray().size() >= 2))
                {
                    node.hasLayout = true;
                    node.layoutX = posValue.toArray().at(0).toDouble(0.0);
                    node.layoutY = posValue.toArray().at(1).toDouble(0.0);
                }

                const QJsonValue sizeValue = layoutObject.value(QStringLiteral("size"));

                if (sizeValue.isArray() && (sizeValue.toArray().size() >= 2))
                {
                    node.layoutWidth = sizeValue.toArray().at(0).toDouble(0.0);
                    node.layoutHeight = sizeValue.toArray().at(1).toDouble(0.0);
                }

                node.layoutTitle =
                    layoutObject.value(QStringLiteral("title")).toString().trimmed();
                node.layoutCollapsed =
                    layoutObject.value(QStringLiteral("collapsed")).toBool(false);
            }
        }
    }

    // 未识别的顶层字段原样保留，保证往返无损。
    static const QStringList knownKeys = {QStringLiteral("version"),
                                          QStringLiteral("nodes"),
                                          QStringLiteral("edges"),
                                          QStringLiteral("editor")};

    for (auto iterator = rootObject.constBegin(); iterator != rootObject.constEnd(); ++iterator)
    {
        if (!knownKeys.contains(iterator.key()))
        {
            out.m_extraFields.insert(iterator.key(), iterator.value());
        }
    }

    errorMessage.clear();
    return true;
}

QByteArray DagDocument::ToJsonData() const
{
    return QJsonDocument(ToJsonObject()).toJson(QJsonDocument::Indented);
}

QJsonObject DagDocument::ToJsonObject() const
{
    QJsonObject rootObject;
    rootObject.insert(QStringLiteral("version"), kFormatVersion);

    QJsonArray nodeArray;

    for (const _tagDagDocumentNode &node : m_nodes)
    {
        QJsonObject nodeObject;
        nodeObject.insert(QStringLiteral("id"), node.id);
        nodeObject.insert(QStringLiteral("type"), node.type);
        nodeObject.insert(QStringLiteral("config"), node.config);
        nodeArray.append(nodeObject);
    }

    rootObject.insert(QStringLiteral("nodes"), nodeArray);

    QJsonArray edgeArray;

    for (const _tagDagDocumentEdge &edge : m_edges)
    {
        QJsonObject edgeObject;
        edgeObject.insert(QStringLiteral("from"), edge.from);
        edgeObject.insert(QStringLiteral("to"), edge.to);
        edgeArray.append(edgeObject);
    }

    rootObject.insert(QStringLiteral("edges"), edgeArray);

    QJsonObject editorObject;
    editorObject.insert(QStringLiteral("revision"), static_cast<qint64>(m_revision));

    QJsonArray scrollArray;
    scrollArray.append(m_scrollX);
    scrollArray.append(m_scrollY);
    editorObject.insert(QStringLiteral("scroll"), scrollArray);
    editorObject.insert(QStringLiteral("zoom"), m_zoom);

    QJsonObject layoutNodes;

    for (const _tagDagDocumentNode &node : m_nodes)
    {
        if (node.hasLayout)
        {
            layoutNodes.insert(node.id, SerializeLayout(node));
        }
    }

    editorObject.insert(QStringLiteral("nodes"), layoutNodes);
    rootObject.insert(QStringLiteral("editor"), editorObject);

    for (auto iterator = m_extraFields.constBegin(); iterator != m_extraFields.constEnd();
         ++iterator)
    {
        rootObject.insert(iterator.key(), iterator.value());
    }

    return rootObject;
}

const QVector<_tagDagDocumentNode> &DagDocument::GetNodes() const
{
    return m_nodes;
}

const QVector<_tagDagDocumentEdge> &DagDocument::GetEdges() const
{
    return m_edges;
}

void DagDocument::SetNodes(const QVector<_tagDagDocumentNode> &nodes)
{
    m_nodes = nodes;
}

void DagDocument::SetEdges(const QVector<_tagDagDocumentEdge> &edges)
{
    m_edges = edges;
}

bool DagDocument::FindNode(const QString &nodeId, _tagDagDocumentNode &out) const
{
    const int index = IndexOfNode(nodeId);

    if (index < 0)
    {
        return false;
    }

    out = m_nodes.at(index);
    return true;
}

int DagDocument::IndexOfNode(const QString &nodeId) const
{
    for (int index = 0; index < m_nodes.size(); ++index)
    {
        if (m_nodes.at(index).id == nodeId)
        {
            return index;
        }
    }

    return -1;
}

bool DagDocument::UpsertNode(const _tagDagDocumentNode &node)
{
    if (node.id.trimmed().isEmpty())
    {
        return false;
    }

    const int existingIndex = IndexOfNode(node.id);

    if (existingIndex >= 0)
    {
        m_nodes[existingIndex] = node;
    }
    else
    {
        m_nodes.append(node);
    }

    return true;
}

bool DagDocument::RemoveNode(const QString &nodeId)
{
    const int index = IndexOfNode(nodeId);

    if (index < 0)
    {
        return false;
    }

    m_nodes.removeAt(index);

    for (int edgeIndex = m_edges.size() - 1; edgeIndex >= 0; --edgeIndex)
    {
        const _tagDagDocumentEdge &edge = m_edges.at(edgeIndex);

        if ((edge.from == nodeId) || (edge.to == nodeId))
        {
            m_edges.removeAt(edgeIndex);
        }
    }

    return true;
}

bool DagDocument::AddEdge(const QString &from, const QString &to)
{
    const QString normalizedFrom = from.trimmed();
    const QString normalizedTo = to.trimmed();

    if (normalizedFrom.isEmpty() || normalizedTo.isEmpty()
        || (normalizedFrom == normalizedTo))
    {
        return false;
    }

    for (const _tagDagDocumentEdge &edge : m_edges)
    {
        if ((edge.from == normalizedFrom) && (edge.to == normalizedTo))
        {
            return false;
        }
    }

    _tagDagDocumentEdge edge;
    edge.from = normalizedFrom;
    edge.to = normalizedTo;
    m_edges.append(edge);
    return true;
}

bool DagDocument::RemoveEdge(const QString &from, const QString &to)
{
    for (int index = 0; index < m_edges.size(); ++index)
    {
        const _tagDagDocumentEdge &edge = m_edges.at(index);

        if ((edge.from == from) && (edge.to == to))
        {
            m_edges.removeAt(index);
            return true;
        }
    }

    return false;
}

double DagDocument::GetScrollX() const
{
    return m_scrollX;
}

double DagDocument::GetScrollY() const
{
    return m_scrollY;
}

double DagDocument::GetZoom() const
{
    return m_zoom;
}

void DagDocument::SetViewport(double scrollX, double scrollY, double zoom)
{
    m_scrollX = scrollX;
    m_scrollY = scrollY;
    m_zoom = (zoom > 0.0) ? zoom : 1.0;
}

quint64 DagDocument::GetRevision() const
{
    return m_revision;
}

void DagDocument::SetRevision(quint64 revision)
{
    m_revision = revision;
}

} // namespace vpet
