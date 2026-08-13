#include "main_window.h"
#include "vpet/agent/agent_runtime.h"
#include "vpet/splash_window.h"
#include "vpet/tts_server_manager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QStringList>
#include <QTimer>

namespace
{

const QString AGENT_DAG_CONFIG_FILE_NAME = QStringLiteral("agent_dag_structure.json");

/**
 * @brief 查找 Agent DAG 配置文件
 * @return 配置文件绝对路径；未找到时返回空字符串
 */
QString FindAgentDagConfigPath()
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QStringList candidatePaths =
    {
        exeDir + QStringLiteral("/") + AGENT_DAG_CONFIG_FILE_NAME,
        QDir::currentPath() + QStringLiteral("/") + AGENT_DAG_CONFIG_FILE_NAME,
        exeDir + QStringLiteral("/../") + AGENT_DAG_CONFIG_FILE_NAME,
        exeDir + QStringLiteral("/../../") + AGENT_DAG_CONFIG_FILE_NAME
    };

    for (const QString &candidatePath : candidatePaths)
    {
        if (QFileInfo::exists(candidatePath))
        {
            return QFileInfo(candidatePath).absoluteFilePath();
        }
    }

    return QString();
}

} // anonymous namespace

/**
 * @brief 获取动画资源根目录
 *
 * 优先使用可执行文件所在目录下的 Animation/；调试时回退到项目根目录。
 *
 * @return 动画资源根目录
 */
static QString GetAnimationBasePath()
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QStringList candidateDirectories =
    {
        exeDir,
        QDir::currentPath(),
        QDir(exeDir).absoluteFilePath(QStringLiteral("..")),
        QDir(exeDir).absoluteFilePath(QStringLiteral("../.."))
    };

    for (const QString &candidateDirectory : candidateDirectories)
    {
        const QString animationDirectory = QDir(candidateDirectory).filePath(
            QStringLiteral("Animation"));
        if (QDir(animationDirectory).exists())
        {
            return QDir(animationDirectory).absolutePath();
        }
    }

    return QDir(exeDir).filePath(QStringLiteral("Animation"));
}

/**
 * @brief 程序入口
 * @param[in] argc 参数个数
 * @param[in] argv 参数列表
 * @return 进程退出码
 */
int main(int argc, char *argv[])
{
    QApplication application(argc, argv);

    // 创建启动画面
    vpet::SplashWindow splashWindow;
    splashWindow.SetStatusText(QStringLiteral("正在初始化..."));
    splashWindow.show();
    application.processEvents();

    // 创建 TTS 服务管理器
    vpet::TtsServerManager ttsServerManager;

    bool ttsServerReady = false;

    // 先建立事件循环 — 必须在 Start() 之前连接所有信号
    // 因为 Start() 失败时会同步发射 ServerStartFailed，
    // 如果连接在 Start() 之后，信号将丢失导致事件循环永远等待
    QEventLoop waitLoop;

    QObject::connect(&ttsServerManager,
                     &vpet::TtsServerManager::ServerReady,
                     &waitLoop,
                     &QEventLoop::quit);

    QObject::connect(&ttsServerManager,
                     &vpet::TtsServerManager::ServerStartFailed,
                     &waitLoop,
                     &QEventLoop::quit);

    QObject::connect(&ttsServerManager,
                     &vpet::TtsServerManager::StatusChanged,
                     &splashWindow,
                     &vpet::SplashWindow::SetStatusText);

    QObject::connect(&ttsServerManager,
                     &vpet::TtsServerManager::ServerReady,
                     &splashWindow,
                     [&]()
    {
        ttsServerReady = true;
    });

    // 启动 TTS 服务；同步失败时不进入事件循环，避免无效等待安全超时。
    // 启动成功则进入异步健康检查，就绪后发射 ServerReady → waitLoop.quit。
    // 超时（约 60 秒）后也会发射 ServerStartFailed → waitLoop.quit
    // 安全网：如果因任何原因两个信号都没发射，60 秒后强制退出等待
    QTimer safetyTimer;
    safetyTimer.setSingleShot(true);

    QObject::connect(&safetyTimer, &QTimer::timeout, &waitLoop, &QEventLoop::quit);

    safetyTimer.start(60000);

    if (ttsServerManager.Start(QString()))
    {
        waitLoop.exec();
    }

    safetyTimer.stop();

    // 隐藏启动画面
    splashWindow.hide();

    // 启动 Agent DAG 运行时，后续语音输入将进入节点化链路。
    vpet::AgentRuntime agentRuntime;
    QString agentErrorMessage;

    if (!agentRuntime.LoadDefaultLlmConfig(agentErrorMessage))
    {
        qWarning() << "[Agent]" << agentErrorMessage
                   << "Voice input will stop before LLM request.";
    }

    if (!agentRuntime.LoadDefaultVisionLlmConfig(agentErrorMessage))
    {
        qWarning() << "[Agent]" << agentErrorMessage
                   << "Vision LLM DAG node will be skipped before request.";
    }

    if (!agentRuntime.LoadDefaultWebSearchConfig(agentErrorMessage))
    {
        qWarning() << "[Agent]" << agentErrorMessage
                   << "Web research requests will fail according to the DAG failure policy.";
    }

    if (!agentRuntime.LoadDefaultMemoryConfig(agentErrorMessage))
    {
        qWarning() << "[Agent]" << agentErrorMessage
                   << "Memory nodes will be skipped without affecting replies.";
    }

    const QString agentDagConfigPath = FindAgentDagConfigPath();

    if (agentDagConfigPath.isEmpty())
    {
        qWarning() << "[Agent] agent_dag_structure.json not found. Agent runtime disabled.";
    }
    else if (!agentRuntime.Start(agentDagConfigPath, agentErrorMessage))
    {
        qWarning() << "[Agent] Failed to start runtime:" << agentErrorMessage;
    }

    // 创建并初始化主窗口
    vpet::MainWindow window;
    window.SetAgentRuntime(&agentRuntime);

    if (!window.Initialize(GetAnimationBasePath()))
    {
        QMessageBox::critical(nullptr,
                              QStringLiteral("VPet 启动失败"),
                              QStringLiteral("无法加载动画资源，请检查 Animation/ 目录。"));
        return 1;
    }

    // 如果 TTS 就绪，将服务器 URL 传递给主窗口
    if (ttsServerReady)
    {
        // TTS 配置已在 TtsServerManager 中加载，PetController 中的
        // TtsClient 会通过 tts_config.json 自行完成 HTTP 请求配置
    }

    // 不调用 setParent：ttsServerManager 是栈对象，挂到 QObject 树会在
    // ~MainWindow 中被 delete，随后栈展开再析构一次，构成未定义行为。
    window.show();

    const int exitCode = application.exec();

    // 退出前停止记忆服务（flush 已提交写入），再停止 TTS 服务（窗口已销毁，栈对象仍有效）
    agentRuntime.ShutdownMemory();
    ttsServerManager.Stop();

    return exitCode;
}
