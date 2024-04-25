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

#include "mapfetcher.h"
#include "mapfetcher_p.h"
#include "networksqlitecache_p.h"
#include "tilecache_p.h"
#include "utils_p.h"

#include <QtPositioning/private/qwebmercator_p.h>
#include <QtLocation/private/qgeocameratiles_p_p.h>
#include <QRandomGenerator>
#include <QRandomGenerator64>
#include <QNetworkRequest>
#include <QCoreApplication>

#include <QThreadPool>
#include <QRunnable>
#include <QThread>
#include <QQueue>

#include <QByteArray>
#include <QMatrix4x4>
#include <QQuaternion>
#include <QVector3D>
#include <QOpenGLPixelTransferOptions>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions_4_5_Core>

#include <QStandardPaths>
#include <QDirIterator>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>

#include <iostream>
#include <string>
#include <cstdlib>
#include <vector>
#include <map>

struct GeoTileSpec {
    QGeoTileSpec ts;
    Heightmap::Neighbors nb;

    bool operator == (const GeoTileSpec &rhs) const {
        return ts == rhs.ts;
    }

    bool operator <(const GeoTileSpec &rhs) const {
        return ts < rhs.ts;
    }
};
QDebug operator<<(QDebug d, const GeoTileSpec &k) {
    QDebug nsp = d.nospace();
    nsp << k.ts << "-"<<k.nb;
    return d;
}

namespace  {
const std::map<Heightmap::Neighbor, std::shared_ptr<QImage>> boundaryRasters{
    {Heightmap::Top, {}},
    {Heightmap::Bottom, {}},
    {Heightmap::Left, {}},
    {Heightmap::Right, {}},
    {Heightmap::TopLeft, {}},
    {Heightmap::TopRight, {}},
    {Heightmap::BottomLeft, {}},
    {Heightmap::BottomRight, {}}
};
const QString urlTemplateTerrariumS3{"https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"};
//const quint64 tileSize{256};
} // namespace

namespace  {
static constexpr std::array<Heightmap::Neighbor, 8> neighbors = {
        Heightmap::Top,
        Heightmap::Bottom,
        Heightmap::Left,
        Heightmap::Right,
        Heightmap::TopLeft,
        Heightmap::TopRight,
        Heightmap::BottomLeft,
        Heightmap::BottomRight};
const std::map<Heightmap::Neighbor, TileKey> neighborOffsets = {
        {Heightmap::Top, TileKey(0,-1,0)},
        {Heightmap::Bottom, TileKey(0,1,0)},
        {Heightmap::Left, TileKey(-1,0,0)},
        {Heightmap::Right, TileKey(1,0,0)},
        {Heightmap::TopLeft, TileKey(-1,-1,0)},
        {Heightmap::TopRight, TileKey(1,-1,0)},
        {Heightmap::BottomLeft, TileKey(-1,1,0)},
        {Heightmap::BottomRight, TileKey(1,1,0)}
};
const std::map<Heightmap::Neighbor, Heightmap::Neighbor> neighborReciprocal = {
        {Heightmap::Top, Heightmap::Bottom},
        {Heightmap::Bottom, Heightmap::Top},
        {Heightmap::Left, Heightmap::Right},
        {Heightmap::Right, Heightmap::Left},
        {Heightmap::TopLeft, Heightmap::BottomRight},
        {Heightmap::TopRight, Heightmap::BottomLeft},
        {Heightmap::BottomLeft, Heightmap::TopRight},
        {Heightmap::BottomRight, Heightmap::TopLeft}
};
//quint64 getX(const TileData& d) {
//    return d.k.x;
//}
//quint64 getY(const TileData& d) {
//    return d.k.y;
//}
quint64 getX(const QGeoTileSpec& d) {
    return quint64(d.x());
}
quint64 getY(const QGeoTileSpec& d) {
    return quint64(d.y());
}
quint64 getX(const GeoTileSpec& d) {
    return quint64(d.ts.x());
}
quint64 getY(const GeoTileSpec& d) {
    return quint64(d.ts.y());
}
template <class T>
std::tuple<quint64, quint64, quint64, quint64> getMinMax(const std::set<T> &tileSet) {
    quint64 minX{std::numeric_limits<quint64>::max()},
            maxX{std::numeric_limits<quint64>::min()},
            minY{std::numeric_limits<quint64>::max()},
            maxY{std::numeric_limits<quint64>::min()};

    for (const auto &t: tileSet) {
        minX = std::min(minX, getX(t));
        maxX = std::max(maxX, getX(t));
        minY = std::min(minY, getY(t));
        maxY = std::max(maxY, getY(t));
    }
    return {minX, maxX, minY, maxY};
}
int subtileSide(int sourceZoom, int destinationZoom) {
    if (sourceZoom >= destinationZoom)
        return 1;
    const int side = 1 << (destinationZoom - sourceZoom);
    return side;
}
int subtilesPerTile(int sourceZoom, int destinationZoom) {
    const int side = subtileSide(sourceZoom, destinationZoom);
    return side * side;
}
std::set<GeoTileSpec> tilesFromBounds(const QList<QGeoCoordinate> &crds,
                                      const quint8 zoom,
                                      bool rectangular = false) {
    QGeoCameraTilesPrivate ct;
    ct.m_intZoomLevel = zoom;
    ct.m_sideLength = 1 << ct.m_intZoomLevel;
    ct.m_mapVersion = 1;

    if (crds.size() < 3)
        return {};

    PolygonVector p;
    for (const auto &c: crds) {
        if (!c.isValid())
            return {};
        p << ct.m_sideLength * QWebMercator::coordToMercator(c);
    }

    auto tiles = ct.tilesFromPolygon(p);

    if (tiles.empty())
        return {};

    const QGeoTileSpec &templateSpec = *tiles.begin();
    QHash<QPoint, QGeoTileSpec> hashedTileSet;
    if (!rectangular) {
        for (const auto &c: qAsConst(tiles)) {
            hashedTileSet[{c.x(), c.y()}] = c;
        }
    } else {
        quint64 minX, maxX, minY, maxY;
        std::set<QGeoTileSpec> tileSet(tiles.begin(), tiles.end());
        std::tie(minX, maxX, minY, maxY) = getMinMax(tileSet);

        for (int x=minX; x<=maxX; ++x) {
            for (int y=minY; y<=maxY; ++y) {
                hashedTileSet[{x,y}] = QGeoTileSpec(templateSpec.plugin(),
                                                    templateSpec.mapId(),
                                                    templateSpec.zoom(),
                                                    x,y,templateSpec.version());
            }
        }
    }

    auto getBoundaries = [&hashedTileSet](const QGeoTileSpec &t) {
                                Heightmap::Neighbors res;
                                const int &x = t.x();
                                const int &y = t.y();
                                if (hashedTileSet.contains({x-1,y}))
                                    res |= Heightmap::Left;
                                if (hashedTileSet.contains({x+1,y}))
                                    res |= Heightmap::Right;
                                if (hashedTileSet.contains({x,y-1}))
                                    res |= Heightmap::Top;
                                if (hashedTileSet.contains({x,y+1}))
                                    res |= Heightmap::Bottom;
                                if (hashedTileSet.contains({x-1,y-1}))
                                    res |= Heightmap::TopLeft;
                                if (hashedTileSet.contains({x+1,y-1}))
                                    res |= Heightmap::TopRight;
                                if (hashedTileSet.contains({x-1,y+1}))
                                    res |= Heightmap::BottomLeft;
                                if (hashedTileSet.contains({x+1,y+1}))
                                    res |= Heightmap::BottomRight;
                                return res;
                            };

    std::set<GeoTileSpec> res;
    for (const auto &t: hashedTileSet) {
        res.insert({t, getBoundaries(t)});
    }
    return res;
}
void requestMapTiles(const std::set<GeoTileSpec> &tiles,
                     const QList<QString> &urlTemplate,
                     const quint8 destinationZoom,
                     const quint64 id,
                     const bool coverageRequest,
                     ThrottledNetworkFetcher &nam,
                     QObject *destFinished,
                     const char *onFinishedSlot,
                     QObject *destError,
                     const char *onErrorSlot) {
    int i = 0;
    for (const auto &t: tiles) {
        auto tileUrl = urlTemplate[i++ % urlTemplate.size()];
        tileUrl = tileUrl.replace(QStringLiteral("{x}"), QString::number(t.ts.x()))
                .replace(QStringLiteral("{y}"), QString::number(t.ts.y()))
                .replace(QStringLiteral("{z}"), QString::number(t.ts.zoom()));

        nam.requestTile(tileUrl,
                        {quint64(t.ts.x()), quint64(t.ts.y()), quint8(t.ts.zoom())},
                        destinationZoom,
                        id,
                        coverageRequest,
                        t.nb,
                        destFinished,
                        onFinishedSlot,
                        destError,
                        onErrorSlot);
    }
}
} //namespace

