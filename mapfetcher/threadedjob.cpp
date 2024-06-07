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

#include "mapfetcher_p.h"
#include "utils_p.h"
#include "astcencoder.h"

#include <QImage>
#include <QByteArray>

#include <QtPositioning/private/qwebmercator_p.h>
#include <QtPositioning/private/qdoublevector3d_p.h>

#include <QCoreApplication>
#include <QThread>

#include <vector>
#include <map>

namespace {
quint64 getX(const TileData& d) {
    return d.k.x;
}
quint64 getY(const TileData& d) {
    return d.k.y;
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
void setSubImage(QImage &dst, const QImage &src, const QPoint origin) {
    if (dst.width() < src.width() + origin.x()
            || dst.height() < src.height() + origin.y())
        return;

    for (int y = 0; y < src.height(); ++y) {
        for (int x = 0; x < src.width(); ++x) {
            const int dx = x + origin.x();
            const int dy = y + origin.y();
            const auto c = src.pixelColor(x, y);
            dst.setPixelColor(dx, dy, c);
        }
    }
}
QImage assembleTileFromSubtiles(const std::set<TileData> &subCache) {
    if (subCache.empty())
        return {};
    quint64 minX, maxX, minY, maxY;
    std::tie(minX, maxX, minY, maxY) = getMinMax(subCache);
    const size_t subTileRes = subCache.begin()->img.width();
    const size_t destRes = (maxX - minX + 1) * subTileRes;
    auto srcFormat = subCache.begin()->img.format();
    if (srcFormat == QImage::Format_Indexed8
            || srcFormat == QImage::Format_Mono
            || srcFormat == QImage::Format_MonoLSB
            || srcFormat == QImage::Format_Grayscale8
            || srcFormat == QImage::Format_Grayscale16) {
        srcFormat = QImage::Format_RGBA8888;
    }
    QImage res(destRes, destRes, srcFormat);

    for (const auto &t: subCache) {
        const QImage &i = t.img;
        const TileKey &k = t.k;
        setSubImage(res, i, QPoint((k.x - minX) * subTileRes,
                                   (k.y - minY) * subTileRes));
    }
    return res;
}
bool isEven(const QSize &s) {
    return (s.width() % 2) == 0 && (s.height() % 2) == 0;
}
QRgb meters2terrarium(float meters) {
//    Terrarium format PNG tiles contain raw elevation data in meters,
//    in Mercator projection (EPSG:3857).
//    All values are positive with a 32,768 offset, split into the red,
//    green, and blue channels, with 16 bits of integer and 8 bits of fraction. To decode:
//    (red * 256 + green + blue / 256) - 32768
    meters += 32768;
//            meters -= dem.minMax().first;
    float integral;
    float fractional = modf(meters, &integral);

    int r = (int(integral) >> 8) & 0xFF;
    int g = int(integral) & 0xFF;
    int b = fractional * 256;

    return qRgb(r,g,b);
}
} // namespace

bool ThreadedJobQueue::JobComparator::operator()(const ThreadedJobData *l, const ThreadedJobData *r) const
{
    if (l && r)
        return r->priority() < l->priority();
    return false;
}

ThreadedJobQueue::ThreadedJobQueue(size_t numThread, QObject *parent): QObject(parent)
{
    m_threads.resize(numThread);
    for (size_t i = 0; i < numThread; ++i) {
        m_threads[i] = new LoopedThread;
        m_threads[i]->setObjectName("ThreadedJobQueue " + objectName() + " Thread " + QString::number(i));
        m_currentJobs[m_threads[i]] = nullptr;
    }
}

ThreadedJobQueue::~ThreadedJobQueue() {
    for (size_t i = 0; i < m_threads.size(); ++i) {
        m_threads[i]->quit();
    }
    for (size_t i = 0; i < m_threads.size(); ++i) {
        m_threads[i]->wait();
    }
    for (size_t i = 0; i < m_threads.size(); ++i) {
        m_threads[i]->terminate();
    }
}

QThread *ThreadedJobQueue::findIdle()
{
    for (auto it: m_currentJobs) {
        if (it.second == nullptr)
            return it.first;
    }
    return nullptr;
}

void ThreadedJobQueue::schedule(ThreadedJobData *handler) {
    if (!handler) {
        qWarning() << "ThreadedJobQueue::schedule: null handler!";
        return;
    }
    m_jobs.push(handler);

    next(findIdle());
}

void ThreadedJobQueue::onJobDestroyed(QObject *o, QThread *t)
{
    Q_UNUSED(o)
    if (m_currentJobs.find(t) == m_currentJobs.end())
        qFatal("Job destroyed from the wrong thread!");
    m_currentJobs[t] = nullptr;
    next(findIdle());
}

void ThreadedJobQueue::next(QThread *t) {
    if (!t)
        return;

    if (m_jobs.empty()) {
        return;
    }
    auto jobData = m_jobs.top();
    auto job = ThreadedJob::fromData(jobData);
    if (!job) {// impossible?
        qWarning() << "ThreadedJobQueue::next : null next job!";
        next(t);
    }  else  {
        job->move2thread(*t);
        connect(job, &ThreadedJob::jobDestroyed,
                this, &ThreadedJobQueue::onJobDestroyed, Qt::QueuedConnection);
        m_currentJobs[t] = job;
        m_jobs.pop();

        if (!t->isRunning()) {
            t->start();
            t->setPriority(QThread::LowPriority); // TODO: try lowest?
        }

        QMetaObject::invokeMethod(job,
                                  "process",
                                  Qt::QueuedConnection);
    }
    QCoreApplication::processEvents(); // Somehow not helping
}


ThreadedJob::ThreadedJob()
{
    connect(this, &ThreadedJob::finished, this, &QObject::deleteLater);
    connect(this, &ThreadedJob::error, this, &QObject::deleteLater);
    connect(this, &QObject::destroyed, this, &ThreadedJob::onDestroyed);
}

void ThreadedJob::onDestroyed(QObject *j)
{
    emit jobDestroyed(j, QThread::currentThread());
}

// Might not be needed, according to doc "When a QObject is moved to another thread, all its children will be automatically moved too."
void ThreadedJob::move2thread(QThread &t) {
    std::function<void(QObject *)> moveChildren2Thread;
    moveChildren2Thread = [&t, &moveChildren2Thread](QObject *o) {
        o->moveToThread(&t);
        for (auto c: qAsConst(o->children())) {
            moveChildren2Thread(c);
        }
    };
    moveChildren2Thread(this);
}

TileReplyHandler::TileReplyHandler(QNetworkReply *reply,
                                   MapFetcherWorker &mapFetcher)
    : m_reply(reply), m_mapFetcher(&mapFetcher)
{
    reply->setParent(this);
    connect(this, &TileReplyHandler::insertTile,
            &mapFetcher, &MapFetcherWorker::onInsertTile, Qt::QueuedConnection);
    connect(this, &TileReplyHandler::insertCompressedTileData,
            &mapFetcher, &MapFetcherWorker::onInsertCompressedTileData, Qt::QueuedConnection);
    connect(this, &TileReplyHandler::insertCoverage,
            &mapFetcher, &MapFetcherWorker::onInsertCoverage, Qt::QueuedConnection);
    connect(this, &TileReplyHandler::insertTile, this, &ThreadedJob::finished);
    connect(this, &TileReplyHandler::insertCompressedTileData, this, &ThreadedJob::finished);
    connect(this, &TileReplyHandler::insertCoverage, this, &ThreadedJob::finished);
    connect(this, &TileReplyHandler::expectingMoreSubtiles, this, &ThreadedJob::finished);
}

void TileReplyHandler::process()
{
    if (!m_reply) {
        emit error();
        return;
    }
    if (m_reply->property("c").toBool())
        processCoverageTile();
    else
        processStandaloneTile();
}

void TileReplyHandler::processStandaloneTile()
{
    auto reply = m_reply; //parented under this, no need for individual destruction

    QByteArray data = reply->readAll();
    if (!data.size()) {
        qWarning() << "Empty dem tile received "<<reply->errorString();
        emit error();
        return;
    }

    const quint64 x = reply->property("x").toUInt();
    const quint64 y = reply->property("y").toUInt();
    const quint8 z = reply->property("z").toUInt();
    const quint8 dz = reply->property("dz").toUInt();
    const quint64 id = reply->property("ID").toUInt();
//    const Heightmap::Neighbors boundaries = Heightmap::Neighbors(reply->property("b").toUInt());
    TileKey k;

    QByteArray md5;
    if (z == dz) {
        k = TileKey{x,y,z};
        if (m_emitUncompressedData) {
            emit insertCompressedTileData(id,
                                          k,
                                          std::make_shared<QByteArray>(std::move(data)));
        } else {
            auto tile = std::make_shared<QImage>(QImage::fromData(std::move(data)).mirrored(false, !m_dem));
            if (m_computeHash)
                md5 = md5QImage(*tile);
            emit insertTile(id,
                            k,
                            std::move(tile),
                            std::move(md5));
        }
    } else if (z > dz) {
        int destSideLength = 1 << dz;
        int sideLength = 1 << z;
        const size_t numSubtiles =   (sideLength / destSideLength);
        const size_t totSubTiles = numSubtiles * numSubtiles;
        quint64 dx = (x * destSideLength) / sideLength;
        quint64 dy = (y * destSideLength) / sideLength;
        TileKey dk{dx, dy, dz};
        k = dk;
        std::set<TileData> &subCache = m_mapFetcher->d_func()->m_tileCacheCache[id][dk]; // as this ThreadedJobQueue is currently mono-thread, this can be accessed without mutex
        subCache.insert({{x,y,z}, QImage::fromData(std::move(data))});

        if (subCache.size() == totSubTiles) {
            QImage image = assembleTileFromSubtiles(subCache).mirrored(false, !m_dem);
            if (m_computeHash)
                md5 = md5QImage(image);
            emit insertTile(id,
                            TileKey{dx,dy,dz},
                            std::make_shared<QImage>(std::move(image)),
                            std::move(md5));
            m_mapFetcher->d_func()->m_tileCacheCache[id].erase(dk);
        } else {
            emit expectingMoreSubtiles();
        }
    } else { // z < dz -- split
        auto tile = QImage::fromData(std::move(data)).mirrored(false, !m_dem);
        int nSubTiles = 1 << (dz - z);
        int subTileSize = tile.size().width() / nSubTiles;

        if (nSubTiles > tile.size().width()) {
            qFatal("Requested too fine subdivision"); // TODO: don't fatal here
        }

        auto extractSubTile = [&tile, &subTileSize](int x, int y) {
            QImage res(subTileSize, subTileSize, tile.format());

            for (int sx = 0; sx < subTileSize; ++sx) {
                for (int sy = 0; sy < subTileSize; ++sy) {
                    res.setPixelColor(sx,sy,tile.pixelColor(x*subTileSize + sx,
                                                            y*subTileSize + sy));
                }
            }
            return res;
        };

        for (int sy = 0; sy < nSubTiles; ++sy) {
            for (int sx = 0; sx < nSubTiles; ++sx) {
                    auto t = std::make_shared<QImage>(extractSubTile(sx, sy));
                    if (m_computeHash)
                        md5 = md5QImage(*t);

                    emit insertTile(id,
                                    TileKey{x * nSubTiles + sx,
                                            y * nSubTiles + sy,
                                            dz},
                                    std::move(t),
                                    std::move(md5));
            }
        }
    }
}

void TileReplyHandler::processCoverageTile()
{
    auto reply = m_reply;
    if (!reply) {
        qWarning() << "NULL reply";
        emit error();
        return;
    }
    const quint64 id = reply->property("ID").toUInt();
    const quint64 x = reply->property("x").toUInt();
    const quint64 y = reply->property("y").toUInt();
    const quint8 z = reply->property("z").toUInt();
    auto d = m_mapFetcher->d_func();

    auto request = d->m_requests.find(id);
    if (request == d->m_requests.end()) {
        qWarning() << "processCoverageTile: request id not present";
        emit error();
        return; // belongs to an errored request;
    }

    QList<QGeoCoordinate> crds;
    quint8 zoom;
    quint64 totalTileCount;
    bool clip;
    std::tie(crds, zoom, totalTileCount, clip) = request->second;
    QByteArray data; ;

    if (reply->error() != QNetworkReply::NoError
            || !(data = reply->readAll()).size()) {
        // Drop the records and ignore this request

        qWarning() << "Tile request " << TileKey(x,y,z)
                   << " for request " << crds<<","<<zoom<<"  FAILED";
        d->m_tileSets.erase(id);
        d->m_requests.erase(id);
        emit error();
        return;
    }

    d->m_tileSets[id].insert({TileKey{x,y,z}, QImage::fromData(std::move(data))});
    if (d->m_tileSets[id].size() == totalTileCount) {
        // combine tiles and fire reply
        finalizeCoverageRequest(id);
    } else {
        emit expectingMoreSubtiles();
    }
}

void TileReplyHandler::finalizeCoverageRequest(quint64 id)
{
    auto d = m_mapFetcher->d_func();

    auto it = d->m_tileSets.find(id); // no need for mutex as worker thread is single thread
    if (it == d->m_tileSets.end()) {
        qWarning() << "finalizeCoverageRequest: request id not present";
        emit error();
        return;
    }
    auto tileSet = std::move(it->second);
    auto request = std::move(d->m_requests[id]);

    d->m_tileSets.erase(it);
    d->m_requests.erase(id);

    if (!tileSet.size()) {
        qWarning() << "finalizeCoverageRequest: empty tileSet";
        emit error();
        return;
    }

    QList<QGeoCoordinate> crds;
    quint8 zoom;
    quint64 totalTileCount;
    bool clip;
    std::tie(crds, zoom, totalTileCount, clip) = request;

    // find min max to compute result extent
    quint64 minX, maxX, minY, maxY;
    std::tie(minX, maxX, minY, maxY) = getMinMax(tileSet);    

    const quint64 hTiles = maxX - minX + 1;
    const quint64 vTiles = maxY - minY + 1;

    const size_t tileRes = tileSet.begin()->img.size().width();

    auto srcFormat = tileSet.begin()->img.format();
    if (srcFormat == QImage::Format_Indexed8
            || srcFormat == QImage::Format_Mono
            || srcFormat == QImage::Format_MonoLSB
            || srcFormat == QImage::Format_Grayscale8
            || srcFormat == QImage::Format_Grayscale16) {
        srcFormat = QImage::Format_RGBA8888;
    }
    QImage res(QSize(hTiles * tileRes,
                     vTiles * tileRes),
                     srcFormat); // Same format of input tiles

    for (const auto &td: tileSet) {
        const auto &t = td.img;
        const auto &k = td.k;

        for (size_t y = 0; y < size_t(tileRes); ++y) {
            for (size_t x = 0; x < size_t(tileRes); ++x) {
                const int dx = (k.x - minX) * tileRes + x;
                const int dy = (k.y - minY) * tileRes + y;

                res.setPixelColor(dx,dy,t.pixelColor(x,y));
            }
        }
    }

    if (clip) {
        double minLat = qInf();
        double maxLat = -qInf();
        double minLon = qInf();
        double maxLon = -qInf();
        for (const auto &c: crds) {
            minLat = qMin(minLat, c.latitude());
            maxLat = qMax(maxLat, c.latitude());
            minLon = qMin(minLon, c.longitude());
            maxLon = qMax(maxLon, c.longitude());
        }

        QGeoCoordinate tlc(maxLat, minLon);
        QGeoCoordinate brc(minLat, maxLon);

        int xleft, xright, ytop, ybot;
        const size_t sideLength = 1 << size_t(zoom);
        // TODO: find tlc and brc in mercator space from crds, then use them here
        const QDoubleVector2D tl = sideLength * tileRes
                                   * QWebMercator::coordToMercator(tlc);
        const QDoubleVector2D br = sideLength * tileRes
                                   * QWebMercator::coordToMercator(brc);
        const QDoubleVector2D tileTl = QDoubleVector2D(minX * tileRes, minY * tileRes);
        const QDoubleVector2D tileBr = QDoubleVector2D(maxX * tileRes + tileRes,
                                                       maxY * tileRes + tileRes);

        xleft = std::max(0, int(tl.x() - tileTl.x()));
        xright = std::max(0, (fmod(tileBr.x() - br.x(), 1) == 0.0)
                        ? int(tileBr.x() - br.x())
                        : int(tileBr.x() - br.x()) - 1);
        ytop = std::max(0, int(tl.y() - tileTl.y()));
        ybot = std::max(0, (fmod(tileBr.y() - br.y(), 1) == 0.0)
                        ? int(tileBr.y() - br.y())
                        : int(tileBr.y() - br.y()) - 1);

        QImage clipped(res.size() - QSize(xleft + xright, ytop + ybot),
                       res.format());

        for (int dy = 0; dy < clipped.height(); ++dy) {
            for (int dx = 0; dx < clipped.width(); ++dx) {
                int sx = dx + xleft;
                int sy = dy + ytop;
                clipped.setPixel(dx, dy, res.pixel(sx, sy));
            }
        }

        res = std::move(clipped);
    }

    emit insertCoverage(id, std::make_shared<QImage>(res.mirrored(false, !m_dem)));
}

CachedCompoundTileHandler::CachedCompoundTileHandler(quint64 id, TileKey k, quint8 sourceZoom, QByteArray md5, QString urlTemplate, MapFetcherWorker &mapFetcher)
    : m_id(id)
    , m_key(k)
    , m_sourceZoom(sourceZoom)
    , m_md5(std::move(md5))
    , m_urlTemplate(std::move(urlTemplate))
    , m_mapFetcher(&mapFetcher) {
    connect(this, &CachedCompoundTileHandler::tileReady,
            &mapFetcher, &MapFetcherWorker::tileReady, Qt::QueuedConnection);
    connect(this, &CachedCompoundTileHandler::tileReady, this, &ThreadedJob::finished);
}

void CachedCompoundTileHandler::process() {
    QImage res = CompoundTileCache::instance().tile(m_urlTemplate,
                                                    m_key.x,
                                                    m_key.y,
                                                    m_sourceZoom,
                                                    m_key.z);
    if (res.isNull()) {
        emit error();
        return;
    }

    emit tileReady(m_id,
                   m_key,
                   std::make_shared<QImage>(std::move(res)),
                   std::move(m_md5));
}

DEMReadyHandler::DEMReadyHandler(std::shared_ptr<QImage> demImage,
                                 const TileKey k,
                                 DEMFetcherWorker &demFetcher,
                                 quint64 id,
                                 bool coverage,
                                 std::map<Heightmap::Neighbor, std::shared_ptr<QImage> > neighbors)
    : m_demFetcher(&demFetcher)
    , m_demImage(std::move(demImage))
    , m_requestId(id)
    , m_coverage(coverage)
    , m_key(k)
    , m_neighbors(neighbors)
{
    connect(this, &DEMReadyHandler::insertHeightmap, m_demFetcher, &DEMFetcherWorker::onInsertHeightmap, Qt::QueuedConnection);
    connect(this, &DEMReadyHandler::insertHeightmapCoverage, m_demFetcher, &DEMFetcherWorker::onInsertHeightmapCoverage, Qt::QueuedConnection);
    connect(this, &DEMReadyHandler::insertHeightmapCoverage, this, &ThreadedJob::finished);
    connect(this, &DEMReadyHandler::insertHeightmap, this, &ThreadedJob::finished);
}

void DEMReadyHandler::process()
{
    if (!m_demImage) {
        qWarning() << "NULL image in DEM Generation!";
        return;
    }
    std::shared_ptr<HeightmapBase> h =
            std::make_shared<Heightmap>(Heightmap::fromImage(*m_demImage, m_neighbors));

    if (m_coverage)
        emit insertHeightmapCoverage(m_requestId, h);
    else
        emit insertHeightmap(m_requestId, m_key, h);
}

int Raster2ASTCData::priority() const { return Raster2ASTCHandler::priority(); }


Raster2ASTCHandler::Raster2ASTCHandler(Raster2ASTCData *data)
    : d(data)
{
    if (!d)
        qFatal("Raster2ASTCHandler(): null data");
    connect(this, &Raster2ASTCHandler::insertTileASTC, &d->m_fetcher, &ASTCFetcherWorker::onInsertTileASTC, Qt::QueuedConnection);
    connect(this, &Raster2ASTCHandler::insertCoverageASTC, &d->m_fetcher, &ASTCFetcherWorker::onInsertCoverageASTC, Qt::QueuedConnection);
    connect(this, &Raster2ASTCHandler::insertCoverageASTC, this, &ThreadedJob::finished);
    connect(this, &Raster2ASTCHandler::insertTileASTC, this, &ThreadedJob::finished);
}

void Raster2ASTCHandler::process()
{
    if (!d->m_rasterImage) {
        if (!d->m_compressedRaster) {
            qWarning() << "NULL image in Compressed Texture Generation!";
            return;
        }
        d->m_rasterImage =
                std::make_shared<QImage>(QImage::fromData(*d->m_compressedRaster).mirrored(false, true)); // TODO BEWARE of mirrored when doing the same on DEM!!!!!!!!!!

        d->m_md5 = md5QImage(*d->m_rasterImage);
    }
    std::shared_ptr<OpenGLTextureData> t =
            std::static_pointer_cast<OpenGLTextureData>(
                ASTCCompressedTextureData::fromImage(d->m_rasterImage,
                                                     d->m_k.x,
                                                     d->m_k.y,
                                                     d->m_k.z,
                                                     d->m_md5));

    if (d->m_coverage)
        emit insertCoverageASTC(d->m_id, t);
    else
        emit insertTileASTC(d->m_id, d->m_k, t);
}

DEMTileReplyHandler::DEMTileReplyHandler(QNetworkReply *reply, MapFetcherWorker &mapFetcher)
:   TileReplyHandler(reply, mapFetcher) {
    m_dem = true;
}

ASTCTileReplyHandler::ASTCTileReplyHandler(QNetworkReply *reply, MapFetcherWorker &mapFetcher)
    :   TileReplyHandler(reply, mapFetcher) {
    m_emitUncompressedData = true;
}

Heightmap2CompressedTextureHandler::Heightmap2CompressedTextureHandler(Heightmap2CompressedTextureData *data) : d(data)
{
    if (!d)
        qFatal("Heightmap2ASTCHandler(): null data");
    connect(this, &Heightmap2CompressedTextureHandler::insertCompressedHeightmap,
            &d->m_fetcher, &CompressedDEMFetcherWorker::onInsertCompressedHeightmap, Qt::QueuedConnection);
    connect(this, &Heightmap2CompressedTextureHandler::insertCompressedHeightmap,
            this, &ThreadedJob::finished);
}
Heightmap2CompressedTextureHandler::~Heightmap2CompressedTextureHandler() {}

const TileKey &Heightmap2CompressedTextureHandler::tileKey() const {
    return d->m_k;
}

struct HeightmapBPTC: public Heightmap
{
    ~HeightmapBPTC() override = default;

