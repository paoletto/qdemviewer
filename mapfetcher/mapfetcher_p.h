/****************************************************************************
**
** Copyright (C) 2024- Paolo Angelelli <paoletto@gmail.com>
**
** Commercial License Usage
** Licensees holding a valid commercial qdemviewer license may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement with the copyright holder. For licensing terms
** and conditions and further information contact the copyright holder.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3. The licenses are as published by
** the Free Software Foundation at https://www.gnu.org/licenses/gpl-3.0.html,
** with the exception that the use of this work for training artificial intelligence
** is prohibited for both commercial and non-commercial use.
**
****************************************************************************/

#ifndef MAPFETCHER_P_H
#define MAPFETCHER_P_H

#include "mapfetcher.h"
#include "tilecache_p.h"

#include <QtCore/private/qobject_p.h>
#include <QQueue>
#include <QThread>
#include <QTimer>
#include <QCoreApplication>
#include <QtLocation/private/qgeotilespec_p.h>
#include <unordered_map>
#include <map>
#include <set>
#include <queue>
#include <unordered_set>
#include <private/qtexturefiledata_p.h>

using HeightmapCache = std::unordered_map<TileKey, std::shared_ptr<Heightmap>>;
using TileNeighborsMap = std::unordered_map<TileKey,
                            std::pair< Heightmap::Neighbors,
                                       std::map<Heightmap::Neighbor, std::shared_ptr<QImage>>>>;
using TileCache = std::unordered_map<TileKey, std::shared_ptr<QImage>>;
using TileCacheCache = std::unordered_map<TileKey, std::set<TileData>>;
using TileCacheASTC = std::unordered_map<TileKey, std::shared_ptr<CompressedTextureData>>;

struct ThreadedJobData {
    enum class JobType {
        Invalid = 0,
        TileReply = 1,
        CachedCompoundTile = 2,
        DEMTileReply = 3,
        DEMReady = 4,
        Raster2ASTC = 5,
        ASTCTileReply = 6
    };

    virtual ~ThreadedJobData() {}
    virtual JobType type() const { return JobType::Invalid; }
    virtual int priority() const = 0;

protected:
    ThreadedJobData() {}
};

class ThreadedJob : public QObject
{
    Q_OBJECT
public:
    ThreadedJob();
    ~ThreadedJob() override = default;

    void move2thread(QThread &t);

    static ThreadedJob *fromData(ThreadedJobData *);

public slots:
    virtual void process() = 0;

signals:
    void finished();
    void start();
    void error();
    void jobDestroyed(QObject *, QThread *);

private slots:
    void onDestroyed(QObject *);

friend class ThreadedJobQueue;
};

class ThreadedJobQueue: public QObject
{
Q_OBJECT
public:
    ThreadedJobQueue(size_t numThread = 1, QObject *parent = nullptr);
    ~ThreadedJobQueue() override;

    void schedule(ThreadedJobData *data);

protected slots:
    void onJobDestroyed(QObject *, QThread *);

protected:
    void next(QThread *t);
    QThread *findIdle();

    struct JobComparator {
        bool operator()(const ThreadedJobData * l, const ThreadedJobData * r) const;
    };
    std::priority_queue<ThreadedJobData*, std::vector<ThreadedJobData *>,  JobComparator> m_jobs;
    std::unordered_map<QThread *, QObject *> m_currentJobs;
    std::vector<QThread *> m_threads;
};

class ThrottledNetworkFetcher : public QObject
{
Q_OBJECT
public:
    ThrottledNetworkFetcher(QObject *parent = nullptr, size_t maxConcurrentRequests = 300);
    ~ThrottledNetworkFetcher() = default;

    void requestTile(const QUrl &request,
                     const TileKey &k,
                     const quint8 destinationZoom,
                     const quint64 id,
                     const bool coverageRequest,
                     const Heightmap::Neighbors boundaries,
                     QObject *destFinished,
                     const char *onFinished,
                     QObject *destError = nullptr,
                     const char *onErrorSlot = nullptr);

protected slots:
    void onFinished();

protected:
    void request(const QUrl &u,
                 const TileKey &k,
                 const quint8 destinationZoom,
                 const quint64 id, const bool coverage,
                 const Heightmap::Neighbors boundaries,
                 QObject *destFinished,
                 const char *onFinishedSlot,
                 QObject *destError,
                 const char *onErrorSlot);