// Here because of tilesFromBounds
quint8 MapFetcher::zoomForCoverage(const QList<QGeoCoordinate> &crds,
                                   const size_t tileResolution,
                                   const size_t maxCoverageResolution,
                                   bool rectangular) {
    if (tileResolution == 0
        || crds.isEmpty())
        return 0;

    quint64 minX, maxX, minY, maxY;
    for (int z = 1; z <= 20; ++z) {
        auto ts = tilesFromBounds(crds, z, rectangular);
        std::set<GeoTileSpec> &tileSet = ts;
        std::tie(minX, maxX, minY, maxY) = getMinMax(tileSet);
        const quint64 hTiles = maxX - minX + 1;
        const quint64 vTiles = maxY - minY + 1;
        if (std::min(hTiles * tileResolution, vTiles * tileResolution) > maxCoverageResolution)
            return z - 1;
    }
    return 20;
}

// Use one nam + network cache for all instances of MapFetcher.
class NAM
{
public:
    static NAM& instance()
    {
        static NAM    instance;
        return instance;
    }

    QNetworkAccessManager &nam() {
        return m_nm;
    };


    quint64 cacheSize() {
        return m_networkCache.cacheSize();
    }

    QString cachePath() const {
        return m_cacheDirPath;
    }

    void addURLMultiTemplate(const QString &urlTemplate) {
        m_networkCache.addEquivalenceClass(urlTemplate);
    }

private:
    QString m_cacheDirPath;
    QNetworkAccessManager m_nm;
//    QNetworkDiskCache m_networkCache;
//    NetworkInMemoryCache m_networkCache;
    NetworkSqliteCache m_networkCache;


    NAM()
:  m_cacheDirPath(QStringLiteral("%1/networkCache.sqlite").arg(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)))
//   m_cacheDirPath(QStringLiteral("/tmp/networkCache.sqlite"))
  ,m_networkCache(m_cacheDirPath) {
        ;
//        m_networkCache.setCacheDirectory(m_cacheDirPath);
//        m_networkCache.setMaximumCacheSize(qint64(5)
//                                          * qint64(1024)
//                                          * qint64(1024)
//                                          * qint64(1024)); // TODO: make configurable
        m_nm.setCache(&m_networkCache);
    }

