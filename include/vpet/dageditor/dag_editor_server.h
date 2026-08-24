#ifndef VPET_DAGEDITOR_DAG_EDITOR_SERVER_H
#define VPET_DAGEDITOR_DAG_EDITOR_SERVER_H

#include "vpet/dageditor/agent_dag_validator.h"
#include "vpet/dageditor/dag_document.h"
#include "vpet/dageditor/dag_http_request.h"

#include <QJsonObject>
#include <QObject>
#include <QUrl>
#include <QVector>

class QFileSystemWatcher;
class QTimer;

namespace vpet
{

class AgentNodeCatalog;
class AgentRuntime;
class DagDocumentStore;
class DagHttpServer;

/**
 * @brief DAG 编辑器本地服务
 *
 * 职责：静态资源伺服（qrc）、REST API、SSE 事件推送、token/Host 安全校验，
 * 以及「保存 → 校验 → 原子落盘 → 热重载」链路的编排。
 *
 * 安全模型：仅绑定回环地址；全部 /api/* 请求要求 X-DAG-Token 头或 ?token= 查询
 * 参数匹配；Host 头白名单防御 DNS rebinding；不返回任何 CORS 头。
 */
class DagEditorServer : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造服务（不启动监听）
     * @param[in] parent 父对象
     */
    explicit DagEditorServer(QObject *parent = nullptr);

    /**
     * @brief 析构时停止服务
     */
    ~DagEditorServer() override;

    /**
     * @brief 启动服务：加载文档基线、生成 token 并绑定随机回环端口
     * @param[in] configPath DAG 配置文件路径
     * @param[in] agentRuntime 运行时引用（热重载与状态查询；可为空，空时只读降级）
     * @param[in] catalog 节点目录；空则使用进程默认实例
     * @param[out] errorMessage 失败描述
     * @return 启动成功返回 true
     */
    bool Start(const QString &configPath,
               AgentRuntime *agentRuntime,
               const AgentNodeCatalog *catalog,
               QString &errorMessage);

    /**
     * @brief 停止服务并释放文档仓库
     */
    void Stop();

    /**
     * @brief 判断服务是否正在运行
     * @return 运行中返回 true
     */
    bool IsRunning() const;

    /**
     * @brief 获取带 token 的编辑器访问 URL
     * @return 形如 http://127.0.0.1:<port>/?token=<token>
     */
    QUrl EditorUrl() const;

private:
    /**
     * @brief 统一请求入口：安全校验后分发到各路由
     * @param[in] request 已解析请求
     * @return 待写回响应
     */
    _tagDagHttpResponse HandleRequest(const _tagDagHttpRequest &request);

    /** @brief 伺服 qrc 静态资源（无需 token）。 */
    _tagDagHttpResponse HandleStaticResource(const _tagDagHttpRequest &request);

    /** @brief GET /api/meta。 */
    _tagDagHttpResponse HandleMeta();

    /** @brief GET /api/object_info。 */
    _tagDagHttpResponse HandleObjectInfo();

    /** @brief GET /api/graph。 */
    _tagDagHttpResponse HandleGetGraph();

    /** @brief PUT /api/graph?baseRevision=N：校验→保存→热重载。 */
    _tagDagHttpResponse HandlePutGraph(const _tagDagHttpRequest &request);

    /** @brief POST /api/graph/validate：干跑校验。 */
    _tagDagHttpResponse HandleValidate(const _tagDagHttpRequest &request);

    /** @brief POST /api/graph/reload：手动触发热重载。 */
    _tagDagHttpResponse HandleReload();

    /** @brief GET /api/runtime/status：只读忙碌状态。 */
    _tagDagHttpResponse HandleRuntimeStatus();

    /**
     * @brief 构造 JSON 响应
     * @param[in] statusCode HTTP 状态码
     * @param[in] object JSON 对象
     * @return 响应
     */
    static _tagDagHttpResponse JsonResponse(int statusCode, const QJsonObject &object);

    /**
     * @brief 构造携带 issues 数组的响应体
     * @param[in] statusCode HTTP 状态码
     * @param[in] prefix 顶层包装键；空则直接输出 {issues:[...]}
     * @param[in] issues 校验问题列表
     * @return 响应
     */
    static _tagDagHttpResponse IssuesResponse(int statusCode,
                                              const QString &prefix,
                                              const QVector<DagValidationIssue> &issues);

    /**
     * @brief 序列化单个节点 spec 为 object_info 条目
     * @param[in] spec 节点描述
     * @return JSON 条目
     */
    static QJsonObject SerializeSpec(const DagNodeSpec &spec);

    /**
     * @brief 解析请求体中的文档 JSON
     * @param[in] request 请求
     * @param[out] document 输出文档
     * @return 解析成功返回 true
     */
    static bool ParseDocumentBody(const _tagDagHttpRequest &request, DagDocument &document);

    /**
     * @brief 触发热重载并广播结果
     * @return 重载结果 JSON（applied/reason）
     */
    QJsonObject TriggerHotReload();

    /**
     * @brief 向 SSE 主频道广播一个事件
     * @param[in] eventName 事件名
     * @param[in] payloadObject 载荷对象
     */
    void Broadcast(const QString &eventName, const QJsonObject &payloadObject);

private slots:
    /**
     * @brief 配置文件外部修改回调（防抖后采纳为新基线并广播）
     */
    void OnConfigFileChanged();

private:
    DagHttpServer *m_httpServer;      ///< HTTP 传输层
    DagDocumentStore *m_documentStore; ///< 文档仓库（本对象持有）
    AgentRuntime *m_agentRuntime;      ///< 运行时引用（不持有所有权）
    QString m_token;                   ///< 访问令牌
    QFileSystemWatcher *m_configWatcher; ///< 配置文件监视器（外部修改采纳）
    QTimer *m_externalDebounceTimer;     ///< 外部修改防抖定时器
};

} // namespace vpet

#endif // VPET_DAGEDITOR_DAG_EDITOR_SERVER_H
