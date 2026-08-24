#include "vpet/dageditor/agent_dag_validator.h"

#include <QtTest>

using namespace vpet;

namespace
{

/**
 * @brief 构造最小合法节点
 */
_tagDagDocumentNode MakeNode(const QString &id, const QString &type, const QJsonObject &config = {})
{
    _tagDagDocumentNode node;
    node.id = id;
    node.type = type;
    node.config = config;
    return node;
}

_tagDagDocumentEdge MakeEdge(const QString &from, const QString &to)
{
    _tagDagDocumentEdge edge;
    edge.from = from;
    edge.to = to;
    return edge;
}

/**
 * @brief 在问题列表中查找指定 code（可限定节点）
 */
bool HasIssue(const QVector<DagValidationIssue> &issues,
              const QString &code,
              DagValidationSeverity severity,
              const QString &nodeId = QString())
{
    for (const DagValidationIssue &issue : issues)
    {
        if ((issue.code == code) && (issue.severity == severity)
            && (nodeId.isEmpty() || (issue.nodeId == nodeId)))
        {
            return true;
        }
    }

    return false;
}

} // namespace

class DagValidatorTest : public QObject
{
    Q_OBJECT

private slots:
    void DefaultGraphPassesWithoutErrors();
    void DetectsUnknownType();
    void DetectsDuplicateNodeId();
    void DetectsSelfLoopDuplicateAndDanglingEdges();
    void DetectsCycle();
    void DetectsEmptyGraph();
    void DetectsConfigValueProblems();
    void WarnsOnJoinWithoutMergePolicy();
    void WarnsOnOrphanNode();
    void WarnsOnTriggerForNonSourceNode();

private:
    AgentNodeCatalog m_catalog;
    AgentDagValidator m_validator{&m_catalog};
};

void DagValidatorTest::DefaultGraphPassesWithoutErrors()
{
    QVERIFY(RegisterDefaultNodeSpecs(m_catalog));

    QFile exampleFile(QStringLiteral(DAG_EDITOR_EXAMPLE_CONFIG_PATH));
    QVERIFY2(exampleFile.open(QIODevice::ReadOnly),
             "example config must exist for validator test");

    DagDocument document;
    QString errorMessage;

    QVERIFY2(DagDocument::FromJsonData(exampleFile.readAll(), document, errorMessage),
             qPrintable(errorMessage));

    // 默认图允许出现 warning（join 无 merge 策略），但不得有 error。
    const QVector<DagValidationIssue> issues = m_validator.Validate(document);

    for (const DagValidationIssue &issue : issues)
    {
        QVERIFY2(issue.severity != DagValidationSeverity::Error,
                 qPrintable(QStringLiteral("%1: %2").arg(issue.code, issue.message)));
    }
}

void DagValidatorTest::DetectsUnknownType()
{
    QVERIFY(RegisterDefaultNodeSpecs(m_catalog));

    DagDocument document;
    document.SetNodes({MakeNode(QStringLiteral("a"), QStringLiteral("not.a.type"))});
    document.SetEdges({});

    const QVector<DagValidationIssue> issues = m_validator.Validate(document);
    QVERIFY(HasIssue(issues, QStringLiteral("unknown_type"), DagValidationSeverity::Error,
                     QStringLiteral("a")));
    QVERIFY(AgentDagValidator::HasErrors(issues));
}

void DagValidatorTest::DetectsDuplicateNodeId()
{
    QVERIFY(RegisterDefaultNodeSpecs(m_catalog));

    DagDocument document;
    document.SetNodes({MakeNode(QStringLiteral("dup"), QStringLiteral("user.input")),
                       MakeNode(QStringLiteral("dup"), QStringLiteral("llm.chat"))});

    const QVector<DagValidationIssue> issues = m_validator.Validate(document);
    QVERIFY(HasIssue(issues, QStringLiteral("duplicate_node_id"), DagValidationSeverity::Error,
                     QStringLiteral("dup")));
}