public:
    NAM(NAM const&)            = delete;
    void operator=(NAM const&) = delete;
};

ThrottledNetworkFetcher::ThrottledNetworkFetcher(QObject *parent, size_t maxConcurrentRequests)
: QObject(parent), m_nm(NAM::instance().nam()), m_maxConcurrent(maxConcurrentRequests)
{
}

void ThrottledNetworkFetcher::requestTile(const QUrl &u,
                                          const TileKey &k,
                                          const quint8 destinationZoom,
                                          const quint64 id,
                                          const bool coverageRequest,
                                          const Heightmap::Neighbors boundaries,
                                          QObject *destFinished,
                                          const char *onFinishedSlot,
                                          QObject *destError,
                                          const char *onErrorSlot)
{
    if (!destFinished || !onFinishedSlot) {
        qWarning() << "Trying to request "<<u<<" without a receiver is not allowed. skipping";
        return;
    }

    if (m_active >= m_maxConcurrent) {
        m_pendingRequests.append({u,
                                  k,
                                  destinationZoom,
                                  id,
                                  coverageRequest,
                                  boundaries,
                                  destFinished,
                                  std::string(onFinishedSlot),
                                  destError,
                                  onErrorSlot});
        return;
    }

    request(u,
            k,
            destinationZoom,
            id,
            coverageRequest,
            boundaries,
            destFinished,
            onFinishedSlot,
            destError,
            onErrorSlot);
}

void ThrottledNetworkFetcher::onFinished()
{
    --m_active;
    while (m_active < m_maxConcurrent) { // unnecessary if?
        if (m_pendingRequests.isEmpty())
            break;
        QUrl u;
        TileKey k;
        quint8 dz;
        quint64 id;
        bool coverageRequest;
        quint32 boundaries;
        QObject *destFinished;
        std::string onFinishedSlot;
        QObject *destError;
        std::string onErrorSlot;

        std::tie(u,
                 k,
                 dz,
                 id,
                 coverageRequest,
                 boundaries,
                 destFinished,
                 onFinishedSlot,
                 destError,
                 onErrorSlot) = m_pendingRequests.dequeue();
        request(u, k, dz, id, coverageRequest, Heightmap::Neighbors(boundaries)
                , destFinished, onFinishedSlot.c_str(), destError, onErrorSlot.c_str());
    }
}

void ThrottledNetworkFetcher::request(const QUrl &u,
                                      const TileKey &k,
                                      const quint8 destinationZoom,
                                      const quint64 id,
                                      const bool coverage,
                                      const Heightmap::Neighbors boundaries,
                                      QObject *destFinished,
                                      const char *onFinishedSlot,
                                      QObject *destError,
                                      const char *onErrorSlot)
{
    QNetworkRequest request;
    QNetworkRequest::CacheLoadControl cacheSetting{QNetworkRequest::PreferCache};
    if (NetworkConfiguration::offline)
        cacheSetting = QNetworkRequest::AlwaysCache;

    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, cacheSetting);
    request.setHeader(QNetworkRequest::UserAgentHeader, QCoreApplication::applicationName());
    request.setUrl(u);

    if (NetworkConfiguration::logNetworkRequests)
        qInfo() << "<-- "<<u;

    QNetworkReply *reply = m_nm.get(request);
    reply->setProperty("x",k.x);
    reply->setProperty("y",k.y);
    reply->setProperty("z",k.z);
    reply->setProperty("dz",destinationZoom);
    reply->setProperty("b", quint32(boundaries));
    if (id > 0)
        reply->setProperty("ID",id);
    reply->setProperty("c", coverage);
    if (destFinished && onFinishedSlot)
        connect(reply, SIGNAL(finished()), destFinished, onFinishedSlot);
    if (destError && onErrorSlot)
        connect(reply, SIGNAL(errorOccurred(QNetworkReply::NetworkError)), destError, onErrorSlot);
    connect(reply, &QNetworkReply::finished, this, &ThrottledNetworkFetcher::onFinished);
//    connect(reply, &QNetworkReply::destroyed,
//            this, &ThrottledNetworkFetcher::onFinished, Qt::QueuedConnection);
    ++m_active;
}

void NetworkIOManager::addURLTemplate(const QString urlTemplate)
{
    NAM::instance().addURLMultiTemplate(urlTemplate);
}

void NetworkIOManager::requestSlippyTiles(MapFetcher *f,
                                          quint64 requestId,
                                          const QList<QGeoCoordinate> &crds,
                                          const quint8 zoom,
                                          quint8 destinationZoom,
                                          bool compound)
{
    if (crds.isEmpty()) {
        qWarning() << "requestSlippyTiles: Invalid bounds";
        return;
    }

    MapFetcherWorker *w = getMapFetcherWorker(f);
    w->setURLTemplate(f->urlTemplate()); // it might change in between requests
    w->requestSlippyTiles(requestId, crds, zoom, destinationZoom, compound);
}

void NetworkIOManager::requestSlippyTiles(DEMFetcher *f,
                                          quint64 requestId,
                                          const QList<QGeoCoordinate> &crds,
                                          const quint8 zoom,
                                          quint8 destinationZoom)
{
    if (crds.isEmpty()) {
        qWarning() << "requestSlippyTiles: Invalid bounds";
        return;
    }
    DEMFetcherWorker *w = getDEMFetcherWorker(f);
    w->setURLTemplate(f->urlTemplate()); // it might change in between requests
    w->requestSlippyTiles(requestId, crds, zoom, destinationZoom, true);
}

