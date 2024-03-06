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

#include <QStandardPaths>
#include <QDirIterator>
#include <QDir>
#include <QFileInfo>

#include <iostream>
#include <string>
#include <cstdlib>
#include <vector>
#include <map>

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
template <class T>
void hash_combine(std::size_t& seed, const T& v) {
    seed ^= std::hash<T>()(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}
bool isPowerOf2(size_t x) {
    return (x & (x - 1)) == 0;
}
}

namespace std {
size_t hash<TileKey>::operator()(const TileKey& id) const {
    std::size_t seed = 0;
    hash_combine(seed, id.x);
    hash_combine(seed, id.y);
    hash_combine(seed, quint64(id.z));
    return seed;
}
}

struct TileKeyRegistrar
{
    TileKeyRegistrar()
    {
        qRegisterMetaType<TileKey>("TileKey");
        qRegisterMetaType<std::shared_ptr<QImage>>("QImageShared");
        qRegisterMetaType<std::shared_ptr<Heightmap>>("HeightmapShared");
    }
};

Q_GLOBAL_STATIC(TileKeyRegistrar, initTileKey)

struct GeoTileSpec {
    QGeoTileSpec ts;
    Heightmap::Neighbors nb;

    bool operator == (const GeoTileSpec &rhs) const {
        return ts == rhs.ts;
    }
    bool operator < (const GeoTileSpec &rhs) const {
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
const quint64 tileSize{256};
quint64 getX(const TileData& d) {
    return d.k.x;
}
quint64 getY(const TileData& d) {
    return d.k.y;
}
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
void requestMapTiles(const std::set<GeoTileSpec> &tiles,
                     const QString &urlTemplate,
                     const quint8 destinationZoom,
                     const quint64 id,
                     ThrottledNetworkFetcher &nam,
                     QObject *destFinished,
                     const char *onFinishedSlot,
                     QObject *destError,
                     const char *onErrorSlot) {

    for (const auto &t: tiles) {
        auto tileUrl = urlTemplate;
        tileUrl = tileUrl.replace(QStringLiteral("{x}"), QString::number(t.ts.x()))
                .replace(QStringLiteral("{y}"), QString::number(t.ts.y()))
                .replace(QStringLiteral("{z}"), QString::number(t.ts.zoom()));

        nam.requestTile(tileUrl,
                        {quint64(t.ts.x()), quint64(t.ts.y()), quint8(t.ts.zoom())},
                        destinationZoom,
                        id,
                        t.nb,
                        destFinished,
                        onFinishedSlot,
                        destError,
                        onErrorSlot);
    }
}
std::set<GeoTileSpec> tilesFromBounds(const QGeoCoordinate &ctl,
                                   const QGeoCoordinate &cbr,
                                   const quint8 zoom) {
    QGeoCameraTilesPrivate ct;
    ct.m_intZoomLevel = zoom;
    ct.m_sideLength = 1 << ct.m_intZoomLevel;
//    ct.m_pluginString;
//    ct.m_mapType;
    ct.m_mapVersion = 1;

    const QDoubleVector2D tl = ct.m_sideLength * QWebMercator::coordToMercator(ctl);
    const QDoubleVector2D br = ct.m_sideLength * QWebMercator::coordToMercator(cbr);
    const QDoubleVector2D tr(br.x(), tl.y());
    const QDoubleVector2D bl(tl.x(), br.y());

    // (0,0) at top left
    if (br.x() <= tl.x()
            || br.y() <= tl.y()) {
        qWarning() << "Invalid bounding box: " << ctl << " " << cbr << " "
                                                   << tl << " "<< br << " "
                                                   << QWebMercator::coordToMercator(ctl) << " "
                                                   << QWebMercator::coordToMercator(cbr);
        return {};
    }

    PolygonVector p;
    p << tl;
    p << tr;
    p << br;
    p << bl;
    p << tl;
    auto tiles = ct.tilesFromPolygon(p);
    quint64 minX, maxX, minY, maxY;
    std::set<QGeoTileSpec> tileSet(tiles.begin(), tiles.end());
    std::tie(minX, maxX, minY, maxY) = getMinMax(tileSet);

    auto getBoundaries = [minX, maxX, minY, maxY](const QGeoTileSpec &t) {
                                Heightmap::Neighbors res;
                                const quint64 &x = t.x();
                                const quint64 &y = t.y();
                                if (x > minX)
                                    res |= Heightmap::Left;
                                if (x < maxX)
                                    res |= Heightmap::Right;
                                if (y > minY)
                                    res |= Heightmap::Top;
                                if (y < maxY)
                                    res |= Heightmap::Bottom;
                                if (res.testFlag(Heightmap::Left) && res.testFlag(Heightmap::Top))
                                    res |= Heightmap::TopLeft;
                                if (res.testFlag(Heightmap::Right) && res.testFlag(Heightmap::Top))
                                    res |= Heightmap::TopRight;
                                if (res.testFlag(Heightmap::Left) && res.testFlag(Heightmap::Bottom))
                                    res |= Heightmap::BottomLeft;
                                if (res.testFlag(Heightmap::Right) && res.testFlag(Heightmap::Bottom))
                                    res |= Heightmap::BottomRight;
                                return res;
                            };
    std::set<GeoTileSpec> res;
    for (const auto &t: tileSet) {
        res.insert({t, getBoundaries(t)});
    }
    return res;
}
} // namespace

bool TileKey::operator==(const TileKey &o) const {
    return x == o.x
            && y == o.y
            && z == o.z;
}

bool TileKey::operator<(const TileKey &o) const {
    return z < o.z ||
            (z == o.z && y < o.y) ||
            (z == o.z && y == o.y && x < o.x);
}

QDebug operator<<(QDebug d, const TileKey &k) {
    QDebug nsp = d.nospace();
    nsp << "{"<<k.x <<","<<k.y<<","<<k.z<<"}";
    return d;
}

bool TileData::operator<(const TileData &o) const {
    return k < o.k;
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

void MapFetcher::requestSlippyTiles(const QGeoCoordinate &ctl,
                                     const QGeoCoordinate &cbr,
                                     const quint8 zoom,
                                     quint8 destinationZoom) {
    Q_D(MapFetcher);
    if (!ctl.isValid() || !cbr.isValid()) { // TODO: verify cbr is actually br of ctl
        qWarning() << "requestSlippyTiles: Invalid bounds";
        return;
    }
    destinationZoom = std::min(zoom, destinationZoom); // TODO: implement the other way

    auto tiles = tilesFromBounds(ctl, cbr, destinationZoom); // Calculating using dz to avoid partial coverage
    for (const auto &t: tiles) {
        d->trackNeighbors({quint64(t.ts.x()),
                           quint64(t.ts.y()),
                           destinationZoom}, t.nb);
    }
    if (destinationZoom != zoom) {
        // split tiles to request
        quint64 destSideLength = 1 << int(destinationZoom);
        quint64 sideLength = 1 << int(zoom);
        const size_t numSubtiles = sideLength / destSideLength;
        std::set<GeoTileSpec> srcTiles;
        for (const auto &dt: tiles) {
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

    const QString urlTemplate = (d->m_urlTemplate.isEmpty())
                                    ? urlTemplateTerrariumS3
                                    : d->m_urlTemplate;
    QVector<GeoTileSpec> ss(tiles.begin(), tiles.end());

    requestMapTiles(tiles,
                    urlTemplate,
                    destinationZoom,
                    0,
                    d->m_nm,
                    this, SLOT(onTileReplyFinished()),
                    this, SLOT(networkReplyError(QNetworkReply::NetworkError)));
}

namespace  {
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
}



void MapFetcher::onTileReplyFinished() {
    Q_D(MapFetcher);
    QNetworkReply *reply = static_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    // const bool cached = reply->attribute(QNetworkRequest::SourceIsFromCacheAttribute).toBool();

    disconnect(reply, SIGNAL(finished()), this, SLOT(onTileReplyFinished()));
    disconnect(reply, SIGNAL(errorOccurred(QNetworkReply::NetworkError)),
                this, SLOT(networkReplyError(QNetworkReply::NetworkError)));

    if (reply->error() != QNetworkReply::NoError) {
        reply->deleteLater();
        return; // Already handled in networkReplyError
    }

    auto *handler = new TileReplyHandler(reply, this);
    d->m_worker->schedule(handler);
}

void MapFetcher::networkReplyError(QNetworkReply::NetworkError) {
    QNetworkReply *reply = static_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    qWarning() << reply->error() << reply->errorString();
    reply->deleteLater();
}

std::shared_ptr<QImage> MapFetcher::tile(const TileKey k)
{
    Q_D(MapFetcher);
    return d->tile(k);
}

std::shared_ptr<QImage> MapFetcher::tileCoverage(quint64 id)
{
    Q_D(MapFetcher);
    auto it = d->m_coverages.find(id);
    if (it == d->m_coverages.end())
        return {};
    auto res = std::move(it->second);
    d->m_coverages.erase(it);
    return res;
}

std::map<TileKey, std::shared_ptr<QImage>> MapFetcher::tileCache() {
    Q_D(MapFetcher);
    auto cache = std::move(d->m_tileCache);
    d->m_tileCache.clear();
    return cache;
}

void MapFetcher::setURLTemplate(const QString &urlTemplate) {
    Q_D(MapFetcher);
    d->m_urlTemplate = urlTemplate;
}

QString MapFetcher::urlTemplate() const
{
    Q_D(const MapFetcher);
    return d->m_urlTemplate;
}

quint8 MapFetcher::zoomForCoverage(const QGeoCoordinate &ctl,
                                   const QGeoCoordinate &cbr,
                                   const size_t tileResolution,
                                   const size_t maxCoverageResolution) {
    if (tileResolution == 0
        || !ctl.isValid()
        || !cbr.isValid())
        return 0;

    quint64 minX, maxX, minY, maxY;
    for (int z = 1; z <= 20; ++z) {
        auto ts = tilesFromBounds(ctl, cbr, z);
        std::set<GeoTileSpec> &tileSet = ts;
        std::tie(minX, maxX, minY, maxY) = getMinMax(tileSet);
        const quint64 hTiles = maxX - minX + 1;
        const quint64 vTiles = maxY - minY + 1;
        if (std::min(hTiles * tileResolution, vTiles * tileResolution) > maxCoverageResolution)
            return z - 1;
    }
    return 20;
}

quint64 MapFetcher::cacheSize() {
    return NAM::instance().cacheSize();
}

QString MapFetcher::cachePath() {
    return NAM::instance().cachePath();
}

// returns request id. 0 is invalid
quint64 MapFetcher::requestCoverage(const QGeoCoordinate &ctl,
                                    const QGeoCoordinate &cbr,
                                    const quint8 zoom,
                                    const bool clip)
{
    Q_D(MapFetcher);
    if (!ctl.isValid() || !cbr.isValid()) // ToDo: check more toroughly
    {
        qWarning() << "requestCoverage: Invalid bounds";
        return 0;
    }


    const auto tiles = tilesFromBounds(ctl, cbr, zoom);
    const QString urlTemplate = (d->m_urlTemplate.isEmpty()) ? urlTemplateTerrariumS3 : d->m_urlTemplate;

    requestMapTiles(tiles,
                    urlTemplate,
                    zoom,
                    d->m_coverageRequestID,
                    d->m_nm,
                    this, SLOT(onTileReplyForCoverageFinished()),
                    this, SLOT(networkReplyError(QNetworkReply::NetworkError)));

    d->m_requests.insert({d->m_coverageRequestID,
                       {ctl, cbr, zoom, tiles.size(), clip}});

    auto res = d->m_coverageRequestID;
    ++d->m_coverageRequestID;
    return res;
}

void MapFetcher::onTileReplyForCoverageFinished()
{
    Q_D(MapFetcher);
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

    auto *handler = new TileReplyHandler(reply, this);
    d->m_worker->schedule(handler);
}

void MapFetcher::onInsertTile(const TileKey k, std::shared_ptr<QImage> i) {
    Q_D(MapFetcher);
    d->m_tileCache.emplace(k, std::move(i));
    emit tileReady(k);
}

void MapFetcher::onInsertCoverage(const quint64 id, std::shared_ptr<QImage> i)
{
    Q_D(MapFetcher);
    d->m_coverages.emplace(id, std::move(i));
    emit coverageReady(id);
}

std::map<TileKey, std::set<TileData> > &MapFetcher::tileCacheCache()
{
    Q_D(MapFetcher);
    return d->m_tileCacheCache;
}

Heightmap Heightmap::fromImage(const QImage &dem,
                               const std::map<Heightmap::Neighbor, std::shared_ptr<QImage> > &borders) {
    Heightmap h;

    auto elevationFromPixel = [](const QImage &i, int x, int y) {
        const QRgb px = i.pixel(x,y);
        const float res = float(qRed(px) * 256 +
                                qGreen(px) +
                                qBlue(px) / 256.0) - 32768.0;
        return res;
    };

    const bool hasBorders = borders.size();
    h.setSize((!hasBorders) ? dem.size() : dem.size() + QSize(2,2));
    for (int y = 0; y < dem.height(); ++y) {
        for (int x = 0; x < dem.width(); ++x) {
            const float elevationMeters = elevationFromPixel(dem, x, y);
            h.setElevation( x + ((hasBorders) ? 1 : 0)
                           ,y + ((hasBorders) ? 1 : 0)
                           ,elevationMeters);
        }
    }

#if 0
     // cloning neighbor value
    if (hasBorders) {
        for (int y = 0; y < h.m_size.height(); ++y ) {
            for (int x = 0; x < h.m_size.width(); ++x) {
                size_t offset = y * h.m_size.width() + x;
                size_t sOffset = y * h.m_size.width();
                if (   y > 0 && y < (h.m_size.height() - 1)
                    && x > 0 && x < (h.m_size.width() - 1)) {
                    continue;
                }
                if (!y) {
                    sOffset = h.m_size.width();
                } else if (y == h.m_size.height() - 1) {
                    sOffset = (y - 1) * h.m_size.width();
                }

                if (!x) {
                    sOffset += 1;
                } else if (x == h.m_size.width() - 1) {
                    sOffset += x - 1;
                } else {
                    sOffset += x;
                }
                h.elevations[offset] = h.elevations[sOffset];
            }
        }
    }
#endif
    if (hasBorders) {
        auto left = [&h, &borders, &elevationFromPixel] () {
            if (!borders.at(Heightmap::Left))
                return;
            const auto &other = borders.at(Heightmap::Left);
            for (int y = 1; y < h.m_size.height() - 1; ++y) {
                auto otherValue =
                        elevationFromPixel(*other, other->size().width()-1, y-1);
                auto thisValue = h.elevation(1, y);
                h.setElevation(0,y,(thisValue+otherValue)*.5);
            }
        };
        auto right = [&h, &borders, &elevationFromPixel] () {
            if (!borders.at(Heightmap::Right))
                return;
            const auto &other = borders.at(Heightmap::Right);
            for (int y = 1; y < h.m_size.height() - 1; ++y) {
                auto otherValue = elevationFromPixel(*other, 0, y-1);
                auto thisValue = h.elevation(h.m_size.width() - 2, y);
                h.setElevation(h.m_size.width() - 1, y, (thisValue+otherValue)*.5);
            }
        };
        auto top = [&h, &borders, &elevationFromPixel] () {
            if (!borders.at(Heightmap::Top))
                return;
            const auto &other = borders.at(Heightmap::Top);
            for (int x = 1; x < h.m_size.width() - 1; ++x) {
                auto otherValue = elevationFromPixel(*other, x - 1, other->size().height()-1);
                auto thisValue = h.elevation(x, 1);
                h.setElevation(x, 0, (thisValue+otherValue)*.5);
            }
        };
        auto bottom = [&h, &borders, &elevationFromPixel] () {
            if (!borders.at(Heightmap::Bottom))
                return;
            const auto &other = borders.at(Heightmap::Bottom);
            for (int x = 1; x < h.m_size.width() - 1; ++x) {
                auto otherValue = elevationFromPixel(*other, x - 1, 0);
                auto thisValue = h.elevation(x, h.m_size.height() - 2);
                h.setElevation(x, h.m_size.height() - 1, (thisValue+otherValue)*.5);
            }
        };
        auto topLeft = [&h, &borders, &elevationFromPixel] () {
            if (!borders.at(Heightmap::Top)
                || !borders.at(Heightmap::Left)
                || !borders.at(Heightmap::TopLeft))
                return;
            const auto &other = borders.at(Heightmap::TopLeft);
            auto tl = elevationFromPixel(*other, other->size().width()-1, other->size().height()-1);
            auto thisValue = h.elevation(1, 1);
            auto left = h.elevation(0, 1);
            auto top = h.elevation(1, 0);
            h.setElevation(0, 0, (thisValue+tl+left+top)*.25);
        };
        auto bottomLeft = [&h, &borders, &elevationFromPixel] () {
            if (!borders.at(Heightmap::Bottom)
                || !borders.at(Heightmap::Left)
                || !borders.at(Heightmap::BottomLeft))
                return;
            const auto &other = borders.at(Heightmap::BottomLeft);
            auto bl = elevationFromPixel(*other, other->size().width()-1, 0);
            auto thisValue = h.elevation(1, h.m_size.height() - 2);
            auto left = h.elevation(0, h.m_size.height() - 2);
            auto bottom = h.elevation(1, h.m_size.height() - 1);
            h.setElevation(0, h.m_size.height() - 1, (thisValue+bl+left+bottom)*.25);
        };
        auto topRight = [&h, &borders, &elevationFromPixel] () {
            if (!borders.at(Heightmap::Top)
                || !borders.at(Heightmap::Right)
                || !borders.at(Heightmap::TopRight))
                return;
            const auto &other = borders.at(Heightmap::TopRight);
            auto tr = elevationFromPixel(*other, 0, other->size().height()-1);
            auto thisValue = h.elevation(h.m_size.width() - 2, 1);
            auto right = h.elevation(h.m_size.width() - 1, 1);
            auto top = h.elevation(h.m_size.width() - 2, 0);
            h.setElevation(h.m_size.width() - 1, 0, (thisValue+tr+right+top)*.25);
        };
        auto bottomRight = [&h, &borders, &elevationFromPixel] () {
            if (!borders.at(Heightmap::Bottom)
                || !borders.at(Heightmap::Right)
                || !borders.at(Heightmap::BottomRight))
                return;
            const auto &other = borders.at(Heightmap::BottomRight);
            auto br = elevationFromPixel(*other, 0, 0);
            auto thisValue = h.elevation(h.m_size.width() - 2, h.m_size.height() - 2);
            auto right = h.elevation(h.m_size.width() - 1, h.m_size.height() - 2);
            auto bottom = h.elevation(h.m_size.width() - 2, h.m_size.height() - 1);
            h.setElevation(h.m_size.width() - 1, h.m_size.height() - 1, (thisValue+br+right+bottom)*.25);
        };
        left();
        right();
        top();
        bottom();
        topLeft();
        topRight();
        bottomLeft();
        bottomRight();
    }

    h.m_hasBorders = hasBorders;
    return h;
}

// TODO: add smarter approaches to allow different output res. E.g., cubic interpolation
void Heightmap::rescale(QSize size) {
    if (size == m_size || m_hasBorders) // TODO improve border handling here
        return;
    float hMultiplier = m_size.width() / float(size.width());
    float vMultiplier = m_size.height() / float(size.height());

    if (  (m_size.isEmpty())
          || (size.width() >= m_size.width())
          || (size.height() >= m_size.height())
          || (fmod(hMultiplier, 1) != 0.0)
          || (fmod(vMultiplier, 1) != 0.0)) {
        qWarning() << "Requested downsampling size "<<size<<" not supported for "<<m_size;
    }

    const size_t arraySize = size.width() * size.height();
    elevations.resize(arraySize);
    {
        size_t pixelsPerPatch = int(hMultiplier) * int(vMultiplier);
        std::vector<float> downsampled;
        for (size_t y = 0; y < size.height(); ++y) {
            for (size_t x = 0; x < size.width(); ++x) {
                float sum = 0;
                for (size_t iy = 0; iy < int(vMultiplier); ++iy) {
                    for (size_t ix = 0; ix < int(hMultiplier); ++ix) {
                        sum += elevations[(y*int(vMultiplier)+iy)*m_size.width()
                                + (x * int(hMultiplier) + ix)];
                    }
                }
                downsampled.push_back(sum / float(pixelsPerPatch));
            }
        }
        elevations.swap(downsampled);
    }
    m_size = size;
}

void Heightmap::rescale(int size) {
    if (m_hasBorders) // TODO improve border handling here
        return;
    int newWidth, newHeight;
    if (m_size.width() >= m_size.height()) {
        newWidth = size;
        newHeight = size * m_size.height() / m_size.width();
    } else {
        newHeight = size;
        newWidth = size * m_size.width() / m_size.height();
    }
    rescale(QSize(newWidth, newHeight));
}

void Heightmap::setSize(QSize size, float initialValue)
{
    m_size = size;
    elevations.resize(m_size.width() * m_size.height(), initialValue);
}

QSize Heightmap::size() const {return m_size;}

float Heightmap::elevation(int x, int y) const {
    return elevations[y*m_size.width()+x];
}

void Heightmap::setElevation(int x, int y, float e) {
    elevations[y*m_size.width()+x] = e;
}

ThreadedJobQueue::ThreadedJobQueue(QObject *parent): QObject(parent) {
    connect(&m_thread, &QThread::finished,
            this, &ThreadedJobQueue::next, Qt::QueuedConnection);
}

ThreadedJobQueue::~ThreadedJobQueue() {

}

void ThreadedJobQueue::schedule(ThreadedJob *handler) {
    handler->move2thread(m_thread);
    connect(handler, &ThreadedJob::finished, &m_thread, &QThread::quit);
    m_jobs.enqueue(handler);

    if (!m_thread.isRunning())
        next();
}

void ThreadedJobQueue::next() {
    if (m_thread.isRunning() || m_jobs.isEmpty()) {
        return;
    }

    m_currentJob = m_jobs.dequeue();
    connect(&m_thread, SIGNAL(started()), m_currentJob, SLOT(process()));
    m_thread.start();
}

MapFetcher::MapFetcher(QObject *parent)
: QObject(*new MapFetcherPrivate, parent) {
    Q_D(MapFetcher);
    initTileKey();
    d->m_worker->setParent(this);
}

MapFetcher::MapFetcher(MapFetcherPrivate &dd, QObject *parent)
:   QObject(dd, parent)
{
    Q_D(MapFetcher);
    initTileKey();
    d->m_worker->setParent(this);
}

MapFetcherPrivate::MapFetcherPrivate()
: QObjectPrivate()
, m_worker(new ThreadedJobQueue)
{
}

MapFetcherPrivate::~MapFetcherPrivate()
{
}

std::shared_ptr<QImage> MapFetcherPrivate::tile(const TileKey k)
{
    const auto it = m_tileCache.find(k);
    if (it != m_tileCache.end()) {
        std::shared_ptr<QImage> res = std::move(it->second);
        m_tileCache.erase(it);
        return res;
    }
    return nullptr;
}

std::shared_ptr<QImage> MapFetcherPrivate::peekTile(const TileKey k)
{
    const auto it = m_tileCache.find(k);
    if (it != m_tileCache.end())
        return it->second;
    return nullptr;
}

QString MapFetcherPrivate::objectName() const
{
    Q_Q(const MapFetcher);
    return q->objectName();
}

TileReplyHandler::TileReplyHandler(QNetworkReply *reply, MapFetcher *mapFetcher)
    : m_reply(reply), m_mapFetcher(mapFetcher)
{
    reply->setParent(this);
    connect(this, &TileReplyHandler::insertTile, m_mapFetcher, &MapFetcher::onInsertTile, Qt::QueuedConnection);
    connect(this, &TileReplyHandler::insertCoverage, m_mapFetcher, &MapFetcher::onInsertCoverage, Qt::QueuedConnection);
    connect(this, &TileReplyHandler::insertTile, this, &ThreadedJob::finished);
    connect(this, &TileReplyHandler::insertCoverage, this, &ThreadedJob::finished);
    connect(this, &TileReplyHandler::expectingMoreSubtiles, this, &ThreadedJob::finished);
}

TileReplyHandler::~TileReplyHandler() {}

ThreadedJob::ThreadedJob()
{
    connect(this, &ThreadedJob::finished, this, &QObject::deleteLater);
}

void ThreadedJob::move2thread(QThread &t) {
    std::function<void(QObject *)> moveChildren2Thread;
    moveChildren2Thread = [&t, &moveChildren2Thread](QObject *o) {
        o->moveToThread(&t);
        for (auto c: qAsConst(o->children())) {
            moveChildren2Thread(c);
        }
    };
    moveChildren2Thread(this);
    connect(this, &ThreadedJob::finished, &t, &QThread::quit);
}

void TileReplyHandler::process()
{
    if (!m_reply)
        return;
    if (m_reply->property("ID").isValid())
        processCoverageTile();
    else
        processStandaloneTile();
}

void TileReplyHandler::processStandaloneTile()
{
    auto reply = m_reply; //parented under this, no need for individual destruction

    QByteArray data = reply->readAll();
    if (!data.size()) {
        qWarning() << "Empty dem tile received";
        return;
    }

    const quint64 x = reply->property("x").toUInt();
    const quint64 y = reply->property("y").toUInt();
    const quint8 z = reply->property("z").toUInt();
    const quint8 dz = reply->property("dz").toUInt();
    const Heightmap::Neighbors boundaries = Heightmap::Neighbors(reply->property("b").toUInt());
    TileKey k;

    if (z == dz) {
        k = TileKey{x,y,z};
//        m_mapFetcher->d_func()->trackNeighbors(k, boundaries);
        emit insertTile(k, std::make_shared<QImage>(QImage::fromData(std::move(data))));
    } else {
        int destSideLength = 1 << dz;
        int sideLength = 1 << z;
        const size_t numSubtiles =   (sideLength / destSideLength);
        const size_t totSubTiles = numSubtiles * numSubtiles;
        quint64 dx = (x * destSideLength) / sideLength;
        quint64 dy = (y * destSideLength) / sideLength;
        TileKey dk{dx, dy, dz};
        k = dk;
        std::set<TileData> &subCache = m_mapFetcher->tileCacheCache()[dk]; // as this ThreadedJobQueue is currently mono-thread, this can be accessed without mutex
        subCache.insert({{x,y,z}, QImage::fromData(std::move(data))});

        if (subCache.size() == totSubTiles) {
            QImage image = assembleTileFromSubtiles(subCache);
//            m_mapFetcher->d_func()->trackNeighbors(k, boundaries); // They all have the same sourceTile bounds
            emit insertTile(TileKey{dx,dy,dz}, std::make_shared<QImage>(std::move(image)));
            m_mapFetcher->tileCacheCache().erase(dk);
        } else {
            emit expectingMoreSubtiles();
        }
    }
}

void TileReplyHandler::processCoverageTile()
{
    auto reply = m_reply;
    if (!reply)
        return;
    const quint64 id = reply->property("ID").toUInt();
    const quint64 x = reply->property("x").toUInt();
    const quint64 y = reply->property("y").toUInt();
    const quint8 z = reply->property("z").toUInt();
    auto d = m_mapFetcher->d_func();

    auto request = d->m_requests.find(id);
    if (request == d->m_requests.end()) {
        return; // belongs to an errored request;
    }

    QGeoCoordinate tlc, brc;
    quint8 zoom;
    quint64 totalTileCount;
    bool clip;
    std::tie(tlc, brc, zoom, totalTileCount, clip) = request->second;
    QByteArray data; ;

    if (reply->error() != QNetworkReply::NoError
            || !(data = reply->readAll()).size()) {
        // Drop the records and ignore this request

        qWarning() << "Tile request " << TileKey(x,y,z)
                   << " for request " << tlc<< "," <<brc<<","<<zoom<<"  FAILED";
        d->m_tileSets.erase(id);
        d->m_requests.erase(id);
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
    if (it == d->m_tileSets.end())
        return;
    auto tileSet = std::move(it->second);
    auto request = std::move(d->m_requests[id]);

    d->m_tileSets.erase(it);
    d->m_requests.erase(id);

    if (!tileSet.size())
        return;

    QGeoCoordinate tlc, brc;
    quint8 zoom;
    quint64 totalTileCount;
    bool clip;
    std::tie(tlc, brc, zoom, totalTileCount, clip) = request;

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
        int xleft, xright, ytop, ybot;
        const size_t sideLength = 1 << size_t(zoom);
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

    emit insertCoverage(id, std::make_shared<QImage>(std::move(res)));
}

ThrottledNetworkFetcher::ThrottledNetworkFetcher(QObject *parent, size_t maxConcurrentRequests)
: QObject(parent), m_nm(NAM::instance().nam()), m_maxConcurrent(maxConcurrentRequests)
{
}

void ThrottledNetworkFetcher::requestTile(const QUrl &u,
                                          const TileKey &k,
                                          const quint8 destinationZoom,
                                          const quint64 id,
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
        quint32 boundaries;
        QObject *destFinished;
        std::string onFinishedSlot;
        QObject *destError;
        std::string onErrorSlot;

        std::tie(u,k,dz,id,boundaries,destFinished,onFinishedSlot,destError,onErrorSlot) = m_pendingRequests.dequeue();
        request(u, k, dz, id, Heightmap::Neighbors(boundaries)
                , destFinished, onFinishedSlot.c_str(), destError, onErrorSlot.c_str());
    }
}

void ThrottledNetworkFetcher::request(const QUrl &u,
                                      const TileKey &k,
                                      const quint8 destinationZoom,
                                      const quint64 id,
                                      const Heightmap::Neighbors boundaries,
                                      QObject *destFinished,
                                      const char *onFinishedSlot,
                                      QObject *destError,
                                      const char *onErrorSlot)
{
    QNetworkRequest request;
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    request.setHeader(QNetworkRequest::UserAgentHeader, QCoreApplication::applicationName());
    request.setUrl(u);

    QNetworkReply *reply = m_nm.get(request);
    reply->setProperty("x",k.x);
    reply->setProperty("y",k.y);
    reply->setProperty("z",k.z);
    reply->setProperty("dz",destinationZoom);
    reply->setProperty("b", quint32(boundaries));
    if (id > 0)
        reply->setProperty("ID",id);
    if (destFinished && onFinishedSlot)
        connect(reply, SIGNAL(finished()), destFinished, onFinishedSlot);
    if (destError && onErrorSlot)
        connect(reply, SIGNAL(errorOccurred(QNetworkReply::NetworkError)), destError, onErrorSlot);
    connect(reply, &QNetworkReply::finished, this, &ThrottledNetworkFetcher::onFinished);
    ++m_active;
}

DEMFetcher::DEMFetcher(QObject *parent, bool borders)
:   MapFetcher(*new DEMFetcherPrivate, parent)
{
    Q_D(DEMFetcher);
    d->m_borders = borders;
    init();
}

std::shared_ptr<Heightmap> DEMFetcher::heightmap(const TileKey k)
{
    Q_D(DEMFetcher);
    const auto it = d->m_heightmapCache.find(k);
    if (it != d->m_heightmapCache.end()) {
        std::shared_ptr<Heightmap> res = std::move(it->second);
        d->m_heightmapCache.erase(it);
        return res;
    }
    return nullptr;
}

std::shared_ptr<Heightmap> DEMFetcher::heightmapCoverage(quint64 id)
{
    Q_D(DEMFetcher);
    auto it = d->m_heightmapCoverages.find(id);
    if (it == d->m_heightmapCoverages.end())
        return {};
    auto res = std::move(it->second);
    d->m_heightmapCoverages.erase(it);
    return res;
}

std::map<TileKey, std::shared_ptr<Heightmap> > DEMFetcher::heightmapCache()
{
    Q_D(DEMFetcher);
    auto cache = std::move(d->m_heightmapCache);
    d->m_heightmapCache.clear();
    return cache;
}

void DEMFetcher::setBorders(bool borders)
{
    Q_D(DEMFetcher);
    if (d->m_borders == borders)
        return;
    d->m_borders = borders; // FIXME: this must not happen while processing a request!!
}

void DEMFetcher::onTileReady(const TileKey k)
{
    Q_D(DEMFetcher);
    auto t = d->peekTile(k);
    if (!t) {
        qWarning() << "DEMFetcher::onTileReady: "<<k<< " not ready!";
        return;
    }
    if (!d->m_borders) {
        t.reset();
        auto h = new DEMReadyHandler(tile(k),
                                     k,
                                     *this,
                                     0);
        d->m_worker->schedule(h);
    } else {
        // check if all borders are available
        if (d->m_tileNeighbors.find(k) == d->m_tileNeighbors.end()) {
            // error
            qWarning() << "Warning: neighbors missing for tile "<<k;
            return;
        }
        auto neighborsComplete = [d](const TileKey &k) {
            auto &nm = d->m_tileNeighbors[k];
            for (const auto n: neighbors) {
                if (nm.first.testFlag(n) && !nm.second[n])
                    return false;
            }
            return true;
        };
        auto existsNeighbor = [k,d](Heightmap::Neighbor n) {
            TileKey nk = k + neighborOffsets.at(n);
            const auto res = d->m_tileNeighbors.find(nk) != d->m_tileNeighbors.end();
            return res;
        };

        auto handleNeighborsComplete = [this, d](TileKey tk) {
            auto tt = d->peekTile(tk);
            if (!bool(tt))
                return;
            tt.reset();
            std::map<Heightmap::Neighbor, std::shared_ptr<QImage>> tileNeighbors;
            tileNeighbors.swap(d->m_tileNeighbors[tk].second);
            d->m_tileNeighbors.erase(tk);
            auto h = new DEMReadyHandler(tile(tk),
                                         tk,
                                         *this,
                                         0,
                                         tileNeighbors);
            d->m_worker->schedule(h);
        };

        if (neighborsComplete(k))
            handleNeighborsComplete(k);

        // Propagate tile into neighbors
        for (const auto n: neighbors) {
            if (existsNeighbor(n)) {
                TileKey nk = k + neighborOffsets.at(n);
                // TODO: add flag check?
                d->m_tileNeighbors[nk].second[neighborReciprocal.at(n)] = t;
                if (neighborsComplete(nk))
                    handleNeighborsComplete(nk);
            }
        }

    }
}

void DEMFetcher::onCoverageReady(quint64 id)
{
    Q_D(DEMFetcher);
    auto c = tileCoverage(id);
    if (!c) {
        qWarning() << "DEMFetcher::onCoverageReady: "<<id<< " not ready!";
        return;
    }
    auto h = new DEMReadyHandler(c,{0,0,0},*this,id);
    d->m_worker->schedule(h);
}

void DEMFetcher::onInsertHeightmap(const TileKey k, std::shared_ptr<Heightmap> h)
{
    Q_D(DEMFetcher);
    d->m_heightmapCache.emplace(k, std::move(h));
    emit heightmapReady(k);
}

void DEMFetcher::onInsertHeightmapCoverage(quint64 id, std::shared_ptr<Heightmap> h)
{
    Q_D(DEMFetcher);
    d->m_heightmapCoverages.emplace(id, std::move(h));
    emit heightmapCoverageReady(id);
}

DEMFetcher::DEMFetcher(DEMFetcherPrivate &dd, QObject *parent)
:   MapFetcher(dd, parent)
{
    init();
}

void DEMFetcher::init()
{
    connect(this, &MapFetcher::tileReady, this, &DEMFetcher::onTileReady);
    connect(this, &MapFetcher::coverageReady, this, &DEMFetcher::onCoverageReady);
}

DEMReadyHandler::DEMReadyHandler(std::shared_ptr<QImage> demImage,
                                 const TileKey k,
                                 DEMFetcher &demFetcher,
                                 quint64 coverageId,
                                 std::map<Heightmap::Neighbor, std::shared_ptr<QImage> > neighbors)
    : m_demFetcher(&demFetcher)
    , m_demImage(std::move(demImage))
    , m_coverageId(coverageId)
    , m_key(k)
    , m_neighbors(neighbors)
{
    connect(this, &DEMReadyHandler::insertHeightmap, m_demFetcher, &DEMFetcher::onInsertHeightmap, Qt::QueuedConnection);
    connect(this, &DEMReadyHandler::insertHeightmapCoverage, m_demFetcher, &DEMFetcher::onInsertHeightmapCoverage, Qt::QueuedConnection);
    connect(this, &DEMReadyHandler::insertHeightmapCoverage, this, &ThreadedJob::finished);
    connect(this, &DEMReadyHandler::insertHeightmap, this, &ThreadedJob::finished);
}

DEMReadyHandler::~DEMReadyHandler() {}

void DEMReadyHandler::process()
{    
    if (!m_demImage) {
        qWarning() << "NULL image in DEM Generation!";
        return;
    }
    std::shared_ptr<Heightmap> h =
            std::make_shared<Heightmap>(Heightmap::fromImage(*m_demImage, m_neighbors));
    if (m_coverageId)
        emit insertHeightmapCoverage(m_coverageId, h);
    else
        emit insertHeightmap(m_key, h);
}

DEMFetcherPrivate::DEMFetcherPrivate() : MapFetcherPrivate() {}

DEMFetcherPrivate::~DEMFetcherPrivate() {}

void DEMFetcherPrivate::trackNeighbors(const TileKey k, Heightmap::Neighbors n) {
    m_tileNeighbors[k] = {n, boundaryRasters};
}



//TODO: dedicated event loop thread for piping queued slots . threaded network requests. Threaded gl texture upload