    QNetworkAccessManager &m_nm;
    size_t m_maxConcurrent;
    size_t m_active{0};

    QQueue<std::tuple<QUrl, TileKey, quint8, quint64, bool, quint32,
                      QObject *, std::string,
                      QObject *, std::string>> m_pendingRequests;
};

class MapFetcherPrivate :  public QObjectPrivate
{
    Q_DECLARE_PUBLIC(MapFetcher)

public:
    MapFetcherPrivate();
    ~MapFetcherPrivate() override;

    std::shared_ptr<QImage> tile(quint64 id, const TileKey k);
    virtual quint64 requestSlippyTiles(const QList<QGeoCoordinate> &crds,
                                       const quint8 zoom,
                                       quint8 destinationZoom,
                                       bool compound);

    virtual quint64 requestCoverage(const QList<QGeoCoordinate> &crds,
                                    const quint8 zoom,
                                    bool clip);

    QString objectName() const;

    QString m_urlTemplate;
    int m_maximumZoomLevel{19};
    bool m_overzoom{false};

    std::map<quint64, TileCache> m_tileCache;
    std::map<quint64, std::shared_ptr<QImage>> m_coverages;
    const QImage m_empty;
};

class DEMFetcherPrivate :  public MapFetcherPrivate
{
    Q_DECLARE_PUBLIC(DEMFetcher)

public:
    DEMFetcherPrivate();
    ~DEMFetcherPrivate() override;

    quint64 requestSlippyTiles(const QList<QGeoCoordinate> &crds,
                               const quint8 zoom,
                               quint8 destinationZoom,
                               bool) override;

    quint64 requestCoverage(const QList<QGeoCoordinate> &crds,
                            const quint8 zoom,
                            bool clip) override;

    std::map<quint64, HeightmapCache> m_heightmapCache;
    std::map<quint64, std::shared_ptr<Heightmap>> m_heightmapCoverages;
    bool m_borders{true};
};

struct ASTCCompressedTextureData : public CompressedTextureData {
    ASTCCompressedTextureData() = default;
    ~ASTCCompressedTextureData() override = default;

    quint64 upload(QSharedPointer<QOpenGLTexture> &t) override;
    quint64 uploadTo2DArray(QSharedPointer<QOpenGLTexture> &texArray,
                                      int layer,
                                      int layers) override;
    QSize size() const override;
    bool hasCompressedData() const override;

    static std::shared_ptr<ASTCCompressedTextureData> fromImage(
                                        const std::shared_ptr<QImage> &i,
                                        qint64 x,
                                        qint64 y,
                                        qint64 z,
                                        QByteArray md5);

    std::shared_ptr<QImage> m_image;
    std::vector<QTextureFileData> m_mips;

    void initStatics();

    static std::vector<QImage> m_white256;
    static std::vector<QTextureFileData> m_white8x8ASTC;
    static std::vector<QTextureFileData> m_transparent8x8ASTC;
};

class ASTCFetcherPrivate :  public MapFetcherPrivate
{
    Q_DECLARE_PUBLIC(ASTCFetcher)

public:
    ASTCFetcherPrivate() = default;
    ~ASTCFetcherPrivate() override = default;

    std::shared_ptr<CompressedTextureData> tileASTC(quint64 id, const TileKey k);
    quint64 requestSlippyTiles(const QList<QGeoCoordinate> &crds,
                               const quint8 zoom,
                               quint8 destinationZoom,
                               bool compound) override;

    quint64 requestCoverage(const QList<QGeoCoordinate> &crds,
                            const quint8 zoom,
                            bool clip) override;

    std::map<quint64, TileCacheASTC> m_tileCacheASTC;
    std::map<quint64, std::shared_ptr<CompressedTextureData>> m_coveragesASTC;
    QAtomicInt m_forwardUncompressed{false};
};