    HeightmapBPTC(Heightmap &&o) {
        m_minMax = std::move(o.m_minMax);
        m_size = std::move(o.m_size);
        elevations = std::move(o.elevations);
        m_hasBorders = o.m_hasBorders;
    };

    quint64 upload(QSharedPointer<QOpenGLTexture> &) override;
    quint64 uploadTo2DArray(QSharedPointer<QOpenGLTexture> &texArray,
                            int layer,
                            int layers) override;

protected:
    HeightmapBPTC() = default;
};

struct HeightmapASTC: public HeightmapBase, public ASTCHeightmapData
{
    ~HeightmapASTC() override = default;
    static std::shared_ptr<HeightmapBase> fromHeightmap(Heightmap &dem,
                                       qint64 x,
                                       qint64 y,
                                       qint64 z);

    quint64 upload(QSharedPointer<QOpenGLTexture> &) override;
    quint64 uploadTo2DArray(QSharedPointer<QOpenGLTexture> &texArray,
                            int layer,
                            int layers) override;
    bool bordersComputed() const override;
    QSize size() const override;
    Format format() const override;
    QPair<float, float> minMax() const override { return m_minMax; }

    bool m_bordersComputed{true};
    QPair<float, float> m_minMax;
protected:
    HeightmapASTC() = default;
};

void Heightmap2CompressedTextureHandler::process()
{
    // cast heightmapBase to heightmap
    Heightmap *h = static_cast<Heightmap *>(d->m_heightmap.get());
    if (!h)
        qFatal("Heightmap2ASTCHandler::process : null heightmap!");

    std::shared_ptr<HeightmapBase> res;
    if (NetworkConfiguration::astcHDREnabled) {
       res  = HeightmapASTC::fromHeightmap(*h, d->m_k.x, d->m_k.y, d->m_k.z);
    } else {
        res = std::make_shared<HeightmapBPTC>(std::move(*h));
    }

    emit insertCompressedHeightmap(d->m_id, d->m_k, std::move(res));
}

quint64 ASTCCompressedTextureData::upload(QSharedPointer<QOpenGLTexture> &t)
{
    if (!NetworkConfiguration::astcEnabled || !m_mips.size())
        return OpenGLTextureUtils::fillSingleTextureUncompressed(t, m_image);
    else
        return OpenGLTextureUtils::fillSingleTextureASTC(t, m_mips);
}

// TODO: move this code out of the library and into the application
quint64 ASTCCompressedTextureData::uploadTo2DArray(QSharedPointer<QOpenGLTexture> &texArray,
                                                   int layer,
                                                   int layers)
{
    if (!NetworkConfiguration::astcEnabled) { // || !m_mips.size()) {
        return OpenGLTextureUtils::fill2DArrayUncompressed(texArray,
                                                           m_image,
                                                           layer,
                                                           layers);
    } else {
        return OpenGLTextureUtils::fill2DArrayASTC(texArray,
                                                   m_mips,
                                                   layer,
                                                   layers);
    } // NetworkConfiguration::astcEnabled
}

QSize ASTCCompressedTextureData::size() const
{
    if (m_mips.size())
        return m_mips[0].size();
    else if (m_image)
        return m_image->size();
    return QSize();
}

bool ASTCCompressedTextureData::hasCompressedData() const
{
    return m_mips.size();
}

std::shared_ptr<OpenGLTextureData>  ASTCCompressedTextureData::fromImage(
        const std::shared_ptr<QImage> &i,
        qint64 x,
        qint64 y,
        qint64 z,
        QByteArray md5)
{
    std::shared_ptr<ASTCCompressedTextureData> res = std::make_shared<ASTCCompressedTextureData>();
    res->m_image = i;

    if (!NetworkConfiguration::astcEnabled)
        return res;

    res->initFromImage(i,x,y,z,md5);

    return res;
}

void ASTCCompressedTextureData::initFromImage(const std::shared_ptr<QImage> &i,
                                              qint64 x,
                                              qint64 y,
                                              qint64 z,
                                              QByteArray md5)
{
    QSize size(i->width(), i->height());
    if (!isEven(size)) {
        qWarning() << "Warning: cannot generate mips for size"<<size;
    }

    ASTCEncoder::instance().generateMips(*m_image,
                                         x,y,z,
                                         m_mips,
                                         std::move(md5));
}

quint64 ASTCHeightmapData::upload(QSharedPointer<QOpenGLTexture> &t)
{
    return OpenGLTextureUtils::fillSingleTextureASTC(t, m_mips);
}

quint64 ASTCHeightmapData::uploadTo2DArray(QSharedPointer<QOpenGLTexture> &texArray,
                                           int layer,
                                           int layers)
{
    qFatal("Not Implemented!");
    return 0;
}

QSize ASTCHeightmapData::size() const
{
    return m_size;
}

bool ASTCHeightmapData::hasCompressedData() const
{
    return true;
}

void ASTCHeightmapData::initFromHeightmap(const Heightmap *h,
                                      qint64 x,
                                      qint64 y,
                                      qint64 z,
                                      QByteArray md5) {
    if (!h)
        qFatal("NULL Heightmap!!");
    m_size = h->size();
    if (!isEven(m_size)) {
        qWarning() << "Warning: cannot generate mips for size"<<h->size();
    }

    m_mips.clear();

    std::vector<float> expandedScaled;
    expandedScaled.reserve(h->elevations.size() * 4);
    QPair<float, float> minMax = h->minMax();
    float range = minMax.second - minMax.first;
    for (const float &e: h->elevations)  { // TODO: do this after missing the cache!
        float val = (e - minMax.first) / range;

        expandedScaled.push_back(val);
        expandedScaled.push_back(val);
        expandedScaled.push_back(val);
        expandedScaled.push_back(val);
    }

    ASTCEncoder::instance(ASTCEncoderConfig::BlockSize12x12,
                          ASTCEncoderConfig::ASTCENC_PRE_EXHAUSTIVE,
                          ASTCEncoderConfig::ASTCENC_PRF_HDR_RGB_LDR_A,
                          {
                              ASTCEncoderConfig::ASTCENC_SWZ_R,
                              ASTCEncoderConfig::ASTCENC_SWZ_R,
                              ASTCEncoderConfig::ASTCENC_SWZ_R,
                              ASTCEncoderConfig::ASTCENC_SWZ_R
                          },
                          {1, 1, 1, 1}
                          ).generateHDRMip(expandedScaled,
                                           h->size(),
                                           x,y,z,
                                           false, //h->m_bordersComplete, TODO figure what's broken with this
                                           m_mips,
                                           std::move(md5));
}

std::shared_ptr<HeightmapBase> HeightmapASTC::fromHeightmap(Heightmap &dem,
                                           qint64 x,
                                           qint64 y,
                                           qint64 z)
{
    std::shared_ptr<HeightmapASTC> res(new HeightmapASTC);

    res->m_minMax = dem.minMax();
    res->m_bordersComputed = dem.bordersComputed();

    if (!NetworkConfiguration::astcHDREnabled)
        return res;

    // compress
    res->initFromHeightmap(&dem, x,y,z, std::move(dem.m_md5)); // discarding dem anyway
    return res;
}

quint64 HeightmapASTC::upload(QSharedPointer<QOpenGLTexture> &t) {
    return ASTCHeightmapData::upload(t);
}

quint64 HeightmapASTC::uploadTo2DArray(QSharedPointer<QOpenGLTexture> &texArray, int layer, int layers) {
    return ASTCHeightmapData::uploadTo2DArray(texArray, layer, layers);
}

quint64 HeightmapBPTC::upload(QSharedPointer<QOpenGLTexture> &t)
{
    return OpenGLTextureUtils::fillSingleTextureBPTC(t, m_size, elevations);
}

quint64 HeightmapBPTC::uploadTo2DArray(QSharedPointer<QOpenGLTexture> &texArray,
                                       int layer,
                                       int layers)
{
    qFatal("Not Implemented!");
    return 0;
}

bool HeightmapASTC::bordersComputed() const { return m_bordersComputed; }

QSize HeightmapASTC::size() const {
    return ASTCHeightmapData::size();
}


HeightmapBase::Format HeightmapASTC::format() const { return Format::CompressedFloat; }

OpenGLTextureData *HeightmapBase::asOpenGLTextureData() {
    return dynamic_cast<OpenGLTextureData *>(this);
}

ThreadedJob *ThreadedJob::fromData(ThreadedJobData *data)
{
    struct ScopeExit {
            ScopeExit(ThreadedJobData &data) : d{ &data } {}
            ~ScopeExit() { delete d; }
            ThreadedJobData *d;
            void release() { d = nullptr; }
    };
    ScopeExit deleter(*data);
    switch (data->type()) {
    case ThreadedJobData::JobType::TileReply: {
        TileReplyData *d = static_cast<TileReplyData *>(data);
        return new TileReplyHandler(d->m_reply,
                                    d->m_mapFetcher) ;
    }
    case ThreadedJobData::JobType::DEMTileReply: {
        DEMTileReplyData *d = static_cast<DEMTileReplyData *>(data);
        return new DEMTileReplyHandler(d->m_reply,
                                       d->m_mapFetcher) ;
    }
    case ThreadedJobData::JobType::ASTCTileReply: {
        ASTCTileReplyData *d = static_cast<ASTCTileReplyData *>(data);
        return new ASTCTileReplyHandler(d->m_reply,
                                        d->m_mapFetcher) ;
    }
    case ThreadedJobData::JobType::CachedCompoundTile: {
        CachedCompoundTileData *d = static_cast<CachedCompoundTileData *>(data);
        return new CachedCompoundTileHandler(d->m_id,
                                             d->m_k,
                                             d->m_sourceZoom,
                                             std::move(d->m_md5),
                                             d->m_urlTemplate,
                                             d->m_mapFetcher);
    }
    case ThreadedJobData::JobType::DEMReady: {
        DEMReadyData *d = static_cast<DEMReadyData *>(data);
        return new DEMReadyHandler(std::move(d->m_demImage),
                                   d->m_k,
                                   d->m_demFetcher,
                                   d->m_id,
                                   d->m_coverage,
                                   std::move(d->m_neighbors));
    }
    case ThreadedJobData::JobType::Raster2ASTC: {
        Raster2ASTCData *d = static_cast<Raster2ASTCData *>(data);
        deleter.release();
        return new Raster2ASTCHandler(std::move(d));
    }
    case ThreadedJobData::JobType::Heightmap2CompressedTexture: {
        Heightmap2CompressedTextureData *d = static_cast<Heightmap2CompressedTextureData *>(data);
        deleter.release();
        return new Heightmap2CompressedTextureHandler(std::move(d));
    }
    default:
        qWarning() << "Unsupported Job Type: " << int(data->type());
        return nullptr;
    }
}