void DagValidatorTest::DetectsSelfLoopDuplicateAndDanglingEdges()
{
    QVERIFY(RegisterDefaultNodeSpecs(m_catalog));

    DagDocument document;
    document.SetNodes({MakeNode(QStringLiteral("a"), QStringLiteral("user.input"))});
    document.SetEdges({MakeEdge(QStringLiteral("a"), QStringLiteral("a")),
                       MakeEdge(QStringLiteral("a"), QStringLiteral("ghost")),
                       MakeEdge(QStringLiteral("phantom"), QStringLiteral("a"))});

    const QVector<DagValidationIssue> issues = m_validator.Validate(document);
    QVERIFY(HasIssue(issues, QStringLiteral("self_loop"), DagValidationSeverity::Error,
                     QStringLiteral("a")));
    QVERIFY(HasIssue(issues, QStringLiteral("edge_references_missing_node"),
                     DagValidationSeverity::Error, QStringLiteral("ghost")));
    QVERIFY(HasIssue(issues, QStringLiteral("edge_references_missing_node"),
                     DagValidationSeverity::Error, QStringLiteral("phantom")));

    DagDocument duplicateEdgeDocument;
    duplicateEdgeDocument.SetNodes(
        {MakeNode(QStringLiteral("a"), QStringLiteral("user.input")),
         MakeNode(QStringLiteral("b"), QStringLiteral("llm.chat"))});
    duplicateEdgeDocument.SetEdges({MakeEdge(QStringLiteral("a"), QStringLiteral("b")),
                                    MakeEdge(QStringLiteral("a"), QStringLiteral("b"))});

    const QVector<DagValidationIssue> duplicateIssues =
        m_validator.Validate(duplicateEdgeDocument);
    QVERIFY(HasIssue(duplicateIssues, QStringLiteral("duplicate_edge"),
                     DagValidationSeverity::Error, QStringLiteral("a")));
}

void DagValidatorTest::DetectsCycle()
{
    QVERIFY(RegisterDefaultNodeSpecs(m_catalog));

    DagDocument document;
    document.SetNodes({MakeNode(QStringLiteral("a"), QStringLiteral("user.input")),
                       MakeNode(QStringLiteral("b"), QStringLiteral("llm.chat")),
                       MakeNode(QStringLiteral("c"), QStringLiteral("output.format"))});
    document.SetEdges({MakeEdge(QStringLiteral("a"), QStringLiteral("b")),
                       MakeEdge(QStringLiteral("b"), QStringLiteral("c")),
                       MakeEdge(QStringLiteral("c"), QStringLiteral("b"))});

    const QVector<DagValidationIssue> issues = m_validator.Validate(document);
    QVERIFY(HasIssue(issues, QStringLiteral("cycle_detected"), DagValidationSeverity::Error));
}

void DagValidatorTest::DetectsEmptyGraph()
{
    QVERIFY(RegisterDefaultNodeSpecs(m_catalog));

    DagDocument document;
    const QVector<DagValidationIssue> issues = m_validator.Validate(document);
    QVERIFY(HasIssue(issues, QStringLiteral("empty_graph"), DagValidationSeverity::Error));

    // 单个孤立源节点是合法图。
    DagDocument single;
    single.SetNodes({MakeNode(QStringLiteral("a"), QStringLiteral("user.input"))});
    QVERIFY(!AgentDagValidator::HasErrors(m_validator.Validate(single)));
}

