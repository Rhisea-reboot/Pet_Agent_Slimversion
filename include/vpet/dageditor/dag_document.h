#ifndef VPET_DAGEDITOR_DAG_DOCUMENT_H
#define VPET_DAGEDITOR_DAG_DOCUMENT_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace vpet
{

/**
 * @brief 编辑器文档中的单个节点（语义字段 + 可选布局元数据）
 *
 * 语义字段（id/type/config）与运行时格式完全一致；布局字段位于 JSON 的
 * editor.nodes.<id> 下，AgentDagGraph 会忽略。
 */
struct _tagDagDocumentNode
{
    QString id;            ///< 节点唯一标识
    QString type;          ///< 节点执行类型
    QJsonObject config;    ///< 节点配置对象

    bool hasLayout = false; ///< 是否携带编辑器布局信息
    double layoutX = 0.0;   ///< 画布 X 坐标
    double layoutY = 0.0;   ///< 画布 Y 坐标
    double layoutWidth = 0.0;  ///< 节点框宽度；0 表示未设置
    double layoutHeight = 0.0; ///< 节点框高度；0 表示未设置
    QString layoutTitle;       ///< 自定义标题；空表示使用目录默认名
    bool layoutCollapsed = false; ///< 是否折叠显示
};

/**
 * @brief 编辑器文档中的一条控制依赖边
 */
struct _tagDagDocumentEdge
{
    QString from; ///< 起始节点标识
    QString to;   ///< 目标节点标识
};

/**
 * @brief DAG 编辑器内存文档模型
 *
 * 对应磁盘上的 agent_dag_structure.json 格式 v1.1：
 * 顶层保留 nodes/edges 原样语义，追加可选 version 与 editor 字段；
 * 未识别的顶层字段会原样保留，保证与手工扩展字段的往返无损。
 */
class DagDocument
{
public:
    /// 当前文件格式版本号；缺失视为 v1（旧文件）
    static constexpr int kFormatVersion = 2;

    /**
     * @brief 构造空文档
     */
    DagDocument() = default;

    /**
     * @brief 从 JSON 字节加载文档
     * @param[in] jsonData 配置文件字节
     * @param[out] out 输出文档
     * @param[out] errorMessage 错误描述
     * @return 加载成功返回 true
     */
    static bool FromJsonData(const QByteArray &jsonData,
                             DagDocument &out,
                             QString &errorMessage);

    /**
     * @brief 从 JSON 对象加载文档
     * @param[in] rootObject 顶层 JSON 对象
     * @param[out] out 输出文档
     * @param[out] errorMessage 错误描述
     * @return 加载成功返回 true
     */
    static bool FromJsonObject(const QJsonObject &rootObject,
                               DagDocument &out,
                               QString &errorMessage);

    /**
     * @brief 序列化为 JSON 字节（带缩进，便于人工 diff）
     * @return JSON 字节
     */
    QByteArray ToJsonData() const;

    /**
     * @brief 序列化为 JSON 对象
     * @return 顶层 JSON 对象
     */
    QJsonObject ToJsonObject() const;

    /**
     * @brief 获取节点列表
     * @return 节点列表
     */
    const QVector<_tagDagDocumentNode> &GetNodes() const;

    /**
     * @brief 获取边列表
     * @return 边列表
     */
    const QVector<_tagDagDocumentEdge> &GetEdges() const;

    /**
     * @brief 替换全部节点（不校验语义，校验由 AgentDagValidator 负责）
     * @param[in] nodes 新节点列表
     */
    void SetNodes(const QVector<_tagDagDocumentNode> &nodes);

    /**
     * @brief 替换全部边
     * @param[in] edges 新边列表
     */
    void SetEdges(const QVector<_tagDagDocumentEdge> &edges);

    /**
     * @brief 按标识查找节点
     * @param[in] nodeId 节点标识
     * @param[out] out 输出节点副本
     * @return 找到返回 true
     */
    bool FindNode(const QString &nodeId, _tagDagDocumentNode &out) const;

    /**
     * @brief 按标识定位节点索引
     * @param[in] nodeId 节点标识
     * @return 索引；不存在返回 -1
     */
    int IndexOfNode(const QString &nodeId) const;

    /**
     * @brief 插入或更新节点（按 id 匹配）
     * @param[in] node 节点定义
     * @return id 为空返回 false
     */
    bool UpsertNode(const _tagDagDocumentNode &node);

    /**
     * @brief 删除节点及其关联边
     * @param[in] nodeId 节点标识
     * @return 实际删除返回 true
     */
    bool RemoveNode(const QString &nodeId);

    /**
     * @brief 追加一条边；重复边与自环直接拒绝
     * @param[in] from 起始节点标识
     * @param[in] to 目标节点标识
     * @return 追加成功返回 true
     */
    bool AddEdge(const QString &from, const QString &to);

    /**
     * @brief 删除第一条匹配边
     * @param[in] from 起始节点标识
     * @param[in] to 目标节点标识
     * @return 实际删除返回 true
     */
    bool RemoveEdge(const QString &from, const QString &to);

    /**
     * @brief 获取画布滚动位置 X
     * @return 滚动位置 X
     */
    double GetScrollX() const;

    /** @brief 获取画布滚动位置 Y。 @return 滚动位置 Y。 */
    double GetScrollY() const;

    /** @brief 获取画布缩放。 @return 缩放值。 */
    double GetZoom() const;

    /** @brief 设置画布视口。 @param[in] scrollX 滚动 X。 @param[in] scrollY 滚动 Y。 @param[in] zoom 缩放。 */
    void SetViewport(double scrollX, double scrollY, double zoom);

    /**
     * @brief 获取文档修订号
     *
     * revision 属于 editor 元数据，由 DocumentStore 维护并随保存递增；
     * 从磁盘加载时读取 editor.revision 作为初值。
     *
     * @return 修订号
     */
    quint64 GetRevision() const;

    /** @brief 设置文档修订号。 @param[in] revision 修订号。 */
    void SetRevision(quint64 revision);

private:
    QVector<_tagDagDocumentNode> m_nodes; ///< 节点列表
    QVector<_tagDagDocumentEdge> m_edges; ///< 边列表
    double m_scrollX = 0.0;               ///< 画布滚动位置 X
    double m_scrollY = 0.0;               ///< 画布滚动位置 Y
    double m_zoom = 1.0;                  ///< 画布缩放
    quint64 m_revision = 0;               ///< 编辑器修订号
    QJsonObject m_extraFields;            ///< 未识别的顶层字段（往返保真）
};

} // namespace vpet

#endif // VPET_DAGEDITOR_DAG_DOCUMENT_H
