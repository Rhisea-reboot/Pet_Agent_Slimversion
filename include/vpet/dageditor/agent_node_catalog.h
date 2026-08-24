#ifndef VPET_DAGEDITOR_AGENT_NODE_CATALOG_H
#define VPET_DAGEDITOR_AGENT_NODE_CATALOG_H

#include <QColor>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

namespace vpet
{

/**
 * @brief 属性面板控件类型（对应 ComfyUI widget 种类的最小子集）
 */
enum class DagWidgetType
{
    Text,          ///< 单行文本
    MultilineText, ///< 多行文本
    Int,           ///< 整数
    Float,         ///< 浮点数
    Bool,          ///< 开关
    Enum,          ///< 下拉选择
    StringList     ///< 字符串列表（编辑器内以逗号分隔输入）
};

/**
 * @brief 节点单个 config 输入的描述
 *
 * 内容提炼自各节点执行实现中读取的 config 键，仅用于编辑器展示与保存前校验，
 * 不参与运行时执行路径。
 */
struct DagInputSpec
{
    QString key;             ///< config 键，如 "temperature"
    QString label;           ///< 中文显示名
    DagWidgetType widget = DagWidgetType::Text; ///< 控件类型
    QVariant defaultValue;   ///< 默认值；为空表示运行时使用内置默认
    QVariant minValue;       ///< Int/Float 可选下界
    QVariant maxValue;       ///< Int/Float 可选上界
    double step = 0.0;       ///< Float 可选步长；0 表示不限制
    QStringList enumValues;  ///< Enum 必填选项列表
    QString tooltip;         ///< 悬浮提示，语义与现有实现保持一致
    bool required = false;   ///< 是否必填（缺失时校验器报 error）
};

/**
 * @brief 单个节点类型的自描述 schema（等价 ComfyUI INPUT_TYPES + 元数据）
 */
struct DagNodeSpec
{
    QString type;            ///< 节点类型键（AgentContextKeys::NODE_TYPE_*）
    QString displayName;     ///< 中文显示名，如 "LLM 对话"
    QString category;        ///< 分类：输入源|感知|记忆|推理|输出|工具
    QColor accentColor;      ///< 画布标题栏配色（按分类）
    QString description;     ///< 一句话说明
    QStringList reads;       ///< 消费的上下文键（文档用途）
    QStringList writes;      ///< 产出的上下文键（文档用途）
    QString triggerSource;   ///< 触发源标记："user" | "vision" | 空
    bool isSource = false;   ///< 是否允许作为触发源节点
    QVector<DagInputSpec> inputs; ///< config 输入声明
};

/**
 * @brief 节点 schema 注册中心（等价 ComfyUI NODE_CLASS_MAPPINGS 的元数据侧）
 *
 * 进程内共享一个默认实例（Instance），测试可构造独立实例验证注册规则。
 * 仅在主线程使用；Register 不支持同类型覆盖，重复注册返回 false。
 */
class AgentNodeCatalog
{
public:
    /**
     * @brief 进程内默认实例（首次调用时注册全部内置节点 spec）
     * @return 共享实例引用
     */
    static const AgentNodeCatalog &Instance();

    /**
     * @brief 构造空目录
     */
    AgentNodeCatalog() = default;

    /**
     * @brief 注册一个节点 spec
     * @param[in] spec 节点描述
     * @return 注册成功返回 true；type 为空、重复或 spec 自相矛盾时返回 false
     */
    bool Register(const DagNodeSpec &spec);

    /**
     * @brief 判断类型是否已注册
     * @param[in] type 节点类型键
     * @return 已注册返回 true
     */
    bool Contains(const QString &type) const;

    /**
     * @brief 按类型查找节点 spec
     * @param[in] type 节点类型键
     * @param[out] out 输出 spec 副本
     * @return 找到返回 true
     */
    bool Find(const QString &type, DagNodeSpec &out) const;

    /**
     * @brief 获取全部已注册 spec（按注册顺序）
     * @return spec 列表
     */
    QVector<DagNodeSpec> All() const;

    /**
     * @brief 获取全部已注册类型键（按注册顺序）
     * @return 类型键列表
     */
    QStringList Types() const;

    /**
     * @brief 清空目录（测试辅助）
     */
    void Clear();

private:
    QVector<QString> m_order;              ///< 保持注册顺序的类型键
    QHash<QString, DagNodeSpec> m_specs;   ///< 类型键到 spec 的映射
};

/**
 * @brief 向目录注册全部内置节点 schema
 *
 * schema 内容以 RegisterDefaultNodeHandlers 注册的处理器与各节点实现读取的
 * config 键为准。单测保证目录集合 == 处理器集合 == 示例配置集合。
 *
 * @param[in,out] catalog 目标目录；已包含同名类型时跳过该类型
 * @return 全部注册成功返回 true
 */
bool RegisterDefaultNodeSpecs(AgentNodeCatalog &catalog);

} // namespace vpet

#endif // VPET_DAGEDITOR_AGENT_NODE_CATALOG_H
