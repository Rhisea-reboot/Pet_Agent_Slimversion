#include "vpet/agent/agent_graph_executor.h"
#include "vpet/agent/agent_context_keys.h"
#include "vpet/agent/agent_runtime.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVariant>
#include <QtTest>

using namespace vpet;
using namespace AgentContextKeys;

namespace
{

/**
 * @brief 两套可区分的图：slow 节点携带不同 tag，用于断言旧图/新图语义
 */
QByteArray BuildGraphJson(const QString &tag)
{
    return QStringLiteral(
               "{\"nodes\":["
               "{\"id\":\"src\",\"type\":\"user.input\",\"config\":{\"trigger\":\"user\"}},"
               "{\"id\":\"slow\",\"type\":\"llm.chat\",\"config\":{\"tag\":\"%1\","
               "\"async_timeout_ms\":60000}},"
               "{\"id\":\"sink\",\"type\":\"output.format\",\"config\":{}}"
               "],\"edges\":["
               "{\"from\":\"src\",\"to\":\"slow\"},{\"from\":\"slow\",\"to\":\"sink\"}"
               "]}")
        .arg(tag)
        .toUtf8();
}

bool WriteFile(const QString &path, const QByteArray &content)
{
    QFile file(path);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        return false;
    }

    return file.write(content) == content.size();
}

/**
 * @brief 构造完整的测试回调集合
 *
 * executeNode 把节点 config.tag 记录到 executedTags；当 asyncEnabled 为真且节点是
 * slow 时置为异步挂起，模拟 llm.chat 等待回调。
 */
AgentGraphExecutor::_tagCallbacks MakeCallbacks(QStringList &executedTags,
                                                int &pendingCount,
                                                bool &completedFlag,
                                                const bool &asyncEnabled)
{
    AgentGraphExecutor::_tagCallbacks callbacks;
    callbacks.prepareInput = [](AgentContext &, QString &)
    {
        return true;
    };
    callbacks.executeNode = [&executedTags, &pendingCount, &asyncEnabled](
                                const _tagAgentDagNode &node,
                                AgentContext &context,
                                QString &)
    {
        executedTags.append(node.config.value(QStringLiteral("tag")).toString());

        if (asyncEnabled && (node.id == QStringLiteral("slow")))
        {
            context.SetValue(RUNTIME_PENDING, true);
            ++pendingCount;
        }

        return true;
    };
    callbacks.registerPending = [](const _tagAgentDagNode &,
                                   const AgentContext &,
                                   quint64,
                                   const QString &,
                                   QString &)
    {
        return true;
    };
    callbacks.hasPending = [&pendingCount]()
    {
        return pendingCount > 0;
    };
    callbacks.clearInput = [](AgentContext &) {};
    callbacks.resetAfterFailure = [](AgentContext &) {};
    callbacks.invocationCompleted = [&completedFlag](AgentContext &)
    {
        completedFlag = true;
    };
    return callbacks;
}

} // namespace

class DagHotReloadTest : public QObject
{
    Q_OBJECT

private slots:
    void IdleReplaceAppliesImmediately();
    void BusyInvocationDefersAndKeepsOldGraph();
    void BrokenGraphIsRejectedAndOldGraphSurvives();

private:
    void WriteGraphs(const QString &caseName);
    QTemporaryDir m_temporaryDirectory;
};

void DagHotReloadTest::WriteGraphs(const QString &caseName)
{
    QVERIFY(m_temporaryDirectory.isValid());
    QDir(m_temporaryDirectory.path()).mkpath(caseName);
    const QString directory = m_temporaryDirectory.filePath(caseName);
    QVERIFY(WriteFile(directory + QStringLiteral("/graph_a.json"),
                      BuildGraphJson(QStringLiteral("A"))));
    QVERIFY(WriteFile(directory + QStringLiteral("/graph_b.json"),
                      BuildGraphJson(QStringLiteral("B"))));
    QVERIFY(WriteFile(directory + QStringLiteral("/broken.json"),
                      QByteArrayLiteral("{\"nodes\":[{\"id\":123}],\"edges\":}")));
}

