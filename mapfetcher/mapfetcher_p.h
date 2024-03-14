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
#include <unordered_map>
#include <map>
#include <set>
#include <unordered_set>


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

friend class ThreadedJobQueue;
};

class TileReplyHandler : public ThreadedJob
{
    Q_OBJECT
public:
    TileReplyHandler(QNetworkReply *reply,
                     MapFetcher *mapFetcher,
                     QObject *insertReceiver,
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
    MapFetcher *m_mapFetcher{nullptr};
    bool m_last{false};
};

class DEMReadyHandler : public ThreadedJob
{
    Q_OBJECT
public:
    DEMReadyHandler(std::shared_ptr<QImage> demImage,
                    const TileKey k,
                    DEMFetcher &demFetcher,
                    quint64 coverageId,
                    std::map<Heightmap::Neighbor, std::shared_ptr<QImage>> neighbors = {});

    ~DEMReadyHandler() override;

signals:
    void insertHeightmap(const TileKey k, std::shared_ptr<Heightmap> i);
    void insertHeightmapCoverage(quint64 coverageId, std::shared_ptr<Heightmap> i);

public slots:
    void process() override;

private:
    DEMFetcher *m_demFetcher{nullptr};
    std::shared_ptr<QImage> m_demImage;
    quint64 m_coverageId{0};
    TileKey m_key;
    std::map<Heightmap::Neighbor, std::shared_ptr<QImage> > m_neighbors;
};

class ThreadedJobQueue: public QObject
{
Q_OBJECT
public:
    ThreadedJobQueue(QObject *parent = nullptr);
    ~ThreadedJobQueue() override;

    void schedule(ThreadedJob *handler);

signals:

protected slots:
    void next();

protected:
    QQueue<ThreadedJob *> m_jobs;
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

    std::shared_ptr<QImage> tile(const TileKey k);
    std::shared_ptr<QImage> peekTile(const TileKey k);
    virtual void trackNeighbors(const TileKey, Heightmap::Neighbors) {}
    virtual void requestSlippyTiles(const QGeoCoordinate &ctl,
                                    const QGeoCoordinate &cbr,
                                    const quint8 zoom,
                                    quint8 destinationZoom);

    QString objectName() const;

    QString m_urlTemplate;
//    ThrottledNetworkFetcher m_nm;

    std::map<TileKey, std::shared_ptr<QImage>> m_tileCache;
    std::map<TileKey, std::set<TileData>> m_tileCacheCache;
    quint64 m_coverageRequestID{1};
    std::map<quint64, std::tuple<QGeoCoordinate, QGeoCoordinate, quint8, quint64, bool>> m_requests;
    std::map<quint64, std::set<TileData>> m_tileSets;
    std::map<quint64, std::shared_ptr<QImage>> m_coverages;
    QScopedPointer<ThreadedJobQueue> m_worker{nullptr}; // TODO: figure how to use a qthreadpool and move qobjects to it
    const QImage m_empty;
};

class DEMFetcherPrivate :  public MapFetcherPrivate
{
    Q_DECLARE_PUBLIC(DEMFetcher)

public:
    DEMFetcherPrivate();
    ~DEMFetcherPrivate() override;

    void requestSlippyTiles(const QGeoCoordinate &ctl,
                            const QGeoCoordinate &cbr,
                            const quint8 zoom,
                            quint8 destinationZoom) override;

//    void trackNeighbors(const TileKey k, Heightmap::Neighbors n) override;

    std::map<TileKey, std::shared_ptr<Heightmap>> m_heightmapCache;
    std::map<quint64, std::shared_ptr<Heightmap>> m_heightmapCoverages;
    bool m_borders{false};
};

class NetworkIOManager: public QObject //living in a separate thread
{
    Q_OBJECT

    using TileNeighborsMap = std::map<TileKey,
                                std::pair< Heightmap::Neighbors,
                                           std::map<Heightmap::Neighbor, std::shared_ptr<QImage>>>>;
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

//    void requestCoverage(MapFetcher &mapFetcher,
//                            quint64 requestId,
//                            const QGeoCoordinate &ctl,
//                            const QGeoCoordinate &cbr,
//                            const quint8 zoom,
//                            const bool clip = false) {

//    }

//    void requestCoverage(DEMFetcher &demFetcher,
//                            quint64 requestId,
//                            const QGeoCoordinate &ctl,
//                            const QGeoCoordinate &cbr,
//                            const quint8 zoom,
//                            const bool clip = false) {

//    }

signals:
    void heightmapReady(const TileKey k);
    void heightmapCoverageReady(quint64 id);
    void tileReady(quint64 id, const TileKey k);
    void coverageReady(quint64 id);

protected slots:
    void onTileReplyFinished();
    void onDEMTileReplyFinished();
//    void onTileReplyForCoverageFinished();
    void onInsertTile(const quint64 id, const TileKey k, std::shared_ptr<QImage> i);
//    void onInsertCoverage(const quint64 id, std::shared_ptr<QImage> i);
    void networkReplyError(QNetworkReply::NetworkError);

protected slots:
//    void onInsertHeightmap(TileKey k, std::shared_ptr<Heightmap> h);
//    void onInsertHeightmapCoverage(quint64 id, std::shared_ptr<Heightmap> h);

protected:
    void trackNeighbors(quint64 requestId, const TileKey k, Heightmap::Neighbors n);


protected:
    QScopedPointer<ThreadedJobQueue> m_worker; // TODO: figure how to use a qthreadpool and move qobjects to it
    std::unordered_map<quint64, quint64> m_request2remainingTiles;
    std::unordered_map<quint64, MapFetcher *> m_request2Consumer;
//    std::unordered_set<MapFetcher *> m_registeredConsumers;

    std::map<quint64, TileNeighborsMap> m_request2Neighbors;
    ThrottledNetworkFetcher *m_nm{nullptr};
};

// singleton to use a single QNAM
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
        const auto requestId = m_slippyRequestID++;
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
//        const auto requestId = m_coverageRequestID++;
        const auto requestId = m_slippyRequestID++;
        QMetaObject::invokeMethod(m_manager.get(), "requestSlippyTiles", Qt::QueuedConnection
                                  , Q_ARG(DEMFetcher *, &demFetcher)
                                  , Q_ARG(qulonglong, requestId)
                                  , Q_ARG(QGeoCoordinate, ctl)
                                  , Q_ARG(QGeoCoordinate, cbr)
                                  , Q_ARG(uchar, zoom));
        return requestId;
    }

//    quint64 requestCoverage(MapFetcher &mapFetcher,
//                            const QGeoCoordinate &ctl,
//                            const QGeoCoordinate &cbr,
//                            const quint8 zoom,
//                            const bool clip = false) {
//        auto requestId = m_coverageRequestID++;

//    }

private:
    NetworkManager() : m_manager(new NetworkIOManager) {
        qDebug() << "NetworkManager()";
        m_manager->moveToThread(&m_thread);
        qDebug() << "NetworkManager() starting thread";
        m_thread.start();
    }
    QThread m_thread; // network ops happens in here
    QScopedPointer<NetworkIOManager> m_manager;
    quint64 m_slippyRequestID{1};
    quint64 m_coverageRequestID{1};
};


#endif