void NetworkIOManager::requestCoverage(MapFetcher *f,
                                       quint64 requestId,
                                       const QList<QGeoCoordinate> &crds,
                                       const quint8 zoom,
                                       const bool clip) {
    if (crds.isEmpty()) {
        qWarning() << "requestCoverage: Invalid bounds";
        return;
    }
    MapFetcherWorker *w = getMapFetcherWorker(f);
    w->setURLTemplate(f->urlTemplate()); // it might change in between requests
    w->requestCoverage(requestId, crds, zoom, clip);
}

void NetworkIOManager::requestCoverage(DEMFetcher *f,
                                       quint64 requestId,
                                       const QList<QGeoCoordinate> &crds,
                                       const quint8 zoom,
                                       const bool clip) {
    if (crds.isEmpty()) {
        qWarning() << "requestCoverage: Invalid bounds";
        return;
    }
    DEMFetcherWorker *w = getDEMFetcherWorker(f);
    w->setURLTemplate(f->urlTemplate()); // it might change in between requests
    w->requestCoverage(requestId, crds, zoom, clip);
}

void NetworkIOManager::requestSlippyTiles(ASTCFetcher *f,
                                          quint64 requestId,
                                          const QList<QGeoCoordinate> &crds,
                                          const quint8 zoom,
                                          quint8 destinationZoom,
                                          bool compound)
{
    if (crds.isEmpty()) {
        qWarning() << "requestSlippyTiles: Invalid bounds";
        return;
    }
    ASTCFetcherWorker *w = getASTCFetcherWorker(f);
    w->setURLTemplate(f->urlTemplate()); // it might change in between requests
    w->requestSlippyTiles(requestId, crds, zoom, destinationZoom, compound);
}

void NetworkIOManager::requestCoverage(ASTCFetcher *f,
                                       quint64 requestId,
                                       const QList<QGeoCoordinate> &crds,
                                       const quint8 zoom,
                                       const bool clip)
{
    if (crds.isEmpty()) {
        qWarning() << "requestCoverage: Invalid bounds";
        return;
    }
    ASTCFetcherWorker *w = getASTCFetcherWorker(f);
    w->setURLTemplate(f->urlTemplate()); // it might change in between requests
    w->requestCoverage(requestId, crds, zoom, clip);
}

quint64 NetworkIOManager::cacheSize() {
    return NAM::instance().cacheSize();
}

QString NetworkIOManager::cachePath() {
    return NAM::instance().cachePath();
}

MapFetcherWorker *NetworkIOManager::getMapFetcherWorker(MapFetcher *f) {
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
        connect(w,
                SIGNAL(requestHandlingFinished(quint64)),
                f,
                SIGNAL(requestHandlingFinished(quint64)), Qt::QueuedConnection);
    } else {
        w = it->second;
    }
    return w;
}

DEMFetcherWorker *NetworkIOManager::getDEMFetcherWorker(DEMFetcher *f) {
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
        connect(w,
                SIGNAL(requestHandlingFinished(quint64)),
                f,
                SIGNAL(requestHandlingFinished(quint64)), Qt::QueuedConnection);
    } else {
        w = it->second;
    }
    return w;
}

ASTCFetcherWorker *NetworkIOManager::getASTCFetcherWorker(ASTCFetcher *f) {
    ASTCFetcherWorker *w;
    auto it = m_astcFetcher2Worker.find(f);
    if (it == m_astcFetcher2Worker.end()) {
        init();
        if (!m_workerASTC)
            m_workerASTC = QSharedPointer<ThreadedJobQueue>(new ThreadedJobQueue(8));
        w = new ASTCFetcherWorker(this, f, m_worker, m_workerASTC);
        m_astcFetcher2Worker.insert({f, w});
        connect(w,
                &MapFetcherWorker::tileReady,
                f,
                &ASTCFetcher::onInsertTile, Qt::QueuedConnection); // onInsertTile is overridden in ASTCFetcher
        connect(w,
                &ASTCFetcherWorker::tileASTCReady,
                f,
                &ASTCFetcher::onInsertASTCTile, Qt::QueuedConnection);
        connect(w,
                &ASTCFetcherWorker::coverageASTCReady,
                f,
                &ASTCFetcher::onInsertASTCCoverage, Qt::QueuedConnection);
        connect(w,
                SIGNAL(requestHandlingFinished(quint64)),
                f,
                SIGNAL(requestHandlingFinished(quint64)), Qt::QueuedConnection);
    } else {
        w = it->second;
    }
    return w;
}

