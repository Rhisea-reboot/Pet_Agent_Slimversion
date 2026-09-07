#include "vpet/agent/agent_dag_graph.h"
#include "vpet/agent/agent_context.h"
#include "vpet/agent/agent_context_keys.h"

#include <QtTest>

namespace
{

QByteArray BuildGraphJson(const QByteArray &nodes, const QByteArray &edges)
{
    return QByteArrayLiteral("{\"nodes\":") + nodes
        + QByteArrayLiteral(",\"edges\":") + edges + QByteArrayLiteral("}");
}

} // namespace

class AgentContextTest : public QObject
{
    Q_OBJECT

private slots:
    void EmptyContextBehavior();
    void SetGetRemoveKeyContract();
    void GetKeysReturnsSortedKeys();
    void OverlayMergesAndOverwrites();
    void OverlaySelfNoOp();
    void BuildDeltaComputesAddedModifiedRemoved();
    void BuildDeltaAliasHandling();
    void SnapshotIsIsolated();
};

void AgentContextTest::EmptyContextBehavior()
{
    vpet::AgentContext context;
    QVERIFY(context.GetKeys().isEmpty());

    QVariant val;
    QVERIFY(!context.GetValue(QStringLiteral("none"), val));
    QVERIFY(val.isNull());
    QVERIFY(!context.Contains(QStringLiteral("none")));
    QVERIFY(!context.RemoveValue(QStringLiteral("none")));
}

void AgentContextTest::SetGetRemoveKeyContract()
{
    vpet::AgentContext context;

    // 非法或空 key 拒绝
    QVERIFY(!context.SetValue(QStringLiteral("   "), QStringLiteral("val")));
    QVERIFY(!context.SetValue(QStringLiteral("k"), QVariant()));

    // 存取并 trim key
    QVERIFY(context.SetValue(QStringLiteral("  test_key  "), QStringLiteral("value1")));
    QVERIFY(context.Contains(QStringLiteral("test_key")));
    QVERIFY(context.Contains(QStringLiteral("  test_key  ")));

    QVariant out;
    QVERIFY(context.GetValue(QStringLiteral("test_key"), out));
    QCOMPARE(out.toString(), QStringLiteral("value1"));

    QVERIFY(context.RemoveValue(QStringLiteral("  test_key ")));
    QVERIFY(!context.Contains(QStringLiteral("test_key")));
}

void AgentContextTest::GetKeysReturnsSortedKeys()
{
    vpet::AgentContext context;
    context.SetValue(QStringLiteral("zebra"), 1);
    context.SetValue(QStringLiteral("apple"), 2);
    context.SetValue(QStringLiteral("banana"), 3);

    const QStringList keys = context.GetKeys();
    QCOMPARE(keys, QStringList({QStringLiteral("apple"),
                               QStringLiteral("banana"),
                               QStringLiteral("zebra")}));
}

void AgentContextTest::OverlayMergesAndOverwrites()
{
    vpet::AgentContext base;
    base.SetValue(QStringLiteral("k1"), QStringLiteral("v1"));
    base.SetValue(QStringLiteral("k2"), QStringLiteral("v2_old"));

    vpet::AgentContext other;
    other.SetValue(QStringLiteral("k2"), QStringLiteral("v2_new"));
    other.SetValue(QStringLiteral("k3"), QStringLiteral("v3"));

    QVERIFY(base.Overlay(other));

    QVariant val;
    QVERIFY(base.GetValue(QStringLiteral("k1"), val));
    QCOMPARE(val.toString(), QStringLiteral("v1"));

    QVERIFY(base.GetValue(QStringLiteral("k2"), val));
    QCOMPARE(val.toString(), QStringLiteral("v2_new"));

    QVERIFY(base.GetValue(QStringLiteral("k3"), val));
    QCOMPARE(val.toString(), QStringLiteral("v3"));
}

