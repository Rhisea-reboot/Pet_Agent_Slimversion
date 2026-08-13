#ifndef VPET_PERCEPTION_VISION_ENCODER_H
#define VPET_PERCEPTION_VISION_ENCODER_H

#include <QByteArray>
#include <QImage>
#include <QPixmap>
#include <QString>

namespace vpet
{

/**
 * @brief 视觉图像编码器
 *
 * 将 QPixmap 转换为后续 LLM 视觉接口可使用的原始字节或 Base64 数据。
 */
class VisionEncoder
{
public:
    /**
     * @brief 编码格式
     */
    enum class VISION_ENCODE_FORMAT
    {
        RAW_PNG,
        RAW_JPEG,
        BASE64_PNG,
        BASE64_JPEG
    };

    /**
     * @brief 编码选项
     */
    struct _tagEncodeOptions
    {
        int maxWidth = 0;              ///< 最大宽度，0 表示不限制
        int maxHeight = 0;             ///< 最大高度，0 表示不限制
        int quality = -1;              ///< 图像质量，-1 表示使用 Qt 默认值
        bool keepAspectRatio = true;   ///< 缩放时是否保持宽高比
    };

    /**
     * @brief 将图像编码为指定格式
     * @param[in] pixmap 输入图像
     * @param[in] format 目标编码格式
     * @param[in] options 编码选项
     * @return 编码后的数据；参数无效或编码失败时返回空数组
     */
    static QByteArray Encode(const QPixmap &pixmap,
                             VISION_ENCODE_FORMAT format,
                             const _tagEncodeOptions &options);

    /**
     * @brief 将线程安全的 QImage 编码为指定格式
     * @param[in] image 输入图像
     * @param[in] format 目标编码格式
     * @param[in] options 编码选项
     * @return 编码后的数据；参数无效或编码失败时返回空数组
     */
    static QByteArray Encode(const QImage &image,
                             VISION_ENCODE_FORMAT format,
                             const _tagEncodeOptions &options);

    /**
     * @brief 使用默认选项将图像编码为指定格式
     * @param[in] pixmap 输入图像
     * @param[in] format 目标编码格式
     * @return 编码后的数据；参数无效或编码失败时返回空数组
     */
    static QByteArray Encode(const QPixmap &pixmap, VISION_ENCODE_FORMAT format);

    /**
     * @brief 将图像编码为原始图像字节
     * @param[in] pixmap 输入图像
     * @param[in] imageFormat 图像格式，如 PNG 或 JPG
     * @param[in] quality 图像质量，-1 表示使用 Qt 默认值
     * @return 编码后的原始图像字节；参数无效或编码失败时返回空数组
     */
    static QByteArray ToImageBytes(const QPixmap &pixmap,
                                   const QString &imageFormat,
                                   int quality = -1);

    /**
     * @brief 将图像编码为 Base64
     * @param[in] pixmap 输入图像
     * @param[in] imageFormat 图像格式，如 PNG 或 JPG
     * @param[in] quality 图像质量，-1 表示使用 Qt 默认值
     * @return Base64 编码结果；参数无效或编码失败时返回空数组
     */
    static QByteArray ToBase64(const QPixmap &pixmap,
                               const QString &imageFormat = QStringLiteral("PNG"),
                               int quality = -1);

    /**
     * @brief 构造 OpenAI Vision API 可用的 data URL
     * @param[in] base64Image Base64 图像数据
     * @param[in] mediaType 媒体类型，如 image/png
     * @return data URL；参数无效时返回空字符串
     */
    static QString MakeVisionDataUrl(const QByteArray &base64Image,
                                     const QString &mediaType = QStringLiteral("image/png"));

private:
    /**
     * @brief 根据编码选项缩放图像
     * @param[in] pixmap 输入图像
     * @param[in] options 编码选项
     * @return 缩放后的图像；无需缩放时返回原图副本
     */
    static QPixmap ScaledPixmap(const QPixmap &pixmap, const _tagEncodeOptions &options);

    /** @brief 根据编码选项缩放 QImage。 */
    static QImage ScaledImage(const QImage &image, const _tagEncodeOptions &options);

    /**
     * @brief 将编码枚举转换为图像格式字符串
     * @param[in] format 编码格式
     * @return 图像格式字符串
     */
    static QString ImageFormatName(VISION_ENCODE_FORMAT format);
};

} // namespace vpet

#endif // VPET_PERCEPTION_VISION_ENCODER_H