// Currently used only for DEM
void DEMFetcherWorker::onTileReady(const quint64 id, const TileKey k,  std::shared_ptr<QImage> i)
{
    Q_D(DEMFetcherWorker);
    if (!i) {
        qWarning() << "DEMFetcher::onTileReady: "<<k<< " not ready!";
        return;
    }
    if (!d->m_borders) {
        auto h = new DEMReadyData(std::move(i),
                                     k,
                                     *this,
                                     id,
                                     false);
        d->m_worker->schedule(h);
    } else {
        if (d->m_request2Neighbors.find(id) == d->m_request2Neighbors.end()) {
            qWarning() << "Neighbors not existing for request: "<<id;
            return;
        }
        TileNeighborsMap &tileNeighbors = d->m_request2Neighbors[id];
        // check if all borders are available
        if (tileNeighbors.find(k) == tileNeighbors.end()) {
            // error
            qWarning() << "Warning: neighbors missing for tile "<<k;
            return;
        }

        d->m_tileCache[id][k] = i; // f->onInsertTile also emits

        auto neighborsComplete = [&tileNeighbors](const TileKey &k) {
            auto &nm = tileNeighbors[k];
            for (const auto n: neighbors) {
                if (nm.first.testFlag(n) && !nm.second[n])
                    return false;
            }
            return true;
        };
        auto existsNeighbor = [k, &tileNeighbors](Heightmap::Neighbor n) {
            TileKey nk = k + neighborOffsets.at(n);
            const auto res = tileNeighbors.find(nk) != tileNeighbors.end();
            return res;
        };

        auto handleNeighborsComplete = [this, d, id, &tileNeighbors](TileKey tk) {
            auto tt = d->peekTile(id, tk);
            if (!bool(tt))
                return;
            tt.reset();
            std::map<Heightmap::Neighbor, std::shared_ptr<QImage>> tileNeighbors_;
            tileNeighbors_.swap(tileNeighbors[tk].second);
            tileNeighbors.erase(tk);
            auto h = new DEMReadyData(d->tile(id, tk),
                                         tk,
                                         *this,
                                         id,
                                         false,
                                         std::move(tileNeighbors_));
            d->m_worker->schedule(h);
        };

        if (neighborsComplete(k)) {
            handleNeighborsComplete(k);
        }

        // Propagate tile into neighbors
        for (const auto n: neighbors) {
            if (existsNeighbor(n)) {
                TileKey nk = k + neighborOffsets.at(n);
                // TODO: add flag check?
                tileNeighbors[nk].second[neighborReciprocal.at(n)] = i;
                if (neighborsComplete(nk)) {
                    handleNeighborsComplete(nk);
                }
            }
        }
    }
}

void DEMFetcherWorker::onCoverageReady(quint64 id,  std::shared_ptr<QImage> i)
{
    Q_D(DEMFetcherWorker);
    auto h = new DEMReadyData(i,
                              {0,0,0},
                              *this,
                              id,
                              true);
    d->m_worker->schedule(h);
}

void DEMFetcherWorker::onInsertHeightmap(quint64 id, const TileKey k, std::shared_ptr<Heightmap> h)
{
    Q_D(DEMFetcherWorker);
    emit heightmapReady(id, k, std::move(h));
    if (!--d->m_request2remainingDEMHandlers[id]) {
        emit requestHandlingFinished(id);
    }
}

void DEMFetcherWorker::onInsertHeightmapCoverage(quint64 id, std::shared_ptr<Heightmap> h)
{
    emit heightmapCoverageReady(id, std::move(h));
    emit requestHandlingFinished(id);
}

MapFetcherWorker::MapFetcherWorker(QObject *parent,
                                   MapFetcher *f,
                                   QSharedPointer<ThreadedJobQueue> worker)
    : QObject(*new MapFetcherWorkerPrivate, parent) {
    Q_D(MapFetcherWorker);
    d->m_fetcher = f;
    d->m_worker = worker;
}

void MapFetcherWorker::requestSlippyTiles(quint64 requestId,
                                          const QList<QGeoCoordinate> &crds,
                                          const quint8 zoom,
                                          quint8 destinationZoom,
                                          bool compound) {
    Q_D(MapFetcherWorker);
    if (crds.isEmpty()) {
        qWarning() << "requestSlippyTiles: Invalid bounds";
        return;
    }
    const quint8 originalDestinationZoom = destinationZoom; // this for the case where dz is larger, and tiles have to be fragmented
    destinationZoom = std::min(zoom, destinationZoom); // clamp dz to zoom. can only be smaller

    auto tiles = tilesFromBounds(crds, destinationZoom); // Calculating using dz to avoid partial coverage
    for (const auto &t: tiles) {
        d->trackNeighbors(requestId,
                          {quint64(t.ts.x()), quint64(t.ts.y()), quint8(t.ts.zoom())},
                          t.nb,
                          originalDestinationZoom); // trackNeighbors has effect only with DEM
    }
    const QString urlTemplate = (d->m_urlTemplate.isEmpty())
            ? urlTemplateTerrariumS3
            : d->m_urlTemplate;
    const URLTemplate urlTemplates =  extractTemplates(urlTemplate);
    d->m_request2urlTemplate[requestId] = urlTemplate;
    d->m_request2sourceZoom[requestId] = zoom;
    auto *df = qobject_cast<DEMFetcherWorker *>(this);
    auto *af = qobject_cast<ASTCFetcherWorker *>(this);
    if (af)  // ASTCFetcher is a multistage fetcher that may request compound tiles
        af->d_func()->m_request2remainingASTCHandlers[requestId] = 0;
    quint64 srcTilesSize = tiles.size();
    std::vector<CachedCompoundTileData *> cachedCompoundTileHandlers;
    if (destinationZoom < zoom) {
        // split tiles to request
        quint64 destSideLength = 1 << int(destinationZoom);
        quint64 sideLength = 1 << int(zoom);
        const size_t numSubtiles = sideLength / destSideLength;
        std::set<GeoTileSpec> srcTiles;
        for (const auto &dt: tiles) {
            if (compound && CompoundTileCache::instance().initialized()) {
                QByteArray data = CompoundTileCache::instance().tileMD5(urlTemplate,
                                                                        dt.ts.x(),
                                                                        dt.ts.y(),
                                                                        zoom,
                                                                        destinationZoom);
                if (data.size()) {
                    cachedCompoundTileHandlers.push_back(new CachedCompoundTileData(
                                                               requestId
                                                             , {quint64(dt.ts.x()), quint64(dt.ts.y()), destinationZoom}
                                                             , zoom
                                                             , std::move(data)
                                                             , urlTemplate
                                                             , *this));
                    --srcTilesSize;
                    continue;
                }
            }

            for (size_t y = 0; y < numSubtiles; ++y) {
                for (size_t x = 0; x < numSubtiles; ++x) {
                    const size_t sx =
                            quint64((quint64(dt.ts.x()) * sideLength) / destSideLength) + x;
                    const size_t sy =
                            quint64((quint64(dt.ts.y()) * sideLength) / destSideLength) + y;
                    QGeoTileSpec st(dt.ts.plugin(), dt.ts.mapId(), zoom
                                    , sx
                                    , sy
                                    , dt.ts.version());
                    srcTiles.insert({st, dt.nb});
                }
            }
        }
        tiles.swap(srcTiles);
    }

    if (tiles.empty()) { // everything was cached
        emit requestHandlingFinished(requestId);
//        return;
    }

    d->m_request2remainingTiles.emplace(requestId, tiles.size());
    d->m_request2remainingHandlers.emplace(requestId,
                                           tiles.size() * subtilesPerTile(zoom, destinationZoom));
    // TODO: replace with polymorphism
    if (df) {
        df->d_func()->m_request2remainingDEMHandlers[requestId] =
                srcTilesSize * subtilesPerTile(zoom, destinationZoom); // TODO: deduplicate? m_request2remainingHandlers might be enough
    }

    if (af) {
        af->d_func()->m_request2remainingASTCHandlers[requestId] =
                srcTilesSize * subtilesPerTile(zoom, destinationZoom); // TODO: deduplicate? m_request2remainingHandlers might be enough
    }

    requestMapTiles(tiles,
                    urlTemplates.alternatives,
                    (compound) ? originalDestinationZoom : zoom,
                    requestId,
                    false,
                    d->m_nm,
                    this, SLOT(onTileReplyFinished()),
                    this, SLOT(networkReplyError(QNetworkReply::NetworkError)));

    for(auto h: cachedCompoundTileHandlers)
        d->m_worker->schedule(h);
}

