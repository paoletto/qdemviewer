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

#include <QtCore/private/qobject_p.h>
#include <QQueue>
#include <QThread>
#include <QTimer>
#include <QCoreApplication>
#include <unordered_map>
#include <map>
#include <set>
#include <queue>
#include <unordered_set>
#include <private/qtexturefiledata_p.h>

using HeightmapCache = std::map<TileKey, std::shared_ptr<Heightmap>>;
using TileNeighborsMap = std::map<TileKey,
                            std::pair< Heightmap::Neighbors,
                                       std::map<Heightmap::Neighbor, std::shared_ptr<QImage>>>>;
using TileCache = std::map<TileKey, std::shared_ptr<QImage>>;
using TileCacheCache = std::map<TileKey, std::set<TileData>>;
using TileCacheASTC = std::map<TileKey, std::shared_ptr<CompressedTextureData>>;

class ThreadedJob : public QObject
{
    Q_OBJECT
public:
    ThreadedJob();
    ~ThreadedJob() override = default;

    void move2thread(QThread &t);
    virtual int priority() const;

public slots:
    virtual void process() = 0;

signals:
    void finished();
    void start();
    void error();

friend class ThreadedJobQueue;
};

class ThreadedJobQueue: public QObject
{
Q_OBJECT
public:
    ThreadedJobQueue(QObject *parent = nullptr);
    ~ThreadedJobQueue() override;

    void schedule(ThreadedJob *handler);

protected slots:
    void next();

protected:
    struct JobComparator {
        bool operator()(const ThreadedJob * l, const ThreadedJob * r) const;
    };
    std::priority_queue<ThreadedJob *, std::vector<ThreadedJob *>,  JobComparator> m_jobs;
    QObject *m_currentJob{nullptr};
    QThread m_thread;
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
    virtual quint64 requestSlippyTiles(const QGeoCoordinate &ctl,
                                    const QGeoCoordinate &cbr,
                                    const quint8 zoom,
                                    quint8 destinationZoom);

    virtual quint64 requestCoverage(const QGeoCoordinate &ctl,
                                    const QGeoCoordinate &cbr,
                                    const quint8 zoom,
                                    bool clip);

    QString objectName() const;

    QString m_urlTemplate;

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

    quint64 requestSlippyTiles(const QGeoCoordinate &ctl,
                            const QGeoCoordinate &cbr,
                            const quint8 zoom,
                            quint8) override;

    quint64 requestCoverage(const QGeoCoordinate &ctl,
                            const QGeoCoordinate &cbr,
                            const quint8 zoom,
                            bool clip) override;

    std::map<quint64, HeightmapCache> m_heightmapCache;
    std::map<quint64, std::shared_ptr<Heightmap>> m_heightmapCoverages;
    bool m_borders{false};
};

struct ASTCCompressedTextureData : public CompressedTextureData {
    ASTCCompressedTextureData() = default;
    ~ASTCCompressedTextureData() override = default;

    quint64 upload(QSharedPointer<QOpenGLTexture> &t) override;
    QSize size() const override;

    static std::shared_ptr<ASTCCompressedTextureData> fromImage(const std::shared_ptr<QImage> &i);

    std::shared_ptr<QImage> m_image;
    std::vector<QTextureFileData> m_mips;
};

class ASTCFetcherPrivate :  public MapFetcherPrivate
{
    Q_DECLARE_PUBLIC(ASTCFetcher)

public:
    ASTCFetcherPrivate() = default;
    ~ASTCFetcherPrivate() override = default;

    std::shared_ptr<CompressedTextureData> tileASTC(quint64 id, const TileKey k);
    quint64 requestSlippyTiles(const QGeoCoordinate &ctl,
                            const QGeoCoordinate &cbr,
                            const quint8 zoom,
                            quint8 destinationZoom) override;

    quint64 requestCoverage(const QGeoCoordinate &ctl,
                            const QGeoCoordinate &cbr,
                            const quint8 zoom,
                            bool clip) override;

