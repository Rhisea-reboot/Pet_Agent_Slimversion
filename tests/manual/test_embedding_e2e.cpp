// 端到端验证：真实 BGE ONNX 模型 + SQLite 向量库 + MemoryService 级联检索
//
// 用法: memory_embedding_e2e.exe [模型目录]
// 模型目录默认: <repo>/models/embedding/bge-small-zh-v1.5
//
// 验证内容:
//   1. 模型加载、输出维度为 512、L2 归一化
//   2. 语义相近句子的余弦相似度高于无关句子
//   3. 查询指令前缀检索排序正确
//   4. SQLite 向量库 upsert / query / get / remove 往返
//   5. MemoryService 级联检索：向量种子 + 图 BFS 扩展
//
// 本程序需要真实模型文件，未注册进 CTest；无模型环境跳过。

#include "vpet/memory/embedding_client.h"
#include "vpet/memory/memory_service.h"
#include "vpet/memory/vector_store.h"

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cmath>
#include <iostream>

namespace
{

const QString DEFAULT_MODEL_DIR = QStringLiteral("F:/Pet Agent/models/embedding/bge-small-zh-v1.5");
const QString MODEL_ID = QStringLiteral("BAAI/bge-small-zh-v1.5");

int s_failures = 0;

void Check(bool condition, const char *name)
{
    if (!condition)
    {
        s_failures += 1;
        std::cerr << "[FAIL] " << name << std::endl;
        return;
    }

    std::cout << "[PASS] " << name << std::endl;
}

float Cosine(const QVector<float> &lhs, const QVector<float> &rhs)
{
    if ((lhs.size() != rhs.size()) || lhs.isEmpty())
    {
        return 0.0f;
    }

    float dot = 0.0f;

    for (int index = 0; index < lhs.size(); ++index)
    {
        dot += lhs.at(index) * rhs.at(index);
    }

    return dot;
}

bool WaitForRequestResult(vpet::MemoryService &service,
                          const QString &petId,
                          const QString &triggerType,
                          quint64 requestId,
                          vpet::MemoryService::_tagRetrieveResult &result,
                          int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeoutMs)
    {
        vpet::MemoryService::_tagRetrieveResult candidate;

        if (service.TakeLatestReadyResult(petId, triggerType, candidate)
            && (candidate.requestId == requestId))
        {
            result = candidate;
            return true;
        }

        QCoreApplication::processEvents();
        QThread::msleep(5);
    }