void MapFetcherWorker::requestCoverage(quint64 requestId, const QList<QGeoCoordinate> &crds, const quint8 zoom, const bool clip) {
    Q_D(MapFetcherWorker);
    const auto tiles = tilesFromBounds(crds, zoom, true);
    if (tiles.empty())     {
        qWarning() << "requestCoverage: empty bounds";
        return;
    }
    const URLTemplate urlTemplates = extractTemplates((d->m_urlTemplate.isEmpty()) ? urlTemplateTerrariumS3 : d->m_urlTemplate);

    requestMapTiles(tiles,
                    urlTemplates.alternatives,
                    zoom,
                    requestId,
                    true,
                    d->m_nm,
                    this, SLOT(onTileReplyForCoverageFinished()),
                    this, SLOT(networkReplyError(QNetworkReply::NetworkError)));

    d->m_requests.insert({requestId,
                          {crds, zoom, tiles.size(), clip}});
}

std::shared_ptr<QImage> MapFetcherWorker::tile(quint64 requestId, const TileKey &k) {
    Q_D(MapFetcherWorker);
    return d->tile(requestId, k);
}

void MapFetcherWorker::setURLTemplate(const QString &urlTemplate) {
    Q_D(MapFetcherWorker);
    d->m_urlTemplate = urlTemplate;
}

void MapFetcherWorker::onTileReplyFinished() {
    Q_D(MapFetcherWorker);
    QNetworkReply *reply = static_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    // const bool cached = reply->attribute(QNetworkRequest::SourceIsFromCacheAttribute).toBool();

    disconnect(reply, &QNetworkReply::finished, this, &MapFetcherWorker::onTileReplyFinished);
    disconnect(reply, &QNetworkReply::errorOccurred,
               this, &MapFetcherWorker::networkReplyError);

    const quint64 id = reply->property("ID").toUInt();
    if (d->m_request2remainingTiles.find(id) == d->m_request2remainingTiles.end()) {
        qWarning() << "No tracked request with id "<<id;
    } else {
        d->m_request2remainingTiles[id]--;
    }
    const quint8 z = reply->property("z").toUInt();
    const quint8 dz = reply->property("dz").toUInt();


    auto *df = qobject_cast<DEMFetcherWorker *>(this);
    auto *af = qobject_cast<ASTCFetcherWorker *>(this);
    if (reply->error() != QNetworkReply::NoError) {
        reply->deleteLater();
        d->m_request2remainingHandlers[id] -= subtilesPerTile(z, dz); // subtilesPerTile > 1 only during fragmentation
        if (df) {
            df->d_func()->m_request2remainingDEMHandlers[id] -= subtilesPerTile(z, dz);
            if (df->d_func()->m_request2remainingDEMHandlers[id] <= 0) {// TODO: deduplicate? m_request2remainingHandlers might be enough
                emit requestHandlingFinished(id);
            }
        } else if (af) {
            af->d_func()->m_request2remainingASTCHandlers[id] -= subtilesPerTile(z, dz);
            if (af->d_func()->m_request2remainingASTCHandlers[id] <= 0) {// TODO: deduplicate? m_request2remainingHandlers might be enough
                emit requestHandlingFinished(id);
            }
        } else {
            if (d->m_request2remainingHandlers[id] <= 0) {
                emit requestHandlingFinished(id);
            }
        }
        return; // Already handled in networkReplyError
    }

    auto *handler = (df) ? new DEMTileReplyData(reply, *this)
                         : (af) ? new ASTCTileReplyData(reply, *this)
                                : new TileReplyData(reply, *this);
    d->m_worker->schedule(handler);
}

