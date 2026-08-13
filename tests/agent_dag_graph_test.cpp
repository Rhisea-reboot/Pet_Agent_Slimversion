#include "vpet/agent/agent_dag_graph.h"

#include <QtTest>

namespace
{

QByteArray BuildGraphJson(const QByteArray &nodes, const QByteArray &edges)
{
    return QByteArrayLiteral("{\"nodes\":") + nodes
        + QByteArrayLiteral(",\"edges\":") + edges + QByteArrayLiteral("}");
}

} // namespace

class AgentDagGraphTest : public QObject
{
    Q_OBJECT

private slots:
    void QueriesBranchedGraph();
    void KeepsStableDeclarationOrder();
    void RejectsEmptyGraph();
    void RejectsCycle();
    void RejectsDuplicateEdge();
    void ClearsOutputsForUnknownNode();
};

void AgentDagGraphTest::QueriesBranchedGraph()
{
    const QByteArray jsonData = BuildGraphJson(
        QByteArrayLiteral(
            R"([{"id":"source_b","type":"source"},{"id":"source_a","type":"source"},{"id":"left","type":"worker"},{"id":"right","type":"worker"},{"id":"join","type":"merge"}])"),
        QByteArrayLiteral(
            R"([{"from":"source_b","to":"left"},{"from":"source_b","to":"right"},{"from":"source_a","to":"right"},{"from":"left","to":"join"},{"from":"right","to":"join"}])"));

    vpet::AgentDagGraph graph;
    QString errorMessage;
    QVERIFY2(graph.LoadFromJsonData(jsonData, errorMessage), qPrintable(errorMessage));

    QCOMPARE(graph.GetSourceNodes(),
             QVector<QString>({QStringLiteral("source_b"), QStringLiteral("source_a")}));

    QVector<QString> successors;
    QVERIFY(graph.GetSuccessors(QStringLiteral("source_b"), successors));
    QCOMPARE(successors,
             QVector<QString>({QStringLiteral("left"), QStringLiteral("right")}));

    QVector<QString> predecessors;
    QVERIFY(graph.GetPredecessors(QStringLiteral("right"), predecessors));
    QCOMPARE(predecessors,
             QVector<QString>({QStringLiteral("source_b"), QStringLiteral("source_a")}));

    int inDegree = -1;
    QVERIFY(graph.GetInDegree(QStringLiteral("join"), inDegree));
    QCOMPARE(inDegree, 2);

    const QHash<QString, int> inDegreeMap = graph.GetInDegreeMap();
    QCOMPARE(inDegreeMap.size(), 5);
    QCOMPARE(inDegreeMap.value(QStringLiteral("source_b")), 0);
    QCOMPARE(inDegreeMap.value(QStringLiteral("right")), 2);
    QCOMPARE(inDegreeMap.value(QStringLiteral("join")), 2);
}

void AgentDagGraphTest::KeepsStableDeclarationOrder()
{
    const QByteArray jsonData = BuildGraphJson(
        QByteArrayLiteral(R"(["source_b","source_a","child_b","child_a","join"] )"),
        QByteArrayLiteral(
            R"([{"from":"source_b","to":"child_b"},{"from":"source_a","to":"child_a"},{"from":"child_b","to":"join"},{"from":"child_a","to":"join"}])"));

    vpet::AgentDagGraph graph;
    QString errorMessage;
    QVERIFY2(graph.LoadFromJsonData(jsonData, errorMessage), qPrintable(errorMessage));

    QVector<QString> order;
    QVERIFY2(graph.TopologicalSort(order, errorMessage), qPrintable(errorMessage));
    QCOMPARE(order,
             QVector<QString>({QStringLiteral("source_b"),
                               QStringLiteral("source_a"),
                               QStringLiteral("child_b"),
                               QStringLiteral("child_a"),
                               QStringLiteral("join")}));
}

void AgentDagGraphTest::RejectsEmptyGraph()
{
    vpet::AgentDagGraph graph;
    QString errorMessage;
    QVERIFY(!graph.LoadFromJsonData(BuildGraphJson(QByteArrayLiteral("[]"),
                                                   QByteArrayLiteral("[]")),
                                    errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("empty"), Qt::CaseInsensitive));
}

void AgentDagGraphTest::RejectsCycle()
{
    const QByteArray jsonData = BuildGraphJson(
        QByteArrayLiteral(R"(["a","b","c"])"),
        QByteArrayLiteral(
            R"([{"from":"a","to":"b"},{"from":"b","to":"c"},{"from":"c","to":"a"}])"));

    vpet::AgentDagGraph graph;
    QString errorMessage;
    QVERIFY(!graph.LoadFromJsonData(jsonData, errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("cycle"), Qt::CaseInsensitive));
    QVERIFY(graph.IsEmpty());
}

void AgentDagGraphTest::RejectsDuplicateEdge()
{
    const QByteArray jsonData = BuildGraphJson(
        QByteArrayLiteral(R"(["a","b"])"),
        QByteArrayLiteral(
            R"([{"from":"a","to":"b"},{"from":"a","to":"b"}])"));

    vpet::AgentDagGraph graph;
    QString errorMessage;
    QVERIFY(!graph.LoadFromJsonData(jsonData, errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("duplicate edge"), Qt::CaseInsensitive));
    QVERIFY(graph.IsEmpty());
}

void AgentDagGraphTest::ClearsOutputsForUnknownNode()
{
    const QByteArray jsonData = BuildGraphJson(
        QByteArrayLiteral(R"(["a","b"])"),
        QByteArrayLiteral(R"([{"from":"a","to":"b"}])"));

    vpet::AgentDagGraph graph;
    QString errorMessage;
    QVERIFY2(graph.LoadFromJsonData(jsonData, errorMessage), qPrintable(errorMessage));

    QVector<QString> nodes({QStringLiteral("stale")});
    QVERIFY(!graph.GetSuccessors(QStringLiteral("missing"), nodes));
    QVERIFY(nodes.isEmpty());

    nodes.append(QStringLiteral("stale"));
    QVERIFY(!graph.GetPredecessors(QStringLiteral("missing"), nodes));
    QVERIFY(nodes.isEmpty());

    int inDegree = -1;
    QVERIFY(!graph.GetInDegree(QStringLiteral("missing"), inDegree));
    QCOMPARE(inDegree, 0);
}

QTEST_APPLESS_MAIN(AgentDagGraphTest)

#include "agent_dag_graph_test.moc"
