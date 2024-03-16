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

using HeightmapCache = std::map<TileKey, std::shared_ptr<Heightmap>>;
using TileNeighborsMap = std::map<TileKey,
                            std::pair< Heightmap::Neighbors,
                                       std::map<Heightmap::Neighbor, std::shared_ptr<QImage>>>>;
using TileCache = std::map<TileKey, std::shared_ptr<QImage>>;
using TileCacheCache = std::map<TileKey, std::set<TileData>>;

class ThreadedJob : public QObject
{
    Q_OBJECT
public:
    ThreadedJob();
    ~ThreadedJob() override = default;

    void move2thread(QThread &t);

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
    void onInsertTile(quint64 id, const TileKey k,  std::shared_ptr<QImage> i);
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
    bool m_borders{false};
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

    void requestSlippyTiles(DEMFetcher *demFetcher,
                               quint64 requestId,
                               const QGeoCoordinate &ctl,
                               const QGeoCoordinate &cbr,
                               const quint8 zoom);

    void requestCoverage(MapFetcher *mapFetcher,
                            quint64 requestId,
                            const QGeoCoordinate &ctl,
                            const QGeoCoordinate &cbr,
                            const quint8 zoom,
                            const bool clip = false);

    void requestCoverage(DEMFetcher *demFetcher,
                            quint64 requestId,
                            const QGeoCoordinate &ctl,
                            const QGeoCoordinate &cbr,
                            const quint8 zoom,
                            const bool clip = false);

protected:
    void init() {
        if (!m_worker)
            m_worker = QSharedPointer<ThreadedJobQueue>(new ThreadedJobQueue);
        if (!m_timer) {
//            m_timer.reset(new QTimer);
//            m_timer->setInterval(1000);
//            m_timer->setSingleShot(false);
//            connect(
//                m_timer.get(), &QTimer::timeout,
//                []() { QCoreApplication::processEvents(); }
//            );
//            m_timer->start();
        }
    }

    MapFetcherWorker *getMapFetcherWorker(MapFetcher *f) {
        MapFetcherWorker *w;
        auto it = m_mapFetcher2Worker.find(f);
        if (it == m_mapFetcher2Worker.end()) {
            init();
            w = new MapFetcherWorker(this, f, m_worker);
            m_mapFetcher2Worker.insert({f, w});
            connect(w,
                    SIGNAL(tileReady(quint64,TileKey,std::shared_ptr<QImage>)),
                    f,
                    SLOT(onInsertTile(quint64,TileKey,std::shared_ptr<QImage>)), Qt::QueuedConnection);
            connect(w,
                    SIGNAL(coverageReady(quint64,std::shared_ptr<QImage>)),
                    f,
                    SLOT(onInsertCoverage(quint64,std::shared_ptr<QImage>)), Qt::QueuedConnection);
        } else {
            w = it->second;
        }
        return w;
    }

    DEMFetcherWorker *getDEMFetcherWorker(DEMFetcher *f) {
        DEMFetcherWorker *w;
        auto it = m_demFetcher2Worker.find(f);
        if (it == m_demFetcher2Worker.end()) {
            init();
            w = new DEMFetcherWorker(this, f, m_worker, f->d_func()->m_borders);
            m_demFetcher2Worker.insert({f, w});
            connect(w,
                    SIGNAL(heightmapReady(quint64,TileKey,std::shared_ptr<Heightmap>)),
                    f,
                    SLOT(onInsertHeightmap(quint64,TileKey,std::shared_ptr<Heightmap>)), Qt::QueuedConnection);
            connect(w,
                    SIGNAL(heightmapCoverageReady(quint64,std::shared_ptr<Heightmap>)),
                    f,
                    SLOT(onInsertHeightmapCoverage(quint64,std::shared_ptr<Heightmap>)), Qt::QueuedConnection);
        } else {
            w = it->second;
        }
        return w;
    }

protected:
    QSharedPointer<ThreadedJobQueue> m_worker; // TODO: figure how to use a qthreadpool and move qobjects to it
    QScopedPointer<QTimer> m_timer;
    std::unordered_map<MapFetcher *, MapFetcherWorker *> m_mapFetcher2Worker;
    std::unordered_map<DEMFetcher *, DEMFetcherWorker *> m_demFetcher2Worker;
};

// singleton to use a single QNAM
struct LoopedThread : public QThread {
    LoopedThread(QObject *parent = nullptr) : QThread(parent) {}
    ~LoopedThread() override = default;

//    void run() override {
//        exec();
//    }
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
                     MapFetcherWorker &mapFetcher,
                     bool last = false);
    ~TileReplyHandler() override;;

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
    bool m_last{false};
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

    ~DEMReadyHandler() override;
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

#endif
