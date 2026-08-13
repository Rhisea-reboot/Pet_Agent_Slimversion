#include "vpet/agent/agent_runtime.h"
#include "vpet/agent/agent_context_keys.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace
{

bool WriteDag(const QTemporaryDir &directory, QString &path)
{
    if (!directory.isValid())
    {
        return false;
    }

    path = directory.filePath(QStringLiteral("integration_dag.json"));
    QFile file(path);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        return false;
    }

    const QByteArray data = QByteArrayLiteral(
        R"({"nodes":[{"id":"user_source","type":"user.input","config":{"trigger":"user"}},{"id":"output","type":"output.format"}],"edges":[{"from":"user_source","to":"output"}]})");
    return file.write(data) == data.size();
}

}

class ApplicationIntegrationTest : public QObject
{
    Q_OBJECT

private slots:
    void UserInputReachesOutputNode();
    void PerceptionFrameReachesRuntimeContext();
};

void ApplicationIntegrationTest::UserInputReachesOutputNode()
{
    QTemporaryDir directory;
    QString configPath;
    QVERIFY(WriteDag(directory, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("user.input"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_RESPONSE,
                                context.GetUserInput());
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("output.format"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        QVariant responseValue;
        return context.GetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_RESPONSE,
                                responseValue)
               && context.SetValue(vpet::AgentContextKeys::OUTPUT_TEXT,
                                   responseValue);
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.ExecuteWithUserInput(QStringLiteral("hello"), errorMessage),
             qPrintable(errorMessage));

    QVariant outputValue;
    QVERIFY(runtime.GetContext().GetValue(vpet::AgentContextKeys::OUTPUT_TEXT, outputValue));
    QCOMPARE(outputValue.toString(), QStringLiteral("hello"));
}

void ApplicationIntegrationTest::PerceptionFrameReachesRuntimeContext()
{
    vpet::AgentRuntime runtime;
    QString errorMessage;

    QVERIFY(runtime.UpdatePerceptionFrame(QByteArrayLiteral("frame"),
                                          1,
                                          QSize(320, 200),
                                          QStringLiteral("vision/screenshot"),
                                          errorMessage));

    QVariant frameValue;
    QVERIFY(runtime.GetContext().GetValue(vpet::AgentContextKeys::VISION_LATEST_FRAME_ID,
                                          frameValue));
    QCOMPARE(frameValue.toInt(), 1);

    QVERIFY(runtime.UpdatePerceptionFrame(QByteArrayLiteral("frame"),
                                          2,
                                          QSize(640, 400),
                                          QStringLiteral("vision/screenshot"),
                                          errorMessage));
    QVERIFY(runtime.GetContext().GetValue(vpet::AgentContextKeys::VISION_LATEST_FRAME_ID,
                                          frameValue));
    QCOMPARE(frameValue.toInt(), 1);
}

QTEST_MAIN(ApplicationIntegrationTest)

#include "application_integration_test.moc"
