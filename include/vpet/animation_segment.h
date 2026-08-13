#ifndef VPET_ANIMATION_SEGMENT_H
#define VPET_ANIMATION_SEGMENT_H

#include "vpet/animation_frame.h"
#include "vpet/common_types.h"

#include <QList>
#include <QString>

namespace vpet
{

/**
 * @brief 动画段，包含同一段（A/B/C/Single）内的所有帧
 */
class AnimationSegment
{
public:
    /**
     * @brief 构造函数
     * @param[in] type 段类型
     */
    explicit AnimationSegment(ANIMATION_TYPE type);

    /**
     * @brief 从目录加载帧
     *
     * 扫描目录下所有 *.png 文件，按文件名中的帧号排序，解析显示时长。
     * 支持的文件名格式：`<任意前缀>_<帧号>_<时长>.png` 或 `_<帧号>_<时长>.png`。
     *
     * @param[in] directoryPath 帧文件所在目录
     * @return 成功加载至少一帧返回 true
     */
    bool LoadFromDirectory(const QString &directoryPath);

    /**
     * @brief 获取段类型
     * @return 段类型
     */
    ANIMATION_TYPE GetType() const;

    /**
     * @brief 获取帧数量
     * @return 帧数量
     */
    int GetFrameCount() const;

    /**
     * @brief 获取指定帧
     * @param[in] index 帧索引，从 0 开始
     * @return 对应帧的常量引用；索引非法时返回一个无效帧
     */
    const AnimationFrame &GetFrame(int index) const;

    /**
     * @brief 判断段是否为空
     * @return 没有帧时返回 true
     */
    bool IsEmpty() const;

    /**
     * @brief 获取段总时长
     * @return 所有帧时长之和，单位毫秒
     */
    int GetTotalDurationMs() const;

private:
    /**
     * @brief 解析单帧文件名，提取帧号与时长
     * @param[in] fileName 文件名，如 "摸头_000_125.png"
     * @param[out] frameIndex 解析出的帧号
     * @param[out] durationMs 解析出的时长
     * @return 解析成功返回 true
     */
    static bool ParseFrameFileName(const QString &fileName, int &frameIndex, int &durationMs);

private:
    ANIMATION_TYPE m_type;       ///< 段类型
    QList<AnimationFrame> m_frames; ///< 帧列表
};

} // namespace vpet

#endif // VPET_ANIMATION_SEGMENT_H