    return false;
}

} // anonymous namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);

    const QString modelDir = (argc > 1) ? QString::fromLocal8Bit(argv[1]) : DEFAULT_MODEL_DIR;

    std::cout << "Model dir: " << modelDir.toStdString() << std::endl;

    vpet::EmbeddingConfig config;
    config.enabled = true;
    config.backend = QStringLiteral("local_onnx");
    config.model = MODEL_ID;
    config.modelDir = modelDir;
    config.onnxModel = QStringLiteral("onnx/model.onnx");
    config.tokenizerFile = QStringLiteral("tokenizer.json");

    vpet::EmbeddingClient client(config);

    std::cout << "[1/5] 模型加载" << std::endl;
    Check(client.Reload(), "模型加载成功 (Reload)");
    Check(client.IsReady(), "客户端就绪 (IsReady)");
    Check(client.Dimension() == 512, "输出维度为 512");
    Check(client.ModelId() == MODEL_ID, "模型 ID 正确");
    Check(client.LastError().isEmpty(), "加载后无诊断错误");

    if (!client.IsReady())
    {
        std::cerr << "Model unavailable; aborting end-to-end verification." << std::endl;
        std::cerr << "Last error: " << client.LastError().toStdString() << std::endl;
        return 1;
    }

    std::cout << "[2/5] 语义相似度" << std::endl;

    QVector<float> vectorCoffee;
    QVector<float> vectorTea;
    QVector<float> vectorHiking;
    QVector<float> queryCoffee;

    Check(client.EmbedDocument(QStringLiteral("用户喜欢喝咖啡"), vectorCoffee),
          "文档向量化: 喝咖啡");
    Check(client.EmbedDocument(QStringLiteral("用户爱喝茶"), vectorTea),
          "文档向量化: 喝茶");
    Check(client.EmbedDocument(QStringLiteral("今天天气晴朗适合爬山"), vectorHiking),
          "文档向量化: 爬山");
    Check(client.EmbedQuery(QStringLiteral("咖啡"), queryCoffee),
          "查询向量化: 咖啡");

    if (vectorCoffee.size() == 512)
    {
        float squaredSum = 0.0f;

        for (const float value : vectorCoffee)
        {
            squaredSum += value * value;
        }

        Check(std::fabs(std::sqrt(squaredSum) - 1.0f) < 1e-3f,
              "文档向量已 L2 归一化");
    }

    const float coffeeTea = Cosine(vectorCoffee, vectorTea);
    const float coffeeHiking = Cosine(vectorCoffee, vectorHiking);
    const float coffeeHikingQuery = Cosine(queryCoffee, vectorHiking);

    std::cout << "  cos(咖啡, 茶)   = " << coffeeTea << std::endl;
    std::cout << "  cos(咖啡, 爬山) = " << coffeeHiking << std::endl;
    std::cout << "  cos(查询咖啡, 爬山) = " << coffeeHikingQuery << std::endl;

    Check(coffeeTea > coffeeHiking, "语义相近句子得分高于无关句子");

    std::cout << "[3/5] 查询指令前缀" << std::endl;

    const float queryCoffeeScore = Cosine(queryCoffee, vectorCoffee);
    std::cout << "  cos(查询咖啡, 咖啡文档) = " << queryCoffeeScore << std::endl;
    Check(queryCoffeeScore > coffeeHikingQuery, "带指令查询优先命中相关文档");

    std::cout << "[4/5] SQLite 向量库往返" << std::endl;

    QTemporaryDir storeDirectory;
    Check(storeDirectory.isValid(), "临时目录可用");

    vpet::VectorStore store;
    QString storeError;

    Check(store.Open(storeDirectory.path() + QStringLiteral("/vectors.sqlite3"), storeError),
          "向量库打开");

    if (store.IsOpen())
    {
        const int dimension = client.Dimension();

        Check(store.Upsert(QStringLiteral("mem_coffee"),
                           MODEL_ID,
                           dimension,
                           vectorCoffee,
                           storeError),
              "写入咖啡向量");
        Check(store.Upsert(QStringLiteral("mem_tea"),
                           MODEL_ID,
                           dimension,
                           vectorTea,
                           storeError),
              "写入茶向量");
        Check(store.Upsert(QStringLiteral("mem_hiking"),
                           MODEL_ID,
                           dimension,
                           vectorHiking,
                           storeError),
              "写入爬山向量");
        Check(store.Count(MODEL_ID) == 3, "向量条数为 3");

        QVector<vpet::VectorStore::_tagVectorHit> hits;

        Check(store.QueryTopK(MODEL_ID, queryCoffee, 3, hits, storeError),
              "top-3 查询执行");
        Check(hits.size() == 3, "top-3 命中 3 条");

        if (!hits.isEmpty())
        {
            std::cout << "  top-1: " << hits.at(0).entryId.toStdString()
                      << " score=" << hits.at(0).score << std::endl;
            Check(hits.at(0).entryId == QStringLiteral("mem_coffee"),
                  "top-1 命中咖啡记忆");
        }

        QString loadedModel;
        int loadedDimension = 0;
        QVector<float> loadedVector;
        Check(store.Get(QStringLiteral("mem_tea"),
                        loadedModel,
                        loadedDimension,
                        loadedVector,
                        storeError),
              "读取茶向量");
        Check((loadedModel == MODEL_ID)
                  && (loadedDimension == dimension)
                  && (loadedVector == vectorTea),
              "读回向量与写入一致");

        Check(store.Remove(QStringLiteral("mem_hiking"), storeError), "删除爬山向量");
        Check(store.Count(MODEL_ID) == 2, "删除后向量条数为 2");

        store.Close();
        Check(!store.IsOpen(), "向量库关闭");
    }

    std::cout << "[5/5] MemoryService 级联检索" << std::endl;

    QTemporaryDir serviceDirectory;
    Check(serviceDirectory.isValid(), "服务临时目录可用");

    {
        vpet::MemoryService service;
        QString serviceError;
        service.SetEmbeddingConfig(config, serviceError);

        Check(service.Start(serviceDirectory.path(), 16, serviceError), "服务启动");

        vpet::MemoryEntry coffee;
        coffee.content = QStringLiteral("用户喜欢喝咖啡");
        coffee.petId = QStringLiteral("pet_e2e");

        vpet::MemoryEntry tea;
        tea.content = QStringLiteral("用户爱喝茶");
        tea.petId = QStringLiteral("pet_e2e");

        vpet::MemoryEntry unrelated;
        unrelated.content = QStringLiteral("用户的电脑重置了");
        unrelated.petId = QStringLiteral("pet_e2e");

        quint64 requestId = 0;
        QString rejectCategory;
        Check(service.TryEnqueueStore(QStringLiteral("pet_e2e"),
                                      QStringLiteral("user"),
                                      coffee,
                                      requestId,
                                      rejectCategory),
              "存储咖啡记忆");
        const QString coffeeId = QStringLiteral("mem_%1").arg(requestId);
        Check(service.TryEnqueueStore(QStringLiteral("pet_e2e"),
                                      QStringLiteral("user"),
                                      tea,
                                      requestId,
                                      rejectCategory),
              "存储茶记忆");
        Check(service.TryEnqueueStore(QStringLiteral("pet_e2e"),
                                      QStringLiteral("user"),
                                      unrelated,
                                      requestId,
                                      rejectCategory),
              "存储无关记忆");

        Check(service.TryEnqueueTag(QStringLiteral("pet_e2e"),
                                    QStringLiteral("user"),
                                    coffeeId,
                                    { QStringLiteral("喜好") },
                                    requestId),
              "给咖啡记忆打标签");

        Check(service.TryEnqueueRetrieve(QStringLiteral("pet_e2e"),
                                         QStringLiteral("user"),
                                         QStringLiteral("咖啡"),
                                         requestId),
              "提交检索任务");
        const quint64 retrieveId = requestId;

        vpet::MemoryService::_tagRetrieveResult result;
        Check(WaitForRequestResult(service,
                                   QStringLiteral("pet_e2e"),
                                   QStringLiteral("user"),
                                   retrieveId,
                                   result,
                                   15000),
              "检索结果返回");

        if (result.ok && !result.entries.isEmpty())
        {
            std::cout << "  命中 " << result.entries.size() << " 条:";
            int coffeeIndex = -1;
            int unrelatedIndex = -1;

            for (int index = 0; index < result.entries.size(); ++index)
            {
                std::cout << " [" << result.entries.at(index).content.toStdString() << "]";

                if (result.entries.at(index).content == QStringLiteral("用户喜欢喝咖啡"))
                {
                    coffeeIndex = index;
                }

                if (result.entries.at(index).content == QStringLiteral("用户的电脑重置了"))
                {
                    unrelatedIndex = index;
                }
            }

            std::cout << std::endl;
            Check(coffeeIndex >= 0, "级联检索命中咖啡记忆");
            Check((unrelatedIndex < 0) || (coffeeIndex >= 0 && unrelatedIndex > coffeeIndex),
                  "无关记忆未排在咖啡记忆之前");
        }

        service.Shutdown(3000);
    }

    std::cout << std::endl;

    if (s_failures == 0)
    {
        std::cout << "RESULT: ALL PASS" << std::endl;
        return 0;
    }

    std::cout << "RESULT: " << s_failures << " FAILURE(S)" << std::endl;
    return 1;
}