class MapFetcherWorkerPrivate;
class MapFetcherWorker : public QObject {
    Q_DECLARE_PRIVATE(MapFetcherWorker)
    Q_OBJECT

public:
    MapFetcherWorker(QObject *parent
                     ,MapFetcher *f
                     ,QSharedPointer<ThreadedJobQueue> worker
//                     ,bool emitHashes = false
                    );
    ~MapFetcherWorker() override = default;

    void requestSlippyTiles(quint64 requestId,
                            const QList<QGeoCoordinate> &crds,
                            const quint8 zoom,
                            quint8 destinationZoom,
                            bool compound = true);

    void requestCoverage(quint64 requestId,
                            const QList<QGeoCoordinate> &crds,
                            const quint8 zoom,
                            const bool clip = false);

    std::shared_ptr<QImage> tile(quint64 requestId, const TileKey &k);

    void setURLTemplate(const QString &urlTemplate);

signals:
    void tileReady(quint64 id,
                   const TileKey k,
                   std::shared_ptr<QImage>,
                   QByteArray md5 = {});
    void compressedTileDataReady(quint64 id,
                                 const TileKey k,
                                 std::shared_ptr<QByteArray>);
    void coverageReady(quint64 id,
                       std::shared_ptr<QImage>);
    void requestHandlingFinished(quint64 id);

protected slots:
    void onTileReplyFinished();
    void onTileReplyForCoverageFinished();
    void onInsertTile(const quint64 id, const TileKey k, std::shared_ptr<QImage> i, QByteArray md5);
    void onInsertCompressedTileData(const quint64 id, const TileKey k, std::shared_ptr<QByteArray> data);
    void onInsertCoverage(const quint64 id, std::shared_ptr<QImage> i);
    void networkReplyError(QNetworkReply::NetworkError);

protected:
    MapFetcherWorker(MapFetcherWorkerPrivate &dd, MapFetcher *f, QSharedPointer<ThreadedJobQueue> worker, QObject *parent = nullptr);

private:
    Q_DISABLE_COPY(MapFetcherWorker)

friend class TileReplyHandler;
friend class NetworkIOManager;
};
class MapFetcherWorkerPrivate :  public QObjectPrivate
{
    Q_DECLARE_PUBLIC(MapFetcherWorker)

public:
    MapFetcherWorkerPrivate() = default;
    ~MapFetcherWorkerPrivate() override = default;

    std::shared_ptr<QImage> tile(quint64 requestId, const TileKey &k);
    std::shared_ptr<QImage> peekTile(quint64 requestId, const TileKey &k);
    virtual void trackNeighbors(quint64,
                                const TileKey &,
                                Heightmap::Neighbors,
                                quint8) {}
    QString objectName() const;

    QString m_urlTemplate;
    ThrottledNetworkFetcher m_nm;
    std::unordered_map<quint64, TileCache> m_tileCache;
    std::unordered_map<quint64, TileCacheCache> m_tileCacheCache;

    std::unordered_map<quint64, std::tuple<QList<QGeoCoordinate>,  // polygon
                                 quint8,          // zoom
                                 quint64,         // numTiles
                                 bool             // clip
                                >> m_requests;
    std::unordered_map<quint64, std::set<TileData>> m_tileSets;

    QSharedPointer<ThreadedJobQueue> m_worker; // TODO: figure how to use a qthreadpool and move qobjects to it
    MapFetcher *m_fetcher{nullptr};
    std::unordered_map<quint64, qint64> m_request2remainingTiles;
    std::unordered_map<quint64, qint64> m_request2remainingHandlers;
    std::unordered_map<quint64, QString> m_request2urlTemplate;
    std::unordered_map<quint64, quint64> m_request2sourceZoom;
    QString m_diskCachePath;

    const QImage m_empty;
};

class DEMFetcherWorkerPrivate;
class DEMFetcherWorker : public MapFetcherWorker {
    Q_DECLARE_PRIVATE(DEMFetcherWorker)
    Q_OBJECT

public:
    DEMFetcherWorker(QObject *parent, DEMFetcher *f, QSharedPointer<ThreadedJobQueue> worker, bool borders=false);
    ~DEMFetcherWorker() override = default;