void MapFetcherWorker::onTileReplyForCoverageFinished() {
    Q_D(MapFetcherWorker);
    QNetworkReply *reply = static_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    // const bool cached = reply->attribute(QNetworkRequest::SourceIsFromCacheAttribute).toBool();

    disconnect(reply, SIGNAL(finished()), this, SLOT(onTileReplyForCoverageFinished()));
    disconnect(reply, SIGNAL(errorOccurred(QNetworkReply::NetworkError)),
               this, SLOT(networkReplyError(QNetworkReply::NetworkError)));

    if (reply->error() != QNetworkReply::NoError) {
        reply->deleteLater();
        return; // Already handled in networkReplyError
    }

    auto *handler = new TileReplyData(reply,
                                         *this);
    d->m_worker->schedule(handler);
}

void MapFetcherWorker::onInsertTile(const quint64 id,
                                    const TileKey k,
                                    std::shared_ptr<QImage> i,
                                    QByteArray md5) {
    Q_D(MapFetcherWorker);
    emit tileReady(id, k, i, md5);
    if (!--d->m_request2remainingHandlers[id] && !qobject_cast<DEMFetcherWorker *>(this)) {
        emit requestHandlingFinished(id);
    }
    if (d->m_request2sourceZoom.at(id) > k.z) { // do only on compound tiles
        CompoundTileCache::instance().insert(d->m_request2urlTemplate.at(id),
                              k.x,
                              k.y,
                              d->m_request2sourceZoom.at(id),
                              k.z,
                              md5,
                              *i);
    }
}

void MapFetcherWorker::onInsertCompressedTileData(const quint64 id,
                                                  const TileKey k,
                                                  std::shared_ptr<QByteArray> data) {
    Q_D(MapFetcherWorker);
    emit compressedTileDataReady(id, k, std::move(data));
    if (!--d->m_request2remainingHandlers[id] && !qobject_cast<DEMFetcherWorker *>(this)) {
        emit requestHandlingFinished(id);
    }
}

void MapFetcherWorker::onInsertCoverage(const quint64 id, std::shared_ptr<QImage> i) {
    emit coverageReady(id, i);
}

void MapFetcherWorker::networkReplyError(QNetworkReply::NetworkError) {
    QNetworkReply *reply = static_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    qWarning() << reply->error() << reply->errorString();
}

MapFetcherWorker::MapFetcherWorker(MapFetcherWorkerPrivate &dd, MapFetcher *f, QSharedPointer<ThreadedJobQueue> worker, QObject *parent)
:   QObject(dd, parent)
{
    Q_D(MapFetcherWorker);
    d->m_fetcher = f;
    d->m_worker = worker;
}

DEMFetcherWorkerPrivate::DEMFetcherWorkerPrivate() : MapFetcherWorkerPrivate() {}

void DEMFetcherWorkerPrivate::trackNeighbors(quint64 id,
                                             const TileKey &k,
                                             Heightmap::Neighbors n,
                                             quint8 destinationZoom)
{
    if (k.z == destinationZoom) {
        insertNeighbors(id, k, n, boundaryRasters);
    } else if (k.z < destinationZoom) {
        int nSubTiles = subtileSide(k.z, destinationZoom);
        auto getBoundaries = [nSubTiles, n](int x, int y) {
            Heightmap::Neighbors res;

            if (x > 0 || n.testFlag(Heightmap::Left))
                res |= Heightmap::Left;
            if (x < (nSubTiles - 1) || n.testFlag(Heightmap::Right))
                res |= Heightmap::Right;
            if (y > 0 || n.testFlag(Heightmap::Top))
                res |= Heightmap::Top;
            if (y < (nSubTiles - 1) || n.testFlag(Heightmap::Bottom))
                res |= Heightmap::Bottom;
            if (   (x > 0  && y > 0)
                || (x == 0  && y > 0  && n.testFlag(Heightmap::Left))
                || (x > 0  && y == 0  && n.testFlag(Heightmap::Top))
                || (x == 0  && y == 0 && n.testFlag(Heightmap::TopLeft)))
                res |= Heightmap::TopLeft;
            if (   (x <  (nSubTiles - 1) && y > 0)
                || (x == (nSubTiles - 1) && y > 0  && n.testFlag(Heightmap::Right))
                || (x <  (nSubTiles - 1) && y == 0 && n.testFlag(Heightmap::Top))
                || (x == (nSubTiles - 1) && y == 0 && n.testFlag(Heightmap::TopRight)))
                res |= Heightmap::TopRight;
            if (   (x > 0 && y < (nSubTiles - 1))
                || (x == 0 && y < (nSubTiles - 1)  && n.testFlag(Heightmap::Left))
                || (x > 0 && y == (nSubTiles - 1) && n.testFlag(Heightmap::Bottom))
                || (x == 0 && y == (nSubTiles - 1) && n.testFlag(Heightmap::BottomLeft)))
                res |= Heightmap::BottomLeft;
            if (   (x <  (nSubTiles - 1) && y < (nSubTiles - 1))
                || (x == (nSubTiles - 1) && y < (nSubTiles - 1)  && n.testFlag(Heightmap::Right))
                || (x <  (nSubTiles - 1) && y == (nSubTiles - 1) && n.testFlag(Heightmap::Bottom))
                || (x == (nSubTiles - 1) && y == (nSubTiles - 1) && n.testFlag(Heightmap::BottomRight)))
                res |= Heightmap::BottomRight;
            return res;
        };

        // derive destination neighbors
        for (int sy = 0; sy < nSubTiles; ++sy) {
            for (int sx = 0; sx < nSubTiles; ++sx) {
                const quint64 dx = k.x * nSubTiles + sx;
                const quint64 dy = k.y * nSubTiles + sy;

                m_request2Neighbors[id][{dx, dy, destinationZoom}] =
                    {getBoundaries(sx, sy), boundaryRasters};
            }
        }
    }
}