/**
 * 空闲时换图立即生效。
 */
void DagHotReloadTest::IdleReplaceAppliesImmediately()
{
    WriteGraphs(QStringLiteral("idle"));

    AgentGraphExecutor executor;
    QString errorMessage;
    QVERIFY2(executor.Load(m_temporaryDirectory.filePath(
                QStringLiteral("idle/graph_a.json")), errorMessage),
             qPrintable(errorMessage));

    std::shared_ptr<const AgentDagGraph> snapshotB;
    QVector<QString> orderB;

    QVERIFY2(AgentGraphExecutor::BuildSnapshotFromFile(m_temporaryDirectory.filePath(
                    QStringLiteral("idle/graph_b.json")),
                snapshotB,
                orderB,
                errorMessage),
             qPrintable(errorMessage));

    QCOMPARE(executor.ReplaceGraphIfIdle(snapshotB, false),
             AgentGraphExecutor::ReplaceGraphResult::Applied);
    QCOMPARE(executor.GetExecutionOrder(), orderB);

    // 空快照必须被拒绝。
    QCOMPARE(executor.ReplaceGraphIfIdle(nullptr, false),
             AgentGraphExecutor::ReplaceGraphResult::RejectedInvalid);
}

/**
 * 忙碌（invocation 进行中且存在异步挂起）期间换图被延迟；旧 invocation 全程使用
 * 旧图执行；收尾后再次尝试换图立即生效，新一轮使用新图。
 */
void DagHotReloadTest::BusyInvocationDefersAndKeepsOldGraph()
{
    WriteGraphs(QStringLiteral("busy"));
    const QString graphAPath = m_temporaryDirectory.filePath(QStringLiteral("busy/graph_a.json"));
    const QString graphBPath = m_temporaryDirectory.filePath(QStringLiteral("busy/graph_b.json"));

    bool asyncEnabled = true;
    QStringList executedTags;
    int pendingCount = 0;
    bool completed = false;
    const auto callbacks = MakeCallbacks(executedTags, pendingCount, completed, asyncEnabled);

    AgentGraphExecutor executor;
    QString errorMessage;
    QVERIFY2(executor.Load(graphAPath, errorMessage), qPrintable(errorMessage));

    AgentContext invocationInput;
    invocationInput.SetValue(RUNTIME_TRIGGER_TYPE, QStringLiteral("user"));

    QVERIFY2(executor.BeginInvocation(invocationInput, false, errorMessage),
             qPrintable(errorMessage));

    AgentContext facadeContext;
    AgentContext sessionContext;

    // slow 节点进入异步挂起，本轮停在半途。
    QVERIFY2(executor.PumpReadyQueue(true,
                                     facadeContext,
                                     sessionContext,
                                     callbacks,
                                     errorMessage),
             qPrintable(errorMessage));
    QVERIFY(executor.IsActive());
    QCOMPARE(pendingCount, 1);
    QCOMPARE(executedTags.size(), 2);          // src（无 tag）+ slow
    QVERIFY(executedTags.contains(QStringLiteral("A")));

    std::shared_ptr<const AgentDagGraph> snapshotB;
    QVector<QString> orderB;

    QVERIFY2(AgentGraphExecutor::BuildSnapshotFromFile(graphBPath, snapshotB, orderB, errorMessage),
             qPrintable(errorMessage));

    // 有异步挂起：换图必须被延迟；即便报告“无 pending”也因 invocation 活跃而延迟。
    QCOMPARE(executor.ReplaceGraphIfIdle(snapshotB, true),
             AgentGraphExecutor::ReplaceGraphResult::DeferredBusy);
    QCOMPARE(executor.ReplaceGraphIfIdle(snapshotB, false),
             AgentGraphExecutor::ReplaceGraphResult::DeferredBusy);

    // 旧图完成本轮：恢复 slow → sink 全部在旧图上执行。
    asyncEnabled = false;
    pendingCount = 0;
    AgentContext resumedContext;
    resumedContext.SetValue(RUNTIME_PENDING_REQUEST_ID, 1);
    resumedContext.RemoveValue(RUNTIME_PENDING);

    const quint64 activeInvocationId = executor.GetLastCompletedInvocationId() + 1;
    QVERIFY2(executor.ResumePendingNode(QStringLiteral("slow"),
                                        activeInvocationId,
                                        resumedContext,
                                        facadeContext,
                                        sessionContext,
                                        callbacks,
                                        errorMessage),
             qPrintable(errorMessage));
    QVERIFY(completed);
    QVERIFY(!executor.IsActive());
    QVERIFY(executedTags.contains(QStringLiteral("A"))); // 本轮全程在旧图上执行
    QVERIFY(!executedTags.contains(QStringLiteral("B")));

    // 收尾后换图立即生效。
    QCOMPARE(executor.ReplaceGraphIfIdle(snapshotB, false),
             AgentGraphExecutor::ReplaceGraphResult::Applied);

    // 新一轮完整跑在新图上。
    QVERIFY2(executor.BeginInvocation(invocationInput, false, errorMessage),
             qPrintable(errorMessage));

    executedTags.clear();
    completed = false;
    AgentContext secondFacade;
    QVERIFY2(executor.PumpReadyQueue(true,
                                     secondFacade,
                                     sessionContext,
                                     callbacks,
                                     errorMessage),
             qPrintable(errorMessage));
    QVERIFY(completed);
    QVERIFY(!executedTags.contains(QStringLiteral("A")));
    QVERIFY(executedTags.contains(QStringLiteral("B")));
}