    std::shared_ptr<Heightmap> heightmap(const TileKey k);
    std::shared_ptr<Heightmap> heightmapCoverage(quint64 id);

signals:
    void heightmapReady(quint64 id, const TileKey k, std::shared_ptr<Heightmap>);
    void heightmapCoverageReady(quint64 id, std::shared_ptr<Heightmap>);

protected slots:
    void onTileReady(quint64 id, const TileKey k,  std::shared_ptr<QImage> i);
    void onCoverageReady(quint64 id,  std::shared_ptr<QImage> i);
    void onInsertHeightmap(quint64 id, const TileKey k, std::shared_ptr<Heightmap> h);
    void onInsertHeightmapCoverage(quint64 id, std::shared_ptr<Heightmap> h);

protected:
//    DEMFetcherWorker(DEMFetcherWorkerPrivate &dd, QObject *parent = nullptr);
    void init();
private:
    Q_DISABLE_COPY(DEMFetcherWorker)
friend class DEMReadyHandler;
friend class NetworkIOManager;
friend class MapFetcherWorker;
};
class DEMFetcherWorkerPrivate :  public MapFetcherWorkerPrivate
{
    Q_DECLARE_PUBLIC(DEMFetcherWorker)
public:
    DEMFetcherWorkerPrivate();
    ~DEMFetcherWorkerPrivate() override = default;

    void trackNeighbors(quint64 id,
                        const TileKey &k,
                        Heightmap::Neighbors n,
                        quint8 destinationZoom) override;

    void insertNeighbors(quint64 id,
                         const TileKey &k,
                         Heightmap::Neighbors n,
                         std::map<Heightmap::Neighbor, std::shared_ptr<QImage>> boundaryRasters);

    std::unordered_map<quint64, TileNeighborsMap> m_request2Neighbors;
    std::unordered_map<quint64, HeightmapCache> m_heightmapCache;
    std::unordered_map<quint64, std::shared_ptr<Heightmap>> m_heightmapCoverages;
    std::unordered_map<quint64, qint64> m_request2remainingDEMHandlers;
    bool m_borders{false};
};

class ASTCFetcherWorkerPrivate;
class ASTCFetcherWorker : public MapFetcherWorker {
    Q_DECLARE_PRIVATE(ASTCFetcherWorker)
    Q_OBJECT

public:
    ASTCFetcherWorker(QObject *parent,
                      ASTCFetcher *f,
                      QSharedPointer<ThreadedJobQueue> worker,
                      QSharedPointer<ThreadedJobQueue> workerASTC);
    ~ASTCFetcherWorker() override = default;

    Q_INVOKABLE void setForwardUncompressed(bool enabled);
    bool forwardUncompressed() const;

signals:
    void tileASTCReady(quint64 id, const TileKey k, std::shared_ptr<CompressedTextureData>);
    void coverageASTCReady(quint64 id, std::shared_ptr<CompressedTextureData>);

protected slots:
    void onTileReady(quint64 id, const TileKey k,  std::shared_ptr<QImage> i, QByteArray md5);
    void onCompressedTileDataReady(quint64 id, const TileKey k,  std::shared_ptr<QByteArray> d);
    void onCoverageReady(quint64 id,  std::shared_ptr<QImage> i);
    void onInsertTileASTC(quint64 id, const TileKey k, std::shared_ptr<CompressedTextureData> h);
    void onInsertCoverageASTC(quint64 id, std::shared_ptr<CompressedTextureData> h);

protected:
    void init();

private:
    Q_DISABLE_COPY(ASTCFetcherWorker)
friend class Raster2ASTCHandler;
friend class NetworkIOManager;
friend class MapFetcherWorker;
};

class ASTCFetcherWorkerPrivate :  public MapFetcherWorkerPrivate
{
    Q_DECLARE_PUBLIC(ASTCFetcherWorker)
public:
    ASTCFetcherWorkerPrivate() = default;
    ~ASTCFetcherWorkerPrivate() override = default;

    bool m_forwardUncompressed{false};
    std::unordered_map<quint64, qint64> m_request2remainingASTCHandlers;
    QSharedPointer<ThreadedJobQueue> m_workerASTC;
};