DEMFetcherWorker::DEMFetcherWorker(QObject *parent, DEMFetcher *f, QSharedPointer<ThreadedJobQueue> worker, bool borders)
    :   MapFetcherWorker(*new DEMFetcherWorkerPrivate, f, worker, parent)
{
    Q_D(DEMFetcherWorker);
    d->m_borders = borders;
    init();
}

void DEMFetcherWorker::init() {
    connect(this, &MapFetcherWorker::tileReady, this, &DEMFetcherWorker::onTileReady);
    connect(this, &MapFetcherWorker::coverageReady, this, &DEMFetcherWorker::onCoverageReady);
}

std::shared_ptr<QImage> MapFetcherWorkerPrivate::tile(quint64 id, const TileKey &k)
{
    const auto it = m_tileCache[id].find(k);
    if (it != m_tileCache[id].end()) {
        std::shared_ptr<QImage> res = std::move(it->second);
        m_tileCache[id].erase(it);
        return res;
    }
    return nullptr;
}

std::shared_ptr<QImage> MapFetcherWorkerPrivate::peekTile(quint64 id, const TileKey &k)
{
    const auto it = m_tileCache[id].find(k);
    if (it != m_tileCache[id].end())
        return it->second;
    return nullptr;
}

QString MapFetcherWorkerPrivate::objectName() const {
    Q_Q(const MapFetcherWorker);
    return q->objectName();
}

ASTCFetcherWorker::ASTCFetcherWorker(QObject *parent,
                                     ASTCFetcher *f,
                                     QSharedPointer<ThreadedJobQueue> worker,
                                     QSharedPointer<ThreadedJobQueue> workerASTC)
:   MapFetcherWorker(*new ASTCFetcherWorkerPrivate, f, worker, parent)
{
    Q_D(ASTCFetcherWorker);
    init();
    d->m_forwardUncompressed = f->forwardUncompressedTiles();
    d->m_workerASTC = std::move(workerASTC);
}

void ASTCFetcherWorker::setForwardUncompressed(bool enabled)
{
    Q_D(ASTCFetcherWorker);
    d->m_forwardUncompressed = enabled;
}

bool ASTCFetcherWorker::forwardUncompressed() const
{
    Q_D(const ASTCFetcherWorker);
    return d->m_forwardUncompressed;
}

void ASTCFetcherWorker::init()
{
    Q_D(ASTCFetcherWorker);
    connect(qobject_cast<ASTCFetcher *>(d->m_fetcher), &ASTCFetcher::forwardUncompressedTilesChanged,
            this, &ASTCFetcherWorker::setForwardUncompressed, Qt::QueuedConnection);
    connect(this, &MapFetcherWorker::tileReady, this, &ASTCFetcherWorker::onTileReady);
    connect(this, &MapFetcherWorker::compressedTileDataReady, this, &ASTCFetcherWorker::onCompressedTileDataReady);
    connect(this, &MapFetcherWorker::coverageReady, this, &ASTCFetcherWorker::onCoverageReady);
}

void ASTCFetcherWorker::onTileReady(quint64 id,
                                    const TileKey k,
                                    std::shared_ptr<QImage> i,
                                    QByteArray md5)
{
    Q_D(ASTCFetcherWorker);
    auto h = new Raster2ASTCData(std::move(i),
                                 k,
                                 *this,
                                 id,
                                 false,
                                 std::move(md5));
    d->m_workerASTC->schedule(h);
}

void ASTCFetcherWorker::onCompressedTileDataReady(quint64 id,
                                                  const TileKey k,
                                                  std::shared_ptr<QByteArray> data)
{
    Q_D(ASTCFetcherWorker);
    auto h = new Raster2ASTCData(std::move(data),
                                 k,
                                 *this,
                                 id,
                                 false);
    d->m_workerASTC->schedule(h);
}

void ASTCFetcherWorker::onCoverageReady(quint64 id,
                                        std::shared_ptr<QImage> i)
{
    Q_D(ASTCFetcherWorker);
    auto h = new Raster2ASTCData(std::move(i),
                                 {0,0,0},
                                 *this,
                                 id,
                                 true,
                                 {});
    d->m_workerASTC->schedule(h);
}

void ASTCFetcherWorker::onInsertTileASTC(quint64 id,
                                         const TileKey k,
                                         std::shared_ptr<CompressedTextureData> h)
{
    Q_D(ASTCFetcherWorker);
    emit tileASTCReady(id, k, std::move(h));
    if (!--d->m_request2remainingASTCHandlers[id]) {
//        emit requestHandlingFinished(id); // it's somewhat involved to avoid emitting this signal
                                            // in mapfetcherworker. So emit it only there, as this
                                            // astc tile will eventually be produced
        emit requestHandlingFinished(id);
    }
}

void ASTCFetcherWorker::onInsertCoverageASTC(quint64 id,
                                             std::shared_ptr<CompressedTextureData> h)
{
    emit coverageASTCReady(id, std::move(h));
}