/**
 * 坏图被拒绝且旧图继续可用；通过 AgentRuntime 的公开 API 验证同样成立。
 */
void DagHotReloadTest::BrokenGraphIsRejectedAndOldGraphSurvives()
{
    WriteGraphs(QStringLiteral("runtime"));
    const QString graphAPath = m_temporaryDirectory.filePath(QStringLiteral("runtime/graph_a.json"));
    const QString graphBPath = m_temporaryDirectory.filePath(QStringLiteral("runtime/graph_b.json"));
    const QString brokenPath = m_temporaryDirectory.filePath(QStringLiteral("runtime/broken.json"));

    AgentRuntime runtime;
    QString errorMessage;

    QVERIFY2(runtime.Load(graphAPath, errorMessage), qPrintable(errorMessage));
    const QVector<QString> originalOrder = runtime.GetExecutionOrder();
    QVERIFY(!originalOrder.isEmpty());

    QSignalSpy reloadedSpy(&runtime, &AgentRuntime::DagGraphReloaded);
    QSignalSpy failedSpy(&runtime, &AgentRuntime::DagGraphReloadFailed);

    AgentRuntime::DagReloadOutcome outcome = AgentRuntime::DagReloadOutcome::Applied;
    QVERIFY(!runtime.RequestDagReload(brokenPath, outcome, errorMessage));
    QCOMPARE(outcome, AgentRuntime::DagReloadOutcome::Failed);
    QCOMPARE(runtime.GetExecutionOrder(), originalOrder); // 旧图原封不动
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(reloadedSpy.count(), 0);

    QVERIFY2(runtime.RequestDagReload(graphBPath, outcome, errorMessage),
             qPrintable(errorMessage));
    QCOMPARE(outcome, AgentRuntime::DagReloadOutcome::Applied);
    QCOMPARE(reloadedSpy.count(), 1);
    QCOMPARE(reloadedSpy.at(0).at(1).toBool(), true); // applied = 立即生效

    QVERIFY(runtime.GetDagConfigPath().endsWith(QStringLiteral("graph_b.json")));
}

QTEST_MAIN(DagHotReloadTest)

#include "dag_hot_reload_test.moc"