class NetworkIOManager: public QObject //living in a separate thread
{
    Q_OBJECT

public:
    NetworkIOManager() = default;
    ~NetworkIOManager() override = default;

public slots:
    void addURLTemplate(const QString urlTemplate);
    void requestSlippyTiles(MapFetcher *mapFetcher,
                            quint64 requestId,
                            const QList<QGeoCoordinate> &crds,
                            const quint8 zoom,
                            quint8 destinationZoom,
                            bool compound);

    void requestCoverage(MapFetcher *mapFetcher,
                            quint64 requestId,
                            const QList<QGeoCoordinate> &crds,
                            const quint8 zoom,
                            const bool clip = false);


    void requestSlippyTiles(DEMFetcher *demFetcher,
                            quint64 requestId,
                            const QList<QGeoCoordinate> &crds,
                            const quint8 zoom,
                            quint8 destinationZoom);

    void requestCoverage(DEMFetcher *demFetcher,
                            quint64 requestId,
                            const QList<QGeoCoordinate> &crds,
                            const quint8 zoom,
                            const bool clip = false);

    void requestSlippyTiles(ASTCFetcher *fetcher,
                            quint64 requestId,
                            const QList<QGeoCoordinate> &crds,
                            const quint8 zoom,
                            quint8 destinationZoom,
                            bool compound);

    void requestCoverage(ASTCFetcher *fetcher,
                            quint64 requestId,
                            const QList<QGeoCoordinate> &crds,
                            const quint8 zoom,
                            const bool clip = false);

    quint64 cacheSize();

    QString cachePath();

protected:
    void init() {
        if (!m_worker)
            m_worker = QSharedPointer<ThreadedJobQueue>(new ThreadedJobQueue);
    }

    MapFetcherWorker *getMapFetcherWorker(MapFetcher *f);

    DEMFetcherWorker *getDEMFetcherWorker(DEMFetcher *f);

    ASTCFetcherWorker *getASTCFetcherWorker(ASTCFetcher *f);

protected:
    QSharedPointer<ThreadedJobQueue> m_worker; // TODO: figure how to use a qthreadpool and move qobjects to it
    QSharedPointer<ThreadedJobQueue> m_workerASTC;

    std::unordered_map<MapFetcher *, MapFetcherWorker *> m_mapFetcher2Worker;
    std::unordered_map<DEMFetcher *, DEMFetcherWorker *> m_demFetcher2Worker;
    std::unordered_map<ASTCFetcher *, ASTCFetcherWorker *> m_astcFetcher2Worker;
};

struct LoopedThread : public QThread {
    LoopedThread(QObject *parent = nullptr) : QThread(parent) {}
    ~LoopedThread() override = default;

    void run() override {
        exec();
    }
};

class NetworkManager
{
public:
    static NetworkManager& instance()
    {
        static NetworkManager instance;
        return instance;
    }

    NetworkManager(NetworkManager const&) = delete;
    void operator=(NetworkManager const&) = delete;

    void addURLTemplate(const QString &urlTemplate) {
        QMetaObject::invokeMethod(m_manager.get(), "addURLTemplate", Qt::QueuedConnection
                                  , Q_ARG(QString , urlTemplate));
    }

    quint64 requestSlippyTiles(MapFetcher &mapFetcher,
                               const QList<QGeoCoordinate> &crds,
                               const quint8 zoom,
                               const quint8 destinationZoom,
                               bool compound) {
        const auto requestId = m_requestID++;
        QMetaObject::invokeMethod(m_manager.get(), "requestSlippyTiles", Qt::QueuedConnection
                                  , Q_ARG(MapFetcher *, &mapFetcher)
                                  , Q_ARG(qulonglong, requestId)
                                  , Q_ARG(QList<QGeoCoordinate>, crds)
                                  , Q_ARG(uchar, zoom)
                                  , Q_ARG(uchar, destinationZoom)
                                  , Q_ARG(bool, compound));
        return requestId;
    }

    quint64 requestCoverage(MapFetcher &mapFetcher,
                            const QList<QGeoCoordinate> &crds,
                            const quint8 zoom,
                            const bool clip = false) {
        auto requestId = m_requestID++;
        QMetaObject::invokeMethod(m_manager.get(), "requestCoverage", Qt::QueuedConnection
                                  , Q_ARG(MapFetcher *, &mapFetcher)
                                  , Q_ARG(qulonglong, requestId)
                                  , Q_ARG(QList<QGeoCoordinate>, crds)
                                  , Q_ARG(uchar, zoom)
                                  , Q_ARG(bool, clip));
        return requestId;
    }

