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
    TileReplyHandler(QNetworkReply *reply, MapFetcher *mapFetcher);
    ~TileReplyHandler() override;;

signals:
    void insertTile(const TileKey k, std::shared_ptr<QImage> i);
    void insertCoverage(const quint64 id, std::shared_ptr<QImage> i);
    void expectingMoreSubtiles();

public slots:
    void process() override;

private:
    void processStandaloneTile();
    void processCoverageTile();
    void finalizeCoverageRequest(quint64 id);

    QNetworkReply *m_reply{nullptr};
    MapFetcher *m_mapFetcher{nullptr};
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

    void requestTile(const QUrl &request, const TileKey &k, const quint8 destinationZoom, const quint64 id,
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
                 const quint64 id,
                 const Heightmap::Neighbors boundaries,
                 QObject *destFinished,
                 const char *onFinishedSlot,
                 QObject *destError,
                 const char *onErrorSlot);

    QNetworkAccessManager &m_nm;
    size_t m_maxConcurrent;
    size_t m_active{0};

    QQueue<std::tuple<QUrl, TileKey, quint8, quint64, quint32,
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

    QString objectName() const;

    QString m_urlTemplate;
    ThrottledNetworkFetcher m_nm;

    std::map<TileKey, std::shared_ptr<QImage>> m_tileCache;
    std::map<TileKey, std::set<TileData>> m_tileCacheCache;
    quint64 m_coverageRequestID{1};
    std::map<quint64, std::tuple<QGeoCoordinate, QGeoCoordinate, quint8, quint64, bool>> m_requests;
    std::map<quint64, std::set<TileData>> m_tileSets;
    std::map<quint64, std::shared_ptr<QImage>> m_coverages;
    ThreadedJobQueue *m_worker{nullptr}; // TODO: figure how to use a qthreadpool and move qobjects to it
    const QImage m_empty;
};

class DEMFetcherPrivate :  public MapFetcherPrivate
{
    Q_DECLARE_PUBLIC(DEMFetcher)

public:
    DEMFetcherPrivate();
    ~DEMFetcherPrivate() override;

    void trackNeighbors(const TileKey k, Heightmap::Neighbors n) override;

    std::map<TileKey, std::shared_ptr<Heightmap>> m_heightmapCache;
    std::map<quint64, std::shared_ptr<Heightmap>> m_heightmapCoverages;
    bool m_borders{false};
    std::map<TileKey,
             std::pair< Heightmap::Neighbors,
                         std::map<Heightmap::Neighbor, std::shared_ptr<QImage>>>> m_tileNeighbors;
};

#endif