void DagValidatorTest::DetectsConfigValueProblems()
{
    QVERIFY(RegisterDefaultNodeSpecs(m_catalog));

    QJsonObject badConfig;
    badConfig.insert(QStringLiteral("temperature"), 5.0);      // 越界
    badConfig.insert(QStringLiteral("stream"), QStringLiteral("yes")); // 类型错误

    DagDocument document;
    document.SetNodes({MakeNode(QStringLiteral("chat"), QStringLiteral("llm.chat"), badConfig)});

    QVector<DagValidationIssue> issues = m_validator.Validate(document);
    QVERIFY(HasIssue(issues, QStringLiteral("value_out_of_range"), DagValidationSeverity::Error));
    QVERIFY(HasIssue(issues, QStringLiteral("invalid_value_type"), DagValidationSeverity::Error));

    QJsonObject enumConfig;
    enumConfig.insert(QStringLiteral("detail"), QStringLiteral("ultra"));

    DagDocument visionDocument;
    visionDocument.SetNodes(
        {MakeNode(QStringLiteral("vision"), QStringLiteral("vision.llm"), enumConfig)});
    issues = m_validator.Validate(visionDocument);
    QVERIFY(HasIssue(issues, QStringLiteral("invalid_enum_value"), DagValidationSeverity::Error));
}

void DagValidatorTest::WarnsOnJoinWithoutMergePolicy()
{
    QVERIFY(RegisterDefaultNodeSpecs(m_catalog));

    DagDocument document;
    document.SetNodes({MakeNode(QStringLiteral("u"), QStringLiteral("user.input")),
                       MakeNode(QStringLiteral("v"), QStringLiteral("vision.input")),
                       MakeNode(QStringLiteral("join"), QStringLiteral("llm.chat"))});
    document.SetEdges({MakeEdge(QStringLiteral("u"), QStringLiteral("join")),
                       MakeEdge(QStringLiteral("v"), QStringLiteral("join"))});

    const QVector<DagValidationIssue> issues = m_validator.Validate(document);
    QVERIFY(!AgentDagValidator::HasErrors(issues));
    QVERIFY(HasIssue(issues, QStringLiteral("join_without_merge_policy"),
                     DagValidationSeverity::Warning, QStringLiteral("join")));

    // 声明 merge 策略后提示消失。
    _tagDagDocumentNode joinWithMerge = MakeNode(QStringLiteral("join"), QStringLiteral("llm.chat"));
    joinWithMerge.config.insert(QStringLiteral("merge"), QJsonObject());
    document.SetNodes({MakeNode(QStringLiteral("u"), QStringLiteral("user.input")),
                       MakeNode(QStringLiteral("v"), QStringLiteral("vision.input")),
                       joinWithMerge});
    QVERIFY(!HasIssue(m_validator.Validate(document),
                      QStringLiteral("join_without_merge_policy"),
                      DagValidationSeverity::Warning));
}

void DagValidatorTest::WarnsOnOrphanNode()
{
    QVERIFY(RegisterDefaultNodeSpecs(m_catalog));

    DagDocument clean;
    clean.SetNodes({MakeNode(QStringLiteral("a"), QStringLiteral("user.input")),
                    MakeNode(QStringLiteral("b"), QStringLiteral("llm.chat")),
                    MakeNode(QStringLiteral("lonely"), QStringLiteral("memory.retrieve"))});
    clean.SetEdges({MakeEdge(QStringLiteral("a"), QStringLiteral("b"))});

    const QVector<DagValidationIssue> issues = m_validator.Validate(clean);
    QVERIFY(HasIssue(issues, QStringLiteral("orphan_node"), DagValidationSeverity::Warning,
                     QStringLiteral("lonely")));
}

void DagValidatorTest::WarnsOnTriggerForNonSourceNode()
{
    QVERIFY(RegisterDefaultNodeSpecs(m_catalog));

    QJsonObject triggerConfig;
    triggerConfig.insert(QStringLiteral("trigger"), QStringLiteral("user"));

    DagDocument document;
    document.SetNodes({MakeNode(QStringLiteral("chat"), QStringLiteral("llm.chat"), triggerConfig)});

    const QVector<DagValidationIssue> issues = m_validator.Validate(document);
    QVERIFY(HasIssue(issues, QStringLiteral("trigger_on_non_source"),
                     DagValidationSeverity::Warning, QStringLiteral("chat")));
    // trigger 声明本身不构成 error。
    QVERIFY(!AgentDagValidator::HasErrors(issues));
}

QTEST_MAIN(DagValidatorTest)

#include "dag_validator_test.moc"