    std::map<quint64, TileCacheASTC> m_tileCacheASTC;
    std::map<quint64, std::shared_ptr<CompressedTextureData>> m_coveragesASTC;
};

class MapFetcherWorkerPrivate;
class MapFetcherWorker : public QObject {
    Q_DECLARE_PRIVATE(MapFetcherWorker)
    Q_OBJECT

public:
    MapFetcherWorker(QObject *parent, MapFetcher *f, QSharedPointer<ThreadedJobQueue> worker);
    ~MapFetcherWorker() override = default;

    void requestSlippyTiles(quint64 requestId,
                            const QGeoCoordinate &ctl,
                            const QGeoCoordinate &cbr,
                            const quint8 zoom,
                            quint8 destinationZoom);

    void requestCoverage(quint64 requestId,
                            const QGeoCoordinate &ctl,
                            const QGeoCoordinate &cbr,
                            const quint8 zoom,
                            const bool clip = false);

    std::shared_ptr<QImage> tile(quint64 requestId, const TileKey &k);

    void setURLTemplate(const QString &urlTemplate);

signals:
    void tileReady(quint64 id, const TileKey k, std::shared_ptr<QImage>);
    void coverageReady(quint64 id, std::shared_ptr<QImage>);
    void requestHandlingFinished(quint64 id);

protected slots:
    void onTileReplyFinished();
    void onTileReplyForCoverageFinished();
    void onInsertTile(const quint64 id, const TileKey k, std::shared_ptr<QImage> i);
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
    MapFetcherWorkerPrivate() :  QObjectPrivate() {}
    ~MapFetcherWorkerPrivate() override = default;

    std::shared_ptr<QImage> tile(quint64 requestId, const TileKey &k);
    std::shared_ptr<QImage> peekTile(quint64 requestId, const TileKey &k);
    virtual void trackNeighbors(quint64, const TileKey &, Heightmap::Neighbors) {}
    QString objectName() const;

    QString m_urlTemplate;
    ThrottledNetworkFetcher m_nm;
    std::map<quint64, TileCache> m_tileCache;
    std::map<quint64, TileCacheCache> m_tileCacheCache;

    std::map<quint64, std::tuple<QGeoCoordinate,  // tl
                                 QGeoCoordinate,  // br
                                 quint8,          // zoom
                                 quint64,         // numTiles
                                 bool             // clip
                                >> m_requests;
    std::map<quint64, std::set<TileData>> m_tileSets;

    QSharedPointer<ThreadedJobQueue> m_worker; // TODO: figure how to use a qthreadpool and move qobjects to it
    MapFetcher *m_fetcher{nullptr};
    std::unordered_map<quint64, quint64> m_request2remainingTiles;
    std::unordered_map<quint64, quint64> m_request2remainingHandlers;
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

    void trackNeighbors(quint64 id, const TileKey &k, Heightmap::Neighbors n) override;

    std::map<quint64, TileNeighborsMap> m_request2Neighbors;
    std::map<quint64, HeightmapCache> m_heightmapCache;
    std::map<quint64, std::shared_ptr<Heightmap>> m_heightmapCoverages;
    std::unordered_map<quint64, quint64> m_request2remainingDEMHandlers;
    bool m_borders{false};
};

class ASTCFetcherWorkerPrivate;
class ASTCFetcherWorker : public MapFetcherWorker {
    Q_DECLARE_PRIVATE(ASTCFetcherWorker)
    Q_OBJECT

public:
    ASTCFetcherWorker(QObject *parent,
                      ASTCFetcher *f,
                      QSharedPointer<ThreadedJobQueue> worker);
    ~ASTCFetcherWorker() override = default;

signals:
    void tileASTCReady(quint64 id, const TileKey k, std::shared_ptr<CompressedTextureData>);
    void coverageASTCReady(quint64 id, std::shared_ptr<CompressedTextureData>);

protected slots:
    void onTileReady(quint64 id, const TileKey k,  std::shared_ptr<QImage> i);
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

    std::unordered_map<quint64, quint64> m_request2remainingASTCHandlers;
};

