#ifndef VPET_ANIMATION_RESOURCE_MANAGER_H
#define VPET_ANIMATION_RESOURCE_MANAGER_H

#include "vpet/animation_clip.h"
#include "vpet/common_types.h"

#include <QList>
#include <QMap>
#include <QString>

namespace vpet
{

/**
 * @brief 动画资源管理器
 *
 * 扫描 Animation/ 目录，解析目录结构并维护所有动画段候选目录。
 * 每次获取剪辑时随机选择候选目录并实时组装，支持每次交互使用不同变体。
 */
class AnimationResourceManager
{
public:
    /**
     * @brief 构造函数
     * @param[in] basePath 动画资源根目录，通常为项目下的 Animation/
     */
    explicit AnimationResourceManager(const QString &basePath);

    /**
     * @brief 加载所有动画资源
     * @return 成功加载至少一个有效动作返回 true
     */
    bool LoadAll();

    /**
     * @brief 获取指定动作与情绪的随机剪辑
     *
     * 对同一动作同一情绪下的多个候选目录随机选择，组装成完整剪辑。
     * 若指定情绪不存在，则回退到 NORMAL 情绪。
     *
     * @param[in] actionName 动作名，如 "normal", "touch_head"
     * @param[in] mood 情绪变体
     * @return 组装后的剪辑；未找到时返回无效剪辑
     */
    AnimationClip GetClip(const QString &actionName, PET_MOOD mood) const;

    /**
     * @brief 判断指定动作是否存在可用剪辑
     * @param[in] actionName 动作名
     * @return 存在返回 true
     */
    bool HasAction(const QString &actionName) const;

    /**
     * @brief 获取所有已加载动作名
     * @return 动作名列表
     */
    QList<QString> GetAllActionNames() const;

    /**
     * @brief 判断资源是否已加载
     * @return 已加载返回 true
     */
    bool IsLoaded() const;

    /**
     * @brief 获取所有 Say 分组动作名
     *
     * 返回形如 "say_self", "say_serious" 的动作名列表，用于待机时随机选择。
     *
     * @return Say 动作名列表
     */
    QList<QString> GetSayActionNames() const;

private:
    /**
     * @brief 单个段类型候选目录信息
     */
    struct _tagSegmentCandidate
    {
        PET_MOOD mood;         ///< 情绪
        ANIMATION_TYPE type;   ///< 段类型
        QString directoryPath; ///< 帧文件所在目录
    };

    /**
     * @brief 加载使用 A/B/C 段结构的动作
     *
     * 扫描动作目录下的 A_xxx/B_xxx/C_xxx 或 xxx/A、xxx/B 等结构，
     * 同一动作同一情绪的多个子目录视为随机候选。
     *
     * @param[in] actionName 动作名
     * @param[in] actionDir 动作根目录
     */
    void LoadSegmentedAction(const QString &actionName, const QString &actionDir);

    /**
     * @brief 加载只有单一动画变体的动作
     *
     * 动作目录下每个子目录被视为一个随机候选，最终作为 Single 段保存。
     *
     * @param[in] actionName 动作名
     * @param[in] actionDir 动作根目录
     * @param[in] mood 情绪
     */
    void LoadSingleSegmentAction(const QString &actionName,
                                 const QString &actionDir,
                                 PET_MOOD mood);

    /**
     * @brief 加载 Say 动作分组
     *
     * 扫描 Say/ 下的每个子目录作为一个独立分组，动作名为 "say_<分组名小写>"。
     *
     * @param[in] sayDir Say 资源根目录
     */
    void LoadSayActions(const QString &sayDir);

    /**
     * @brief 递归收集段目录候选
     *
     * 从动作目录开始向下递归，找到所有叶子帧目录，并标注其段类型与情绪。
     *
     * @param[in] currentPath 当前扫描路径
     * @param[in] parentName 父目录名，用于推断纯 A/B/C 目录的情绪
     * @param[in,out] candidates 收集到的候选列表
     */
    void CollectSegmentCandidates(const QString &currentPath,
                                  const QString &parentName,
                                  QList<_tagSegmentCandidate> &candidates) const;

    /**
     * @brief 从目录名解析段类型与情绪
     * @param[in] dirName 目录名
     * @param[in] parentName 父目录名，用于纯 A/B/C 目录的情绪推断
     * @param[out] type 解析出的段类型
     * @param[out] mood 解析出的情绪
     * @return 解析成功返回 true
     */
    static bool ParseSegmentDirectory(const QString &dirName,
                                      const QString &parentName,
                                      ANIMATION_TYPE &type,
                                      PET_MOOD &mood);

    /**
     * @brief 判断目录名是否表示段目录
     * @param[in] dirName 目录名
     * @return 是段目录返回 true
     */
    static bool IsSegmentDirectoryName(const QString &dirName);

    /**
     * @brief 判断目录是否为叶子帧目录
     * @param[in] dirPath 目录路径
     * @return 包含 png 文件返回 true
     */
    static bool IsLeafFrameDirectory(const QString &dirPath);

    /**
     * @brief 将候选目录列表按情绪和段类型分组
     * @param[in] candidates 候选列表
     * @return 分组结果：情绪 -> 段类型 -> 目录列表
     */
    static QMap<PET_MOOD, QMap<ANIMATION_TYPE, QList<QString>>> GroupCandidatesByMoodAndType(
        const QList<_tagSegmentCandidate> &candidates);

    /**
     * @brief 选择最佳情绪回退
     * @param[in] action 动作名
     * @param[in] preferredMood 首选情绪
     * @return 实际存在的情绪枚举
     */
    PET_MOOD ResolveMood(const QString &action, PET_MOOD preferredMood) const;

private:
    QString m_basePath; ///< 动画资源根目录
    bool m_isLoaded;    ///< 加载状态

    /**
     * @brief 动作名 -> 情绪 -> 段类型 -> 候选目录列表
     */
    QMap<QString, QMap<PET_MOOD, QMap<ANIMATION_TYPE, QList<QString>>>> m_candidates;

    QList<QString> m_sayActionNames; ///< 已加载的 Say 分组动作名列表
};

} // namespace vpet

#endif // VPET_ANIMATION_RESOURCE_MANAGER_H
