#include "vpet/perception/vision_encoder.h"

#include <QBuffer>
#include <QIODevice>
#include <Qt>

namespace vpet
{

QByteArray VisionEncoder::Encode(const QPixmap &pixmap,
                                 VISION_ENCODE_FORMAT format,
                                 const _tagEncodeOptions &options)
{
    if (pixmap.isNull())
    {
        return QByteArray();
    }

    return Encode(pixmap.toImage(), format, options);
}

QByteArray VisionEncoder::Encode(const QImage &image,
                                 VISION_ENCODE_FORMAT format,
                                 const _tagEncodeOptions &options)
{
    if (image.isNull())
    {
        return QByteArray();
    }

    const QImage scaledImage = ScaledImage(image, options);
    const QString imageFormat = ImageFormatName(format);

    if (imageFormat.isEmpty())
    {
        return QByteArray();
    }

    QByteArray imageBytes;
    QBuffer buffer(&imageBytes);

    if (!buffer.open(QIODevice::WriteOnly))
    {
        return QByteArray();
    }

    const QByteArray formatName = imageFormat.toLatin1();

    if (!scaledImage.save(&buffer, formatName.constData(), options.quality))
    {
        return QByteArray();
    }

    if ((format == VISION_ENCODE_FORMAT::BASE64_PNG)
        || (format == VISION_ENCODE_FORMAT::BASE64_JPEG))
    {
        return imageBytes.toBase64();
    }

    return imageBytes;
}

QByteArray VisionEncoder::Encode(const QPixmap &pixmap, VISION_ENCODE_FORMAT format)
{
    const _tagEncodeOptions options;

    return Encode(pixmap, format, options);
}

QByteArray VisionEncoder::ToImageBytes(const QPixmap &pixmap,
                                       const QString &imageFormat,
                                       int quality)
{
    if (pixmap.isNull())
    {
        return QByteArray();
    }

    if (imageFormat.trimmed().isEmpty())
    {
        return QByteArray();
    }

    QByteArray imageBytes;
    QBuffer buffer(&imageBytes);

    if (!buffer.open(QIODevice::WriteOnly))
    {
        return QByteArray();
    }

    const QByteArray formatName = imageFormat.trimmed().toUpper().toLatin1();
    const bool saved = pixmap.save(&buffer, formatName.constData(), quality);

    if (!saved)
    {
        return QByteArray();
    }

    return imageBytes;
}

QByteArray VisionEncoder::ToBase64(const QPixmap &pixmap,
                                   const QString &imageFormat,
                                   int quality)
{
    const QByteArray imageBytes = ToImageBytes(pixmap, imageFormat, quality);

    if (imageBytes.isEmpty())
    {
        return QByteArray();
    }

    return imageBytes.toBase64();
}

QString VisionEncoder::MakeVisionDataUrl(const QByteArray &base64Image, const QString &mediaType)
{
    if (base64Image.isEmpty())
    {
        return QString();
    }

    if (mediaType.trimmed().isEmpty())
    {
        return QString();
    }

    return QStringLiteral("data:%1;base64,%2").arg(mediaType.trimmed(),
                                                   QString::fromLatin1(base64Image));
}

QPixmap VisionEncoder::ScaledPixmap(const QPixmap &pixmap, const _tagEncodeOptions &options)
{
    if (pixmap.isNull())
    {
        return QPixmap();
    }

    const bool hasMaxWidth = options.maxWidth > 0;
    const bool hasMaxHeight = options.maxHeight > 0;

    if (!hasMaxWidth && !hasMaxHeight)
    {
        return pixmap;
    }

    int targetWidth = hasMaxWidth ? options.maxWidth : pixmap.width();
    int targetHeight = hasMaxHeight ? options.maxHeight : pixmap.height();

    if ((targetWidth <= 0) || (targetHeight <= 0))
    {
        return pixmap;
    }

    if ((pixmap.width() <= targetWidth) && (pixmap.height() <= targetHeight))
    {
        return pixmap;
    }

    const Qt::AspectRatioMode aspectRatioMode = options.keepAspectRatio
                                                ? Qt::KeepAspectRatio
                                                : Qt::IgnoreAspectRatio;

    return pixmap.scaled(targetWidth,
                         targetHeight,
                         aspectRatioMode,
                         Qt::SmoothTransformation);
}

QImage VisionEncoder::ScaledImage(const QImage &image, const _tagEncodeOptions &options)
{
    if (image.isNull())
    {
        return QImage();
    }

    const bool hasMaxWidth = options.maxWidth > 0;
    const bool hasMaxHeight = options.maxHeight > 0;

    if (!hasMaxWidth && !hasMaxHeight)
    {
        return image;
    }

    const int targetWidth = hasMaxWidth ? options.maxWidth : image.width();
    const int targetHeight = hasMaxHeight ? options.maxHeight : image.height();

    if ((targetWidth <= 0) || (targetHeight <= 0)
        || ((image.width() <= targetWidth) && (image.height() <= targetHeight)))
    {
        return image;
    }

    const Qt::AspectRatioMode aspectRatioMode = options.keepAspectRatio
                                                 ? Qt::KeepAspectRatio
                                                 : Qt::IgnoreAspectRatio;
    return image.scaled(targetWidth,
                        targetHeight,
                        aspectRatioMode,
                        Qt::SmoothTransformation);
}

QString VisionEncoder::ImageFormatName(VISION_ENCODE_FORMAT format)
{
    switch (format)
    {
    case VISION_ENCODE_FORMAT::RAW_PNG:
    case VISION_ENCODE_FORMAT::BASE64_PNG:
        return QStringLiteral("PNG");

    case VISION_ENCODE_FORMAT::RAW_JPEG:
    case VISION_ENCODE_FORMAT::BASE64_JPEG:
        return QStringLiteral("JPG");
    }

    return QString();
}

} // namespace vpet