class NetworkIOManager: public QObject //living in a separate thread
{
    Q_OBJECT

public:
    NetworkIOManager();
    ~NetworkIOManager() override = default;

public slots:
    void requestSlippyTiles(MapFetcher *mapFetcher,
                               quint64 requestId,
                               const QGeoCoordinate &ctl,
                               const QGeoCoordinate &cbr,
                               const quint8 zoom,
                               quint8 destinationZoom);

    void requestCoverage(MapFetcher *mapFetcher,
                            quint64 requestId,
                            const QGeoCoordinate &ctl,
                            const QGeoCoordinate &cbr,
                            const quint8 zoom,
                            const bool clip = false);


    void requestSlippyTiles(DEMFetcher *demFetcher,
                               quint64 requestId,
                               const QGeoCoordinate &ctl,
                               const QGeoCoordinate &cbr,
                               const quint8 zoom);

    void requestCoverage(DEMFetcher *demFetcher,
                            quint64 requestId,
                            const QGeoCoordinate &ctl,
                            const QGeoCoordinate &cbr,
                            const quint8 zoom,
                            const bool clip = false);

    void requestSlippyTiles(ASTCFetcher *fetcher,
                            quint64 requestId,
                            const QGeoCoordinate &ctl,
                            const QGeoCoordinate &cbr,
                            const quint8 zoom,
                            quint8 destinationZoom);

    void requestCoverage(ASTCFetcher *fetcher,
                            quint64 requestId,
                            const QGeoCoordinate &ctl,
                            const QGeoCoordinate &cbr,
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

    std::unordered_map<MapFetcher *, MapFetcherWorker *> m_mapFetcher2Worker;
    std::unordered_map<DEMFetcher *, DEMFetcherWorker *> m_demFetcher2Worker;
    std::unordered_map<ASTCFetcher *, ASTCFetcherWorker *> m_astcFetcher2Worker;
};

// singleton to use a single QNAM
struct LoopedThread : public QThread {
    LoopedThread(QObject *parent = nullptr) : QThread(parent) {}
    ~LoopedThread() override = default;

    // Somehow this isn't really helping.
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

    quint64 requestSlippyTiles(MapFetcher &mapFetcher,
                               const QGeoCoordinate &ctl,
                               const QGeoCoordinate &cbr,
                               const quint8 zoom,
                               const quint8 destinationZoom) {
        const auto requestId = m_requestID++;
        QMetaObject::invokeMethod(m_manager.get(), "requestSlippyTiles", Qt::QueuedConnection
                                  , Q_ARG(MapFetcher *, &mapFetcher)
                                  , Q_ARG(qulonglong, requestId)
                                  , Q_ARG(QGeoCoordinate, ctl)
                                  , Q_ARG(QGeoCoordinate, cbr)
                                  , Q_ARG(uchar, zoom)
                                  , Q_ARG(uchar, destinationZoom));
        return requestId;
    }

    quint64 requestCoverage(MapFetcher &mapFetcher,
                            const QGeoCoordinate &ctl,
                            const QGeoCoordinate &cbr,
                            const quint8 zoom,
                            const bool clip = false) {
        auto requestId = m_requestID++;
        QMetaObject::invokeMethod(m_manager.get(), "requestCoverage", Qt::QueuedConnection
                                  , Q_ARG(MapFetcher *, &mapFetcher)
                                  , Q_ARG(qulonglong, requestId)
                                  , Q_ARG(QGeoCoordinate, ctl)
                                  , Q_ARG(QGeoCoordinate, cbr)
                                  , Q_ARG(uchar, zoom)
                                  , Q_ARG(bool, clip));
        return requestId;
    }

    quint64 requestSlippyTiles(DEMFetcher &demFetcher,
                               const QGeoCoordinate &ctl,
                               const QGeoCoordinate &cbr,
                               const quint8 zoom) {
        const auto requestId = m_requestID++;
        QMetaObject::invokeMethod(m_manager.get(), "requestSlippyTiles", Qt::QueuedConnection
                                  , Q_ARG(DEMFetcher *, &demFetcher)
                                  , Q_ARG(qulonglong, requestId)
                                  , Q_ARG(QGeoCoordinate, ctl)
                                  , Q_ARG(QGeoCoordinate, cbr)
                                  , Q_ARG(uchar, zoom));
        return requestId;
    }