    quint64 requestSlippyTiles(DEMFetcher &demFetcher,
                               const QList<QGeoCoordinate> &crds,
                               const quint8 zoom,
                               const quint8 destinationZoom) {
        const auto requestId = m_requestID++;
        QMetaObject::invokeMethod(m_manager.get(), "requestSlippyTiles", Qt::QueuedConnection
                                  , Q_ARG(DEMFetcher *, &demFetcher)
                                  , Q_ARG(qulonglong, requestId)
                                  , Q_ARG(QList<QGeoCoordinate>, crds)
                                  , Q_ARG(uchar, zoom)
                                  , Q_ARG(uchar, destinationZoom));
        return requestId;
    }

    quint64 requestCoverage(DEMFetcher &demFetcher,
                            const QList<QGeoCoordinate> &crds,
                            const quint8 zoom,
                            const bool clip = false) {
        auto requestId = m_requestID++;
        QMetaObject::invokeMethod(m_manager.get(), "requestCoverage", Qt::QueuedConnection
                                  , Q_ARG(DEMFetcher *, &demFetcher)
                                  , Q_ARG(qulonglong, requestId)
                                  , Q_ARG(QList<QGeoCoordinate>, crds)
                                  , Q_ARG(uchar, zoom)
                                  , Q_ARG(bool, clip));
        return requestId;
    }

    quint64 requestSlippyTiles(ASTCFetcher &fetcher,
                               const QList<QGeoCoordinate> &crds,
                               const quint8 zoom,
                               const quint8 destinationZoom,
                               bool compound) {
        const auto requestId = m_requestID++;
        QMetaObject::invokeMethod(m_manager.get(), "requestSlippyTiles", Qt::QueuedConnection
                                  , Q_ARG(ASTCFetcher *, &fetcher)
                                  , Q_ARG(qulonglong, requestId)
                                  , Q_ARG(QList<QGeoCoordinate>, crds)
                                  , Q_ARG(uchar, zoom)
                                  , Q_ARG(uchar, destinationZoom)
                                  , Q_ARG(bool, compound));
        return requestId;
    }

    quint64 requestCoverage(ASTCFetcher &fetcher,
                            const QList<QGeoCoordinate> &crds,
                            const quint8 zoom,
                            const bool clip = false) {
        auto requestId = m_requestID++;
        QMetaObject::invokeMethod(m_manager.get(), "requestCoverage", Qt::QueuedConnection
                                  , Q_ARG(ASTCFetcher *, &fetcher)
                                  , Q_ARG(qulonglong, requestId)
                                  , Q_ARG(QList<QGeoCoordinate>, crds)
                                  , Q_ARG(uchar, zoom)
                                  , Q_ARG(bool, clip));
        return requestId;
    }

    quint64 cacheSize() {
        quint64 sz;
        QMetaObject::invokeMethod(m_manager.get(), "cacheSize", Qt::BlockingQueuedConnection
                                  , Q_RETURN_ARG(quint64, sz));
        return sz;
    }

    QString cachePath() {
        QString path;
        QMetaObject::invokeMethod(m_manager.get(), "cachePath", Qt::BlockingQueuedConnection
                                  , Q_RETURN_ARG(QString, path));
        return path;
    }

private:
    NetworkManager() : m_manager(new NetworkIOManager) {
        m_thread.setObjectName("NetworkIOHandler Thread");
        m_manager->moveToThread(&m_thread);
        m_thread.start();
    }
    LoopedThread m_thread; // network ops happens in here
    QScopedPointer<NetworkIOManager> m_manager;
    quint64 m_requestID{1};
};

class TileReplyHandler : public ThreadedJob
{
    Q_OBJECT
public:
    TileReplyHandler(QNetworkReply *reply,
                     MapFetcherWorker &mapFetcher);
    ~TileReplyHandler() override = default;

