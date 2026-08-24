#include "vpet/dageditor/agent_node_catalog.h"
#include "vpet/agent/agent_context_keys.h"

#include <QtTest>

using namespace vpet;
using namespace AgentContextKeys;

namespace
{

/**
 * @brief 运行时 RegisterDefaultNodeHandlers 注册的权威类型清单
 *
 * 与 agent_context_keys.h 的 NODE_TYPE_* 常量一一对应；运行时新增节点类型时
 * 必须同步更新此处与目录，否则一致性测试变红。
 */
QStringList BuildRegistryTypeList()
{
    return {NODE_TYPE_EMOTION_REWRITE,
            NODE_TYPE_LLM_CHAT,
            NODE_TYPE_MEMORY_RETRIEVE,
            NODE_TYPE_MEMORY_STORE,
            NODE_TYPE_OUTPUT_FORMAT,
            NODE_TYPE_PROACTIVE_TOPIC,
            NODE_TYPE_USER_INPUT,
            NODE_TYPE_VISION_INPUT,
            NODE_TYPE_VISION_LLM,
            NODE_TYPE_WEB_RESEARCH};
}

} // namespace

class AgentNodeCatalogTest : public QObject
{
    Q_OBJECT

private slots:
    void DefaultInstanceCoversAllNodeTypes();
    void CatalogMatchesExampleConfigTypes();
    void SpecFieldsAreComplete();
    void RegisterRejectsInvalidSpecs();
    void FindReturnsCopy();

private:
    AgentNodeCatalog m_catalog; // 独立实例，避免依赖单例初始化顺序
};

/**
 * 目录集合 == 处理器集合（NODE_TYPE_* 常量清单）。
 */
void AgentNodeCatalogTest::DefaultInstanceCoversAllNodeTypes()
{
    QVERIFY(RegisterDefaultNodeSpecs(m_catalog));
    QCOMPARE(m_catalog.Types().size(), 10);

    QStringList expected = BuildRegistryTypeList();
    QStringList actual = m_catalog.Types();
    expected.sort();
    actual.sort();
    QCOMPARE(actual, expected);

    // 重复注册同一集合必须失败（幂等保护由 Contains 跳过实现）
    QVERIFY(RegisterDefaultNodeSpecs(m_catalog));
    QCOMPARE(m_catalog.Types().size(), 10);
}

/**
 * 示例配置出现的类型必须全部在目录中（示例配置是用户可见的基线）。
 */
void AgentNodeCatalogTest::CatalogMatchesExampleConfigTypes()
{
    QVERIFY(RegisterDefaultNodeSpecs(m_catalog));

    const QString examplePath = QStringLiteral(DAG_EDITOR_EXAMPLE_CONFIG_PATH);
    QFile exampleFile(examplePath);
    QVERIFY2(exampleFile.open(QIODevice::ReadOnly), qPrintable(examplePath));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(exampleFile.readAll(), &parseError);
    QVERIFY2(parseError.error == QJsonParseError::NoError, qPrintable(parseError.errorString()));

    QStringList exampleTypes;

    for (const QJsonValue &nodeValue : document.object().value(QStringLiteral("nodes")).toArray())
    {
        exampleTypes.append(nodeValue.toObject().value(QStringLiteral("type")).toString());
    }

    exampleTypes.removeDuplicates();

    for (const QString &type : exampleTypes)
    {
        QVERIFY2(m_catalog.Contains(type),
                 qPrintable(QStringLiteral("example type missing in catalog: %1").arg(type)));
    }

    // 触发源节点必须被标记为 source。
    DagNodeSpec spec;
    QVERIFY(m_catalog.Find(NODE_TYPE_USER_INPUT, spec));
    QVERIFY(spec.isSource);
    QVERIFY(m_catalog.Find(NODE_TYPE_VISION_INPUT, spec));
    QVERIFY(spec.isSource);
}

/**
 * 每个 spec 的展示字段必须完整：显示名、分类、颜色、描述；Enum 输入必有选项；
 * 数值输入上下界不颠倒。
 */