    quint64 requestCoverage(DEMFetcher &demFetcher,
                            const QGeoCoordinate &ctl,
                            const QGeoCoordinate &cbr,
                            const quint8 zoom,
                            const bool clip = false) {
        auto requestId = m_requestID++;
        QMetaObject::invokeMethod(m_manager.get(), "requestCoverage", Qt::QueuedConnection
                                  , Q_ARG(DEMFetcher *, &demFetcher)
                                  , Q_ARG(qulonglong, requestId)
                                  , Q_ARG(QGeoCoordinate, ctl)
                                  , Q_ARG(QGeoCoordinate, cbr)
                                  , Q_ARG(uchar, zoom)
                                  , Q_ARG(bool, clip));
        return requestId;
    }

    quint64 requestSlippyTiles(ASTCFetcher &fetcher,
                               const QGeoCoordinate &ctl,
                               const QGeoCoordinate &cbr,
                               const quint8 zoom,
                               const quint8 destinationZoom) {
        const auto requestId = m_requestID++;
        QMetaObject::invokeMethod(m_manager.get(), "requestSlippyTiles", Qt::QueuedConnection
                                  , Q_ARG(ASTCFetcher *, &fetcher)
                                  , Q_ARG(qulonglong, requestId)
                                  , Q_ARG(QGeoCoordinate, ctl)
                                  , Q_ARG(QGeoCoordinate, cbr)
                                  , Q_ARG(uchar, zoom)
                                  , Q_ARG(uchar, destinationZoom));
        return requestId;
    }

    quint64 requestCoverage(ASTCFetcher &fetcher,
                            const QGeoCoordinate &ctl,
                            const QGeoCoordinate &cbr,
                            const quint8 zoom,
                            const bool clip = false) {
        auto requestId = m_requestID++;
        QMetaObject::invokeMethod(m_manager.get(), "requestCoverage", Qt::QueuedConnection
                                  , Q_ARG(ASTCFetcher *, &fetcher)
                                  , Q_ARG(qulonglong, requestId)
                                  , Q_ARG(QGeoCoordinate, ctl)
                                  , Q_ARG(QGeoCoordinate, cbr)
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

signals:
    void insertTile(quint64 id,TileKey k, std::shared_ptr<QImage> i);
    void insertCoverage(quint64 id, std::shared_ptr<QImage> i);
    void expectingMoreSubtiles();

public slots:
    void process() override;

private:
    void processStandaloneTile();
    void processCoverageTile();
    void finalizeCoverageRequest(quint64 id);

    QNetworkReply *m_reply{nullptr};
    MapFetcherWorker *m_mapFetcher{nullptr};
};

class DEMTileReplyHandler : public TileReplyHandler {
    Q_OBJECT
public:
    DEMTileReplyHandler(QNetworkReply *reply,
                     MapFetcherWorker &mapFetcher);
    ~DEMTileReplyHandler() override = default;

    int priority() const override;
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
    int priority() const override;
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

class Raster2ASTCHandler : public ThreadedJob
{
    Q_OBJECT
public:
    Raster2ASTCHandler(std::shared_ptr<QImage> demImage,
                    const TileKey k,
                    ASTCFetcherWorker &fetcher,
                    quint64 id,
                    bool coverage);

    ~Raster2ASTCHandler() override = default;
    const TileKey &tileKey() const {
        return m_key;
    }
    int priority() const override;

signals:
    void insertTileASTC(quint64 id, const TileKey k, std::shared_ptr<CompressedTextureData> i);
    void insertCoverageASTC(quint64 id, std::shared_ptr<CompressedTextureData> i);

public slots:
    void process() override;

private:
    ASTCFetcherWorker *m_fetcher{nullptr};
    std::shared_ptr<QImage> m_rasterImage;
    quint64 m_requestId{0};
    bool m_coverage;
    TileKey m_key;
};

#endif