    static int priority() { return 10; }

signals:
    void insertTile(quint64 id, TileKey k, std::shared_ptr<QImage> i, QByteArray md5 = {});
    void insertCompressedTileData(quint64 id, TileKey k, std::shared_ptr<QByteArray> d);
    void insertCoverage(quint64 id, std::shared_ptr<QImage> i);
    void expectingMoreSubtiles();

public slots:
    void process() override;

protected:
    void processStandaloneTile();
    void processCoverageTile();
    void finalizeCoverageRequest(quint64 id);

    QNetworkReply *m_reply{nullptr};
    MapFetcherWorker *m_mapFetcher{nullptr};
    bool m_computeHash{true}; // it's currently only false for DEM, so it tells whether it is a DEM image
    bool m_emitUncompressedData{false};
    bool m_dem{false};
};

struct TileReplyData : public ThreadedJobData {
    TileReplyData(QNetworkReply *reply,
                  MapFetcherWorker &mapFetcher)
   : ThreadedJobData(), m_reply(reply), m_mapFetcher(mapFetcher)
   {}
    ~TileReplyData() override {}
    JobType type() const override { return JobType::TileReply; }
    int priority() const override { return TileReplyHandler::priority(); }

    QNetworkReply *m_reply;
    MapFetcherWorker &m_mapFetcher;
};

class CachedCompoundTileHandler : public ThreadedJob
{
    Q_OBJECT
public:
    CachedCompoundTileHandler(quint64 id,
                              TileKey k,
                              quint8 sourceZoom,
                              QByteArray md5,
                              QString urlTemplate,
                              MapFetcherWorker &mapFetcher);
    ~CachedCompoundTileHandler() override = default;
    static int priority() { return 9; };

signals:
    void tileReady(quint64 id, TileKey k, std::shared_ptr<QImage> i, QByteArray md5);

public slots:
    void process() override;

protected:
    quint64 m_id;
    TileKey m_key;
    quint8 m_sourceZoom;
    QByteArray m_md5;
    QString m_urlTemplate;
    MapFetcherWorker *m_mapFetcher{nullptr};
};

struct CachedCompoundTileData : public ThreadedJobData {
    CachedCompoundTileData(quint64 id,
                           TileKey k,
                           quint8 sourceZoom,
                           QByteArray md5,
                           QString urlTemplate,
                           MapFetcherWorker &mapFetcher)
   : ThreadedJobData(), m_id(id), m_k(k), m_sourceZoom(sourceZoom), m_md5(std::move(md5))
     , m_urlTemplate(std::move(urlTemplate)), m_mapFetcher(mapFetcher)
   {}
    ~CachedCompoundTileData() override {}
    int priority() const override { return CachedCompoundTileHandler::priority(); }

    JobType type() const override { return JobType::CachedCompoundTile; }

    quint64 m_id;
    TileKey m_k;
    quint8 m_sourceZoom;
    QByteArray m_md5;
    QString m_urlTemplate;
    MapFetcherWorker &m_mapFetcher;
};

class DEMTileReplyHandler : public TileReplyHandler {
    Q_OBJECT
public:
    DEMTileReplyHandler(QNetworkReply *reply,
                     MapFetcherWorker &mapFetcher);
    ~DEMTileReplyHandler() override = default;

    static int priority() { return 7; }
};

struct DEMTileReplyData : public TileReplyData {
    DEMTileReplyData(QNetworkReply *reply,
                     MapFetcherWorker &mapFetcher)
    : TileReplyData(reply, mapFetcher) {}
    ~DEMTileReplyData() override {}
    int priority() const override { return DEMTileReplyHandler::priority(); }

    JobType type() const override { return JobType::DEMTileReply; }
};

class ASTCTileReplyHandler : public TileReplyHandler {
    Q_OBJECT
public:
    ASTCTileReplyHandler(QNetworkReply *reply,
                     MapFetcherWorker &mapFetcher);
    ~ASTCTileReplyHandler() override = default;

    static int priority() { return TileReplyHandler::priority(); }
};

struct ASTCTileReplyData : public TileReplyData {
    ASTCTileReplyData(QNetworkReply *reply,
                     MapFetcherWorker &mapFetcher)
    : TileReplyData(reply, mapFetcher) {}
    ~ASTCTileReplyData() override {}
    JobType type() const override { return JobType::ASTCTileReply; }
};