void AgentContextTest::OverlaySelfNoOp()
{
    vpet::AgentContext context;
    context.SetValue(QStringLiteral("k1"), 100);
    context.SetValue(QStringLiteral("k2"), 200);

    QVERIFY(context.Overlay(context));

    QCOMPARE(context.GetKeys().size(), 2);
    QVariant val;
    QVERIFY(context.GetValue(QStringLiteral("k1"), val));
    QCOMPARE(val.toInt(), 100);
}

void AgentContextTest::BuildDeltaComputesAddedModifiedRemoved()
{
    vpet::AgentContext base;
    base.SetValue(QStringLiteral("same"), QStringLiteral("common"));
    base.SetValue(QStringLiteral("modified"), 10);
    base.SetValue(QStringLiteral("removed"), true);

    vpet::AgentContext current;
    current.SetValue(QStringLiteral("same"), QStringLiteral("common"));
    current.SetValue(QStringLiteral("modified"), 20);
    current.SetValue(QStringLiteral("added"), QStringLiteral("new_val"));

    vpet::AgentContext delta;
    QSet<QString> removedKeys;

    QVERIFY(current.BuildDelta(base, delta, removedKeys));

    QCOMPARE(removedKeys, QSet<QString>({QStringLiteral("removed")}));

    QVariant val;
    QVERIFY(!delta.GetValue(QStringLiteral("same"), val));
    QVERIFY(delta.GetValue(QStringLiteral("modified"), val));
    QCOMPARE(val.toInt(), 20);
    QVERIFY(delta.GetValue(QStringLiteral("added"), val));
    QCOMPARE(val.toString(), QStringLiteral("new_val"));
}

void AgentContextTest::BuildDeltaAliasHandling()
{
    // 测试 delta 与 this 别名：按 HEAD 原始行为，delta.Clear() 使得 this 被清空，因此遍历 baseKeys 时 Contains(key) 均为 false
    vpet::AgentContext base;
    base.SetValue(QStringLiteral("k1"), 1);
    base.SetValue(QStringLiteral("k2"), 2);

    vpet::AgentContext current;
    current.SetValue(QStringLiteral("k1"), 10);
    current.SetValue(QStringLiteral("k3"), 30);

    QSet<QString> removedKeys;
    QVERIFY(current.BuildDelta(base, current, removedKeys));

    QCOMPARE(removedKeys, QSet<QString>({QStringLiteral("k1"), QStringLiteral("k2")}));

    // 测试 delta 与 base 别名：base 被清空，baseKeys 为空，因此 removedKeys 为空
    vpet::AgentContext base2;
    base2.SetValue(QStringLiteral("k1"), 1);
    base2.SetValue(QStringLiteral("k2"), 2);

    vpet::AgentContext current2;
    current2.SetValue(QStringLiteral("k1"), 100);

    QSet<QString> removedKeys2;
    QVERIFY(current2.BuildDelta(base2, base2, removedKeys2));

    QVERIFY(removedKeys2.isEmpty());
    QVariant val;
    QVERIFY(base2.GetValue(QStringLiteral("k1"), val));
    QCOMPARE(val.toInt(), 100);
}

void AgentContextTest::SnapshotIsIsolated()
{
    vpet::AgentContext original;
    original.SetValue(QStringLiteral("k1"), QStringLiteral("initial"));

    vpet::AgentContext snapshot = original.Snapshot();
    snapshot.SetValue(QStringLiteral("k1"), QStringLiteral("mutated"));
    snapshot.SetValue(QStringLiteral("k2"), QStringLiteral("added"));

    QVariant val;
    QVERIFY(original.GetValue(QStringLiteral("k1"), val));
    QCOMPARE(val.toString(), QStringLiteral("initial"));
    QVERIFY(!original.Contains(QStringLiteral("k2")));
}

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

int main(int argc, char *argv[])
{
    int status = 0;

    {
        AgentDagGraphTest dagTest;
        status |= QTest::qExec(&dagTest, argc, argv);
    }

    {
        AgentContextTest contextTest;
        status |= QTest::qExec(&contextTest, argc, argv);
    }

    return status;
}

#include "agent_dag_graph_test.moc"
