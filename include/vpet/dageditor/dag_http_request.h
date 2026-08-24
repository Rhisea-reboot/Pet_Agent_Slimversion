#ifndef VPET_DAGEDITOR_DAG_HTTP_SERVER_H
#define VPET_DAGEDITOR_DAG_HTTP_SERVER_H

#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

class QTcpServer;
class QTcpSocket;

namespace vpet
{

/**
 * @brief 解析后的 HTTP 请求（仅覆盖本地编辑器需要的极小子集）
 */
struct _tagDagHttpRequest
{
    QString method;                  ///< 大写方法名：GET/POST/PUT/HEAD/OPTIONS
    QString path;                    ///< 百分号解码后的路径，不含查询串
    QHash<QString, QString> query;   ///< 查询参数
    QMap<QString, QString> headers;  ///< 头字段；键为小写
    QByteArray body;                 ///< 请求体（受 1 MB 上限约束）

    /**
     * @brief 读取头字段
     * @param[in] name 小写头名称
     * @return 头值；缺失返回空字符串
     */
    QString HeaderValue(const QString &name) const;

    /**
     * @brief 读取查询参数
     * @param[in] key 参数名
     * @return 参数值；缺失返回空字符串
     */
    QString QueryValue(const QString &key) const;
};

/**
 * @brief 待写回的 HTTP 响应
 *
 * isEventStream 为 true 时表示该连接升级为 SSE 长连接，由服务器持有并持续推送。
 */
struct _tagDagHttpResponse
{
    int statusCode = 200;            ///< HTTP 状态码
    QByteArray contentType;          ///< 默认 application/json; charset=utf-8
    QByteArray body;                 ///< 响应体
    bool isEventStream = false;      ///< 升级为 SSE 流
    QString eventChannel;            ///< SSE 频道名（当前仅 "main"）
    QList<QPair<QByteArray, QByteArray>> extraHeaders; ///< 附加响应头
};

/**
 * @brief 基于 QTcpServer 的极简 HTTP/1.1 服务层
 *
 * 设计边界（本地单用户场景）：不做 TLS、不压缩、不分块请求编码；
 * 普通请求采用“响应后关闭连接”策略，仅 SSE 连接保持长开。
 * 全部操作在主线程事件循环内完成，无锁。
 */
class DagHttpServer : public QObject
{
    Q_OBJECT

public:
    /// 请求处理回调；返回待写回响应
    using RequestHandler = std::function<_tagDagHttpResponse(const _tagDagHttpRequest &)>;

    /**
     * @brief 构造服务层
     * @param[in] parent 父对象
     */
    explicit DagHttpServer(QObject *parent = nullptr);

    /**
     * @brief 析构时停止监听并断开全部连接
     */
    ~DagHttpServer() override;

    /**
     * @brief 在回环地址上启动监听
     * @param[in] port 期望端口；0 表示由内核分配临时端口
     * @param[out] errorMessage 失败描述
     * @return 启动成功返回 true
     */
    bool Start(quint16 port, QString &errorMessage);

    /**
     * @brief 停止监听并关闭全部连接
     */
    void Stop();

    /**
     * @brief 获取实际绑定的端口
     * @return 端口号；未启动返回 0
     */
    quint16 Port() const;

    /**
     * @brief 注册请求处理器（启动前设置）
     * @param[in] handler 处理回调
     */
    void SetRequestHandler(RequestHandler handler);

    /**
     * @brief 向全部 SSE 客户端广播一个事件
     * @param[in] channel 频道名
     * @param[in] eventName 事件名
     * @param[in] jsonPayload 载荷（JSON 文本）
     */
    void BroadcastEvent(const QString &channel,
                        const QString &eventName,
                        const QString &jsonPayload);

private slots:
    /**
     * @brief 接受新连接（应用每 IP 并发上限）
     */
    void OnNewConnection();

    /**
     * @brief 连接断开后的收尾：先移出映射表，再延迟销毁 socket
     * @param[in] socket 已断开的连接
     */
    void HandleSocketClosed(QTcpSocket *socket);

private:
    /**
     * @brief 单个连接的就绪读取入口：累积字节并解析完整请求
     */
    void OnReadyRead();

    /**
     * @brief 尝试从接收缓冲区解析一个完整请求
     * @param[in] socket 目标连接
     * @param[out] request 输出请求
     * @return 已取到完整请求返回 true；数据不足返回 false
     */
    bool TryParseRequest(QTcpSocket *socket, _tagDagHttpRequest &request);

    /**
     * @brief 执行路由并写回响应；SSE 响应转为长连接托管
     * @param[in] socket 目标连接
     * @param[in] request 已解析请求
     */
    void DispatchRequest(QTcpSocket *socket, const _tagDagHttpRequest &request);

    /**
     * @brief 写回错误响应并关闭连接
     * @param[in] socket 目标连接
     * @param[in] statusCode 状态码
     * @param[in] message JSON 错误信息
     */
    void WriteErrorAndClose(QTcpSocket *socket, int statusCode, const QString &message);

    /**
     * @brief 将连接升级为 SSE 流并发送初始注释
     * @param[in] socket 目标连接
     * @param[in] channel 频道名
     */
    void UpgradeToEventStream(QTcpSocket *socket, const QString &channel);

    /**
     * @brief 发送 SSE 心跳并清理失效连接
     */
    void SendHeartbeats();

    /**
     * @brief 清理已断开的连接与流记录
     */
    void CleanupClosedSockets();

    QTcpServer *m_tcpServer;                          ///< 底层 TCP 监听
    RequestHandler m_requestHandler;                  ///< 路由处理器
    QTimer *m_heartbeatTimer;                         ///< SSE 心跳定时器
    QHash<QTcpSocket *, QByteArray> m_receiveBuffers; ///< 各连接接收缓冲区
    QList<QTcpSocket *> m_eventStreams;               ///< 托管中的 SSE 连接
};

} // namespace vpet

#endif // VPET_DAGEDITOR_DAG_HTTP_SERVER_H