void AgentNodeCatalogTest::SpecFieldsAreComplete()
{
    QVERIFY(RegisterDefaultNodeSpecs(m_catalog));

    for (const DagNodeSpec &spec : m_catalog.All())
    {
        QVERIFY2(!spec.displayName.trimmed().isEmpty(),
                 qPrintable(spec.type + QStringLiteral(" displayName")));
        QVERIFY2(!spec.category.trimmed().isEmpty(),
                 qPrintable(spec.type + QStringLiteral(" category")));
        QVERIFY2(!spec.description.trimmed().isEmpty(),
                 qPrintable(spec.type + QStringLiteral(" description")));
        QVERIFY2(spec.accentColor.isValid(),
                 qPrintable(spec.type + QStringLiteral(" accentColor")));

        QSet<QString> inputKeys;

        for (const DagInputSpec &input : spec.inputs)
        {
            QVERIFY2(!input.label.trimmed().isEmpty(),
                     qPrintable(spec.type + QStringLiteral("/") + input.key + QStringLiteral(" label")));
            QVERIFY2(!inputKeys.contains(input.key),
                     qPrintable(spec.type + QStringLiteral(" duplicate input ") + input.key));
            inputKeys.insert(input.key);

            if (input.widget == DagWidgetType::Enum)
            {
                QVERIFY2(!input.enumValues.isEmpty(),
                         qPrintable(spec.type + QStringLiteral("/") + input.key
                                    + QStringLiteral(" enum values")));
            }

            if (input.minValue.isValid() && input.maxValue.isValid())
            {
                QVERIFY2(input.minValue.toDouble() <= input.maxValue.toDouble(),
                         qPrintable(spec.type + QStringLiteral("/") + input.key
                                    + QStringLiteral(" bounds")));
            }
        }

        // 关键 config 键抽查：schema 必须覆盖各节点实现实际读取的键。
        if (spec.type == NODE_TYPE_LLM_CHAT)
        {
            for (const char *key : {"temperature", "top_p", "max_tokens", "stream"})
            {
                QVERIFY2(inputKeys.contains(QLatin1String(key)),
                         qPrintable(spec.type + QStringLiteral(" missing ") + key));
            }
        }
        else if (spec.type == NODE_TYPE_WEB_RESEARCH)
        {
            for (const char *key : {"mode", "failure_policy", "max_search_rounds",
                                    "max_queries_per_round", "max_total_results",
                                    "max_context_chars", "total_deadline_ms", "engines"})
            {
                QVERIFY2(inputKeys.contains(QLatin1String(key)),
                         qPrintable(spec.type + QStringLiteral(" missing ") + key));
            }
        }
        else if (spec.type == NODE_TYPE_VISION_LLM)
        {
            for (const char *key : {"prompt", "detail", "max_tokens"})
            {
                QVERIFY2(inputKeys.contains(QLatin1String(key)),
                         qPrintable(spec.type + QStringLiteral(" missing ") + key));
            }
        }
        else if (spec.type == NODE_TYPE_PROACTIVE_TOPIC)
        {
            for (const char *key : {"enabled", "instruction", "min_interval_ms",
                                    "dedup_window_ms"})
            {
                QVERIFY2(inputKeys.contains(QLatin1String(key)),
                         qPrintable(spec.type + QStringLiteral(" missing ") + key));
            }
        }
    }
}

/**
 * 非法 spec 必须被拒绝：空类型、重复类型、无选项 Enum、上下界颠倒。
 */
void AgentNodeCatalogTest::RegisterRejectsInvalidSpecs()
{
    AgentNodeCatalog catalog;
    DagNodeSpec spec;
    QVERIFY(!catalog.Register(spec)); // 空 type

    spec.type = NODE_TYPE_USER_INPUT;
    spec.displayName = QStringLiteral("用户输入");
    QVERIFY(catalog.Register(spec));

    DagNodeSpec duplicate = spec;
    QVERIFY(!catalog.Register(duplicate)); // 重复 type

    DagNodeSpec badEnum = spec;
    badEnum.type = NODE_TYPE_LLM_CHAT;
    DagInputSpec input;
    input.key = QStringLiteral("detail");
    input.widget = DagWidgetType::Enum;
    badEnum.inputs.append(input);
    QVERIFY(!catalog.Register(badEnum)); // Enum 无选项

    DagNodeSpec badBounds = spec;
    badBounds.type = NODE_TYPE_WEB_RESEARCH;
    DagInputSpec rangeInput;
    rangeInput.key = QStringLiteral("max_tokens");
    rangeInput.minValue = 10;
    rangeInput.maxValue = 1;
    badBounds.inputs.append(rangeInput);
    QVERIFY(!catalog.Register(badBounds)); // 上下界颠倒

    QCOMPARE(catalog.Types().size(), 1);
}

void AgentNodeCatalogTest::FindReturnsCopy()
{
    QVERIFY(RegisterDefaultNodeSpecs(m_catalog));
    DagNodeSpec spec;
    QVERIFY(m_catalog.Find(NODE_TYPE_LLM_CHAT, spec));
    QCOMPARE(spec.type, NODE_TYPE_LLM_CHAT);

    spec.displayName = QStringLiteral("modified");
    DagNodeSpec reloaded;
    QVERIFY(m_catalog.Find(NODE_TYPE_LLM_CHAT, reloaded));
    QVERIFY(reloaded.displayName != QStringLiteral("modified"));

    QVERIFY(!m_catalog.Find(QStringLiteral("not.a.type"), reloaded));
    QVERIFY(!m_catalog.Contains(QString()));
}

QTEST_MAIN(AgentNodeCatalogTest)

#include "agent_node_catalog_test.moc"
