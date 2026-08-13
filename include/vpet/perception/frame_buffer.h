#ifndef VPET_PERCEPTION_FRAME_BUFFER_H
#define VPET_PERCEPTION_FRAME_BUFFER_H

#include <QDateTime>
#include <QPixmap>
#include <QString>
#include <QVector>
#include <Qt>

#include <cstddef>

namespace vpet
{

/**
 * @brief 感知帧数据
 */
struct _tagFrame
{
    QPixmap pixmap;       ///< 图像数据
    QDateTime timestamp;  ///< 截图时间戳
    int sequenceId = -1;  ///< 帧序号
    QString filePath;     ///< 截图文件路径；未保存时为空
};

/**
 * @brief 环形帧缓冲
 *
 * 存储最近 N 帧，外部读取索引约定为 0 表示最新帧，1 表示次新帧。
 */
class FrameBuffer
{
public:
    /**
     * @brief 构造函数
     * @param[in] capacity 缓冲容量；小于 1 时使用最小容量
     */
    explicit FrameBuffer(std::size_t capacity);

    /**
     * @brief 写入一帧
     * @param[in] frame 输入帧；空图像帧不会写入
     * @return 写入成功返回 true
     */
    bool Push(const _tagFrame &frame);

    /**
     * @brief 获取最新帧
     * @return 最新帧；缓冲为空时返回无效帧
     */
    _tagFrame GetLatest() const;

    /**
     * @brief 按相对索引获取帧
     * @param[in] index 相对索引，0 表示最新帧
     * @return 指定帧；索引越界时返回无效帧
     */
    _tagFrame GetAt(std::size_t index) const;

    /**
     * @brief 获取最近 N 帧
     * @param[in] count 请求帧数
     * @return 最近帧列表，按最新到最旧排序
     */
    QVector<_tagFrame> GetRecent(std::size_t count) const;

    /**
     * @brief 获取当前帧数
     * @return 当前缓冲帧数
     */
    std::size_t GetSize() const;

    /**
     * @brief 获取缓冲容量
     * @return 缓冲容量
     */
    std::size_t GetCapacity() const;

    /**
     * @brief 判断缓冲是否为空
     * @return 为空返回 true
     */
    bool IsEmpty() const;

    /**
     * @brief 清空缓冲
     */
    void Clear();

    /**
     * @brief 拼接最近 N 帧
     * @param[in] count 请求拼接帧数
     * @param[in] orientation 拼接方向
     * @return 拼接图像；参数无效或缓冲为空时返回空图像
     */
    QPixmap StitchRecent(std::size_t count, Qt::Orientation orientation) const;

private:
    /**
     * @brief 归一化缓冲容量
     * @param[in] capacity 输入容量
     * @return 有效容量
     */
    static std::size_t NormalizeCapacity(std::size_t capacity);

    /**
     * @brief 创建无效帧
     * @return 无效帧对象
     */
    static _tagFrame MakeInvalidFrame();

private:
    std::size_t m_capacity;       ///< 最大缓冲容量
    QVector<_tagFrame> m_frames;  ///< 帧列表，内部按最旧到最新排序
};

} // namespace vpet

#endif // VPET_PERCEPTION_FRAME_BUFFER_H
