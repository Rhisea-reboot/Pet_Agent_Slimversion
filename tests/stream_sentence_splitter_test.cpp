#include "vpet/stream_sentence_splitter.h"

#include <QSignalSpy>
#include <QtTest>

using namespace vpet;

class StreamSentenceSplitterTest : public QObject
{
    Q_OBJECT

private slots:
    void SplitsStrongChinesePunctuation();
    void SplitsFirstSentenceAtSoftPause();
    void UsesLongerThresholdAfterFirstSentence();
    void SplitsNewlineAcrossDeltas();
    void ForceSplitWaitsForPunctuation();
    void ForceSplitSplitsAtLaterPunctuation();
    void ForceSplitFallsBackToEarliestPunctuation();
    void FlushesShortTailAsLastSentence();
};

void StreamSentenceSplitterTest::SplitsStrongChinesePunctuation()
{
    StreamSentenceSplitter splitter;
    QSignalSpy spy(&splitter, &StreamSentenceSplitter::SentenceReady);

    splitter.AppendDelta(1, QString::fromUtf8("你好世界。下一句"));

    QCOMPARE(spy.count(), 1);
    const SentenceChunk chunk = qvariant_cast<SentenceChunk>(spy.takeFirst().at(0));
    QCOMPARE(chunk.requestId, 1);
    QCOMPARE(chunk.index, 0);
    QCOMPARE(chunk.text, QString::fromUtf8("你好世界。"));
    QVERIFY(chunk.isFirst);
    QVERIFY(!chunk.isLast);
}

void StreamSentenceSplitterTest::SplitsFirstSentenceAtSoftPause()
{
    StreamSentenceSplitter splitter;
    QSignalSpy spy(&splitter, &StreamSentenceSplitter::SentenceReady);

    splitter.AppendDelta(2, QString::fromUtf8("让我想想看，后面还有内容"));

    QCOMPARE(spy.count(), 1);
    const SentenceChunk chunk = qvariant_cast<SentenceChunk>(spy.takeFirst().at(0));
    QCOMPARE(chunk.text, QString::fromUtf8("让我想想看，"));
    QVERIFY(chunk.isFirst);
}

void StreamSentenceSplitterTest::UsesLongerThresholdAfterFirstSentence()
{
    StreamSentenceSplitter splitter;
    QSignalSpy spy(&splitter, &StreamSentenceSplitter::SentenceReady);

    splitter.AppendDelta(3, QString::fromUtf8("第一句话。短短一句，仍然继续积累到足够长度以后，"));

    QCOMPARE(spy.count(), 2);
    const SentenceChunk second = qvariant_cast<SentenceChunk>(spy.at(1).at(0));
    QVERIFY(!second.isFirst);
    QVERIFY(second.text.size() >= 15);
}

void StreamSentenceSplitterTest::SplitsNewlineAcrossDeltas()
{
    StreamSentenceSplitter splitter;
    QSignalSpy spy(&splitter, &StreamSentenceSplitter::SentenceReady);

    splitter.AppendDelta(4, QStringLiteral("line"));
    splitter.AppendDelta(4, QStringLiteral(" one\nnext"));

    QCOMPARE(spy.count(), 1);
    const SentenceChunk chunk = qvariant_cast<SentenceChunk>(spy.takeFirst().at(0));
    QCOMPARE(chunk.text, QStringLiteral("line one"));
}

void StreamSentenceSplitterTest::ForceSplitWaitsForPunctuation()
{
    StreamSentenceSplitter splitter;
    QSignalSpy spy(&splitter, &StreamSentenceSplitter::SentenceReady);
    const QString longText(40, QLatin1Char('a'));

    splitter.AppendDelta(5, longText);

    QCOMPARE(spy.count(), 0);

    splitter.AppendDelta(5, QString::fromUtf8("，"));

    QCOMPARE(spy.count(), 1);
    const SentenceChunk chunk = qvariant_cast<SentenceChunk>(spy.takeFirst().at(0));
    QCOMPARE(chunk.text, longText + QString::fromUtf8("，"));
}

void StreamSentenceSplitterTest::ForceSplitSplitsAtLaterPunctuation()
{
    StreamSentenceSplitter splitter;
    QSignalSpy spy(&splitter, &StreamSentenceSplitter::SentenceReady);

    splitter.AppendDelta(7, QString::fromUtf8("好，"));
    splitter.AppendDelta(7, QString(36, QLatin1Char('c')));
    splitter.AppendDelta(7, QString::fromUtf8("，尾句"));

    QCOMPARE(spy.count(), 1);
    const SentenceChunk chunk = qvariant_cast<SentenceChunk>(spy.takeFirst().at(0));
    QCOMPARE(chunk.text, QString::fromUtf8("好，") + QString(36, QLatin1Char('c'))
                         + QString::fromUtf8("，"));
}

void StreamSentenceSplitterTest::ForceSplitFallsBackToEarliestPunctuation()
{
    StreamSentenceSplitter splitter;
    QSignalSpy spy(&splitter, &StreamSentenceSplitter::SentenceReady);

    splitter.AppendDelta(8, QString::fromUtf8("好好好，"));
    splitter.AppendDelta(8, QString(32, QLatin1Char('d')));

    QCOMPARE(spy.count(), 1);
    const SentenceChunk chunk = qvariant_cast<SentenceChunk>(spy.takeFirst().at(0));
    QCOMPARE(chunk.text, QString::fromUtf8("好好好，"));
}

void StreamSentenceSplitterTest::FlushesShortTailAsLastSentence()
{
    StreamSentenceSplitter splitter;
    QSignalSpy spy(&splitter, &StreamSentenceSplitter::SentenceReady);

    splitter.AppendDelta(6, QString::fromUtf8("好的"));
    splitter.FinalizeStream(6);

    QCOMPARE(spy.count(), 1);
    const SentenceChunk chunk = qvariant_cast<SentenceChunk>(spy.takeFirst().at(0));
    QCOMPARE(chunk.text, QString::fromUtf8("好的"));
    QVERIFY(chunk.isFirst);
    QVERIFY(chunk.isLast);
}

QTEST_MAIN(StreamSentenceSplitterTest)
#include "stream_sentence_splitter_test.moc"
