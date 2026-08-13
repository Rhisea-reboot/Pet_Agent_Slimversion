#include "vpet/perception/perception_pipeline.h"
#include "vpet/perception/vision_encoder.h"

#include <QSignalSpy>
#include <QtTest>

class PerceptionPipelineTest : public QObject
{
    Q_OBJECT

private slots:
    void EncodesScaledJpegInWorker();
    void SuppressesUnchangedFrames();
};

void PerceptionPipelineTest::EncodesScaledJpegInWorker()
{
    QImage image(2400, 1600, QImage::Format_RGB32);
    image.fill(Qt::red);
    vpet::VisionEncoder::_tagEncodeOptions options;
    options.maxWidth = 1200;
    options.maxHeight = 1200;
    options.quality = 70;

    const QByteArray encoded = vpet::VisionEncoder::Encode(
        image,
        vpet::VisionEncoder::VISION_ENCODE_FORMAT::BASE64_JPEG,
        options);
    QVERIFY(!encoded.isEmpty());

    const QByteArray jpeg = QByteArray::fromBase64(encoded);
    QImage decoded;
    QVERIFY(decoded.loadFromData(jpeg, "JPG"));
    QCOMPARE(decoded.size(), QSize(1200, 800));
}

void PerceptionPipelineTest::SuppressesUnchangedFrames()
{
    vpet::PerceptionPipeline::_tagConfig config;
    config.enableBuffer = false;
    config.enableChangeDetection = true;
    config.minDispatchIntervalMs = 0;
    config.encodeOptions.maxWidth = 320;
    config.encodeOptions.maxHeight = 320;
    config.encodeOptions.quality = 70;

    vpet::PerceptionPipeline pipeline(config);
    QSignalSpy dataReadySpy(&pipeline, &vpet::PerceptionPipeline::DataReady);
    QVERIFY(pipeline.Start());
    QTRY_VERIFY(dataReadySpy.count() >= 1);
    dataReadySpy.clear();

    QPixmap red(640, 480);
    red.fill(Qt::red);
    QVERIFY(QMetaObject::invokeMethod(&pipeline,
                                      "OnFrameCaptured",
                                      Qt::DirectConnection,
                                      Q_ARG(QPixmap, red),
                                      Q_ARG(int, 1),
                                      Q_ARG(QSize, red.size())));
    QTRY_COMPARE(dataReadySpy.count(), 1);

    QVERIFY(QMetaObject::invokeMethod(&pipeline,
                                      "OnFrameCaptured",
                                      Qt::DirectConnection,
                                      Q_ARG(QPixmap, red),
                                      Q_ARG(int, 2),
                                      Q_ARG(QSize, red.size())));
    QTest::qWait(50);
    QCOMPARE(dataReadySpy.count(), 1);

    QPixmap blue(640, 480);
    blue.fill(Qt::blue);
    QVERIFY(QMetaObject::invokeMethod(&pipeline,
                                      "OnFrameCaptured",
                                      Qt::DirectConnection,
                                      Q_ARG(QPixmap, blue),
                                      Q_ARG(int, 3),
                                      Q_ARG(QSize, blue.size())));
    QTRY_COMPARE(dataReadySpy.count(), 2);
}

QTEST_MAIN(PerceptionPipelineTest)

#include "perception_pipeline_test.moc"