class DEMReadyHandler : public ThreadedJob
{
    Q_OBJECT
public:
    DEMReadyHandler(std::shared_ptr<QImage> demImage,
                    const TileKey k,
                    DEMFetcherWorker &demFetcher,
                    quint64 id,
                    bool coverage,
                    std::map<Heightmap::Neighbor, std::shared_ptr<QImage>> neighbors = {});

    ~DEMReadyHandler() override = default;
    static int priority() { return 8; }
    const TileKey &tileKey() const {
        return m_key;
    }

signals:
    void insertHeightmap(quint64 id, const TileKey k, std::shared_ptr<Heightmap> i);
    void insertHeightmapCoverage(quint64 id, std::shared_ptr<Heightmap> i);

public slots:
    void process() override;

private:
    DEMFetcherWorker *m_demFetcher{nullptr};
    std::shared_ptr<QImage> m_demImage;
    quint64 m_requestId{0};
    bool m_coverage;
    TileKey m_key;
    std::map<Heightmap::Neighbor, std::shared_ptr<QImage> > m_neighbors;
};

struct DEMReadyData : public ThreadedJobData {
    DEMReadyData(std::shared_ptr<QImage> demImage,
                     const TileKey k,
                     DEMFetcherWorker &demFetcher,
                     quint64 id,
                     bool coverage,
                     std::map<Heightmap::Neighbor, std::shared_ptr<QImage>> neighbors = {})
   : ThreadedJobData()
    , m_demImage(std::move(demImage)), m_k(k), m_demFetcher(demFetcher), m_id(id)
    , m_coverage(coverage), m_neighbors(std::move(neighbors))
   {}
    ~DEMReadyData() override {}
    int priority() const override { return DEMReadyHandler::priority(); }

    JobType type() const override { return JobType::DEMReady; }

    std::shared_ptr<QImage> m_demImage;
    const TileKey m_k;
    DEMFetcherWorker &m_demFetcher;
    quint64 m_id;
    bool m_coverage;
    std::map<Heightmap::Neighbor, std::shared_ptr<QImage>> m_neighbors;
};

struct Raster2ASTCData : public ThreadedJobData {
    Raster2ASTCData(std::shared_ptr<QImage> rasterImage,
                    const TileKey k,
                    ASTCFetcherWorker &fetcher,
                    quint64 id,
                    bool coverage,
                    QByteArray md5)
   : ThreadedJobData()
   , m_rasterImage(std::move(rasterImage)), m_k(k), m_fetcher(fetcher), m_id(id)
   , m_coverage(coverage), m_md5(std::move(md5))
   {}

    Raster2ASTCData(std::shared_ptr<QByteArray> compressedImage,
                    const TileKey k,
                    ASTCFetcherWorker &fetcher,
                    quint64 id,
                    bool coverage)
   : ThreadedJobData()
   , m_compressedRaster(std::move(compressedImage)), m_k(k), m_fetcher(fetcher), m_id(id)
   , m_coverage(coverage)
   {}

    ~Raster2ASTCData() override {}
    int priority() const override;
    JobType type() const override { return JobType::Raster2ASTC; }

    std::shared_ptr<QImage> m_rasterImage;
    std::shared_ptr<QByteArray> m_compressedRaster;
    const TileKey m_k;
    ASTCFetcherWorker &m_fetcher;
    quint64 m_id;
    bool m_coverage;
    QByteArray m_md5;
};

class Raster2ASTCHandler : public ThreadedJob
{
    Q_OBJECT
public:
    Raster2ASTCHandler(Raster2ASTCData *d);

    ~Raster2ASTCHandler() override {}
    const TileKey &tileKey() const {
        return d->m_k;
    }
    static int priority() { return 9; }

signals:
    void insertTileASTC(quint64 id, const TileKey k, std::shared_ptr<CompressedTextureData> i);
    void insertCoverageASTC(quint64 id, std::shared_ptr<CompressedTextureData> i);

public slots:
    void process() override;

private:
    QScopedPointer<Raster2ASTCData> d;
};


inline uint qHash (const QPoint & key)
{
    return qHash (QPair<int,int>(key.x(), key.y()) );
}
#endif
