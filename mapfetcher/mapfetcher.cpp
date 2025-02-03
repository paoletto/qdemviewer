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

QAtomicInt NetworkConfiguration::offline{false};
QAtomicInt NetworkConfiguration::astcEnabled{false};
QAtomicInt NetworkConfiguration::logNetworkRequests{false};

namespace  {
template <class T>
void hash_combine(std::size_t& seed, const T& v) {
    seed ^= std::hash<T>()(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
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
        qRegisterMetaType<std::shared_ptr<QByteArray>>("QByteArrayShared");
        qRegisterMetaType<std::shared_ptr<Heightmap>>("HeightmapShared");
        qRegisterMetaType<std::shared_ptr<CompressedTextureData>>("CompressedTextureDataShared");
    }
};

Q_GLOBAL_STATIC(TileKeyRegistrar, initTileKey)

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

bool TileKey::operator>(const TileKey &o) const
{
    return o < *this;
}

QDebug operator<<(QDebug d, const TileKey &k) {
    QDebug nsp = d.nospace();
    nsp << "{"<<k.x <<","<<k.y<<","<<k.z<<"}";
    return d;
}

bool TileData::operator<(const TileData &o) const {
    return k < o.k;
}

quint64 MapFetcher::requestSlippyTiles(const QList<QGeoCoordinate> &crds,
                                       const quint8 zoom,
                                       quint8 destinationZoom,
                                       bool compound) {
    Q_D(MapFetcher);
    return d->requestSlippyTiles(crds, zoom, destinationZoom, compound);
}

std::shared_ptr<QImage> MapFetcher::tile(quint64 id, const TileKey k)
{
    Q_D(MapFetcher);
    return d->tile(id, k);
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

void MapFetcher::setURLTemplate(const QString &urlTemplate) {
    Q_D(MapFetcher);

    d->m_urlTemplate = urlTemplate;
    NetworkManager::instance().addURLTemplate(urlTemplate);
}

QString MapFetcher::urlTemplate() const
{   // TODO: add mutex
    Q_D(const MapFetcher);
    return d->m_urlTemplate;
}

void MapFetcher::setMaximumZoomLevel(int maxzl)
{
    Q_D(MapFetcher);
    if (maxzl == d->m_maximumZoomLevel)
        return;
    d->m_maximumZoomLevel = maxzl;
    emit maximumZoomLevelChanged();
}

int MapFetcher::maximumZoomLevel() const
{
    Q_D(const MapFetcher);
    return d->m_maximumZoomLevel;
}

void MapFetcher::setOverzoom(bool enabled)
{
    Q_D(MapFetcher);
    if (enabled == d->m_overzoom)
        return;
    d->m_overzoom = enabled;
    emit overzoomChanged();
}

int MapFetcher::overzoom() const
{
    Q_D(const MapFetcher);
    return d->m_overzoom;
}

QString MapFetcher::compoundTileCachePath()
{
    return CompoundTileCache::cachePath();
}

quint64 MapFetcher::compoundTileCacheSize()
{
    return CompoundTileCache::cacheSize();
}

quint64 MapFetcher::networkCacheSize() {
    return NetworkManager::instance().cacheSize();
}

QString MapFetcher::networkCachePath() {
    return NetworkManager::instance().cachePath();
}

// returns request id. 0 is invalid
quint64 MapFetcher::requestCoverage(const QList<QGeoCoordinate> &crds,
                                    const quint8 zoom,
                                    const bool clip)
{
    Q_D(MapFetcher);
    return d->requestCoverage(crds, zoom, clip);
}

void MapFetcher::onInsertTile(quint64 id, const TileKey k, std::shared_ptr<QImage> i) {
    Q_D(MapFetcher);
    d->m_tileCache[id][k] = std::move(i);
    emit tileReady(id, k);
}

void MapFetcher::onInsertCoverage(quint64 id, std::shared_ptr<QImage> i)
{
    Q_D(MapFetcher);
    d->m_coverages[id] = std::move(i);
    emit coverageReady(id);
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
    float min_ = std::numeric_limits<float>::max();
    float max_ = std::numeric_limits<float>::min();
    for (int y = 0; y < dem.height(); ++y) {
        for (int x = 0; x < dem.width(); ++x) {
            const float elevationMeters = elevationFromPixel(dem, x, y);
            h.setElevation( x + ((hasBorders) ? 1 : 0)
                           ,y + ((hasBorders) ? 1 : 0)
                           ,elevationMeters);
            max_ = std::max(elevationMeters,max_); min_ = std::min(elevationMeters,min_);
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
#if 1
    if (hasBorders) {
        auto left = [&h, &borders, &elevationFromPixel, &min_, &max_] () {
            if (!borders.at(Heightmap::Left))
                return;
            const auto &other = borders.at(Heightmap::Left);
            for (int y = 1; y < h.m_size.height() - 1; ++y) {
                auto otherValue =
                        elevationFromPixel(*other, other->size().width()-1, y-1);
                auto thisValue = h.elevation(1, y);
                const float elevationMeters = (thisValue+otherValue)*.5;
                h.setElevation(0,y,elevationMeters);
                max_ = std::max(elevationMeters,max_); min_ = std::min(elevationMeters,min_);
            }
        };
        auto right = [&h, &borders, &elevationFromPixel, &min_, &max_] () {
            if (!borders.at(Heightmap::Right))
                return;
            const auto &other = borders.at(Heightmap::Right);
            for (int y = 1; y < h.m_size.height() - 1; ++y) {
                auto otherValue = elevationFromPixel(*other, 0, y-1);
                auto thisValue = h.elevation(h.m_size.width() - 2, y);
                const float elevationMeters = (thisValue+otherValue)*.5;
                h.setElevation(h.m_size.width() - 1, y, elevationMeters);
                max_ = std::max(elevationMeters,max_); min_ = std::min(elevationMeters,min_);
            }
        };
        auto top = [&h, &borders, &elevationFromPixel, &min_, &max_] () {
            if (!borders.at(Heightmap::Top))
                return;
            const auto &other = borders.at(Heightmap::Top);
            for (int x = 1; x < h.m_size.width() - 1; ++x) {
                auto otherValue = elevationFromPixel(*other, x - 1, other->size().height()-1);
                auto thisValue = h.elevation(x, 1);
                const float elevationMeters = (thisValue+otherValue)*.5;
                h.setElevation(x, 0, elevationMeters);
                max_ = std::max(elevationMeters,max_); min_ = std::min(elevationMeters,min_);
            }
        };
        auto bottom = [&h, &borders, &elevationFromPixel, &min_, &max_] () {
            if (!borders.at(Heightmap::Bottom))
                return;
            const auto &other = borders.at(Heightmap::Bottom);
            for (int x = 1; x < h.m_size.width() - 1; ++x) {
                auto otherValue = elevationFromPixel(*other, x - 1, 0);
                auto thisValue = h.elevation(x, h.m_size.height() - 2);
                const float elevationMeters = (thisValue+otherValue)*.5;
                h.setElevation(x, h.m_size.height() - 1, elevationMeters);
                max_ = std::max(elevationMeters,max_); min_ = std::min(elevationMeters,min_);
            }
        };
        auto topLeft = [&h, &borders, &elevationFromPixel, &min_, &max_] () {
            if (!borders.at(Heightmap::Top)
                || !borders.at(Heightmap::Left)
                || !borders.at(Heightmap::TopLeft))
                return;
            const auto &other = borders.at(Heightmap::TopLeft);
            auto tl = elevationFromPixel(*other, other->size().width()-1, other->size().height()-1);
            auto thisValue = h.elevation(1, 1);
            auto left = h.elevation(0, 1);
            auto top = h.elevation(1, 0);
            const float elevationMeters = (thisValue+tl+left+top)*.25;
            h.setElevation(0, 0, elevationMeters);
            max_ = std::max(elevationMeters,max_); min_ = std::min(elevationMeters,min_);
        };
        auto bottomLeft = [&h, &borders, &elevationFromPixel, &min_, &max_] () {
            if (!borders.at(Heightmap::Bottom)
                || !borders.at(Heightmap::Left)
                || !borders.at(Heightmap::BottomLeft))
                return;
            const auto &other = borders.at(Heightmap::BottomLeft);
            auto bl = elevationFromPixel(*other, other->size().width()-1, 0);
            auto thisValue = h.elevation(1, h.m_size.height() - 2);
            auto left = h.elevation(0, h.m_size.height() - 2);
            auto bottom = h.elevation(1, h.m_size.height() - 1);
            const float elevationMeters = (thisValue+bl+left+bottom)*.25;
            h.setElevation(0, h.m_size.height() - 1, elevationMeters);
            max_ = std::max(elevationMeters,max_); min_ = std::min(elevationMeters,min_);
        };
        auto topRight = [&h, &borders, &elevationFromPixel, &min_, &max_] () {
            if (!borders.at(Heightmap::Top)
                || !borders.at(Heightmap::Right)
                || !borders.at(Heightmap::TopRight))
                return;
            const auto &other = borders.at(Heightmap::TopRight);
            auto tr = elevationFromPixel(*other, 0, other->size().height()-1);
            auto thisValue = h.elevation(h.m_size.width() - 2, 1);
            auto right = h.elevation(h.m_size.width() - 1, 1);
            auto top = h.elevation(h.m_size.width() - 2, 0);
            const float elevationMeters = (thisValue+tr+right+top)*.25;
            h.setElevation(h.m_size.width() - 1, 0, elevationMeters);
            max_ = std::max(elevationMeters,max_); min_ = std::min(elevationMeters,min_);
        };
        auto bottomRight = [&h, &borders, &elevationFromPixel, &min_, &max_] () {
            if (!borders.at(Heightmap::Bottom)
                || !borders.at(Heightmap::Right)
                || !borders.at(Heightmap::BottomRight))
                return;
            const auto &other = borders.at(Heightmap::BottomRight);
            auto br = elevationFromPixel(*other, 0, 0);
            auto thisValue = h.elevation(h.m_size.width() - 2, h.m_size.height() - 2);
            auto right = h.elevation(h.m_size.width() - 1, h.m_size.height() - 2);
            auto bottom = h.elevation(h.m_size.width() - 2, h.m_size.height() - 1);
            const float elevationMeters = (thisValue+br+right+bottom)*.25;
            h.setElevation(h.m_size.width() - 1, h.m_size.height() - 1, elevationMeters);
            max_ = std::max(elevationMeters,max_); min_ = std::min(elevationMeters,min_);
        };
        left();
        right();
        top();
        bottom();
        topLeft();
        topRight();
        bottomLeft();
        bottomRight();
        h.m_minMax = QPair<float, float>(min_, max_);
    }
#endif
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
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                float sum = 0;
                for (int iy = 0; iy < int(vMultiplier); ++iy) {
                    for (int ix = 0; ix < int(hMultiplier); ++ix) {
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

void Heightmap::printMinMax() const
{
    float minValue = qInf();
    float maxValue = -qInf();
    for (const auto &v: elevations) {
        minValue = qMin(v, minValue);
        maxValue = qMax(v, maxValue);
    }
    qDebug() << "Heightmap min "<<minValue<<" - max "<<maxValue;
}

float Heightmap::elevation(int x, int y) const
{
    return elevations[y*m_size.width()+x];
}

void Heightmap::setElevation(int x, int y, float e)
{
    elevations[y*m_size.width()+x] = e;
}

QPair<float, float> Heightmap::minMax() const
{
    return m_minMax;
}


MapFetcher::MapFetcher(QObject *parent)
: QObject(*new MapFetcherPrivate, parent)
{
    initTileKey();
}

MapFetcher::MapFetcher(MapFetcherPrivate &dd, QObject *parent)
:   QObject(dd, parent)
{
    initTileKey();
}

MapFetcherPrivate::MapFetcherPrivate()
: QObjectPrivate() {}

MapFetcherPrivate::~MapFetcherPrivate() {}

std::shared_ptr<QImage> MapFetcherPrivate::tile(quint64 id, const TileKey k)
{
    const auto it = m_tileCache[id].find(k);
    if (it != m_tileCache[id].end()) {
        std::shared_ptr<QImage> res = std::move(it->second);
        m_tileCache[id].erase(it);
        return res;
    }
    return nullptr;
}

quint64 MapFetcherPrivate::requestSlippyTiles(const QList<QGeoCoordinate> &crds,
                                              const quint8 zoom,
                                              quint8 destinationZoom,
                                              bool compound)
{
    Q_Q(MapFetcher);
    int cappedZoom = qMin<int>(zoom, m_maximumZoomLevel);
    return NetworkManager::instance().requestSlippyTiles(*q, crds, cappedZoom, destinationZoom, compound);
}

quint64 MapFetcherPrivate::requestCoverage(const QList<QGeoCoordinate> &crds, const quint8 zoom, bool clip)
{
    Q_Q(MapFetcher);
    return NetworkManager::instance().requestCoverage(*q, crds, zoom, clip);
}

QString MapFetcherPrivate::objectName() const
{
    Q_Q(const MapFetcher);
    return q->objectName();
}

DEMFetcher::DEMFetcher(QObject *parent, bool borders)
:   MapFetcher(*new DEMFetcherPrivate, parent)
{
    Q_D(DEMFetcher);
    d->m_borders = borders;
}

std::shared_ptr<Heightmap> DEMFetcher::heightmap(quint64 id, const TileKey k)
{
    Q_D(DEMFetcher);
    const auto it = d->m_heightmapCache[id].find(k);
    if (it != d->m_heightmapCache[id].end()) {
        std::shared_ptr<Heightmap> res = std::move(it->second);
        d->m_heightmapCache[id].erase(it);
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

void DEMFetcher::setBorders(bool borders)
{
    Q_D(DEMFetcher);
    if (d->m_borders == borders)
        return;
    d->m_borders = borders; // FIXME: this must not happen while processing a request!!
}

void DEMFetcher::onInsertHeightmap(quint64 id, const TileKey k, std::shared_ptr<Heightmap> h)
{
    Q_D(DEMFetcher);
    d->m_heightmapCache[id][k] = std::move(h);
    emit heightmapReady(id, k);
}

void DEMFetcher::onInsertHeightmapCoverage(quint64 id, std::shared_ptr<Heightmap> h)
{
    Q_D(DEMFetcher);
    d->m_heightmapCoverages[id] = std::move(h);
    emit heightmapCoverageReady(id);
}

DEMFetcher::DEMFetcher(DEMFetcherPrivate &dd, QObject *parent)
:   MapFetcher(dd, parent)
{
}

DEMFetcherPrivate::DEMFetcherPrivate() : MapFetcherPrivate() {}

DEMFetcherPrivate::~DEMFetcherPrivate() {}

quint64 DEMFetcherPrivate::requestSlippyTiles(const QList<QGeoCoordinate> &crds,
                                              const quint8 zoom,
                                              quint8 destinationZoom,
                                              bool)
{
    Q_Q(DEMFetcher);
    int cappedZoom = qMin<int>(zoom, m_maximumZoomLevel);
    return NetworkManager::instance().requestSlippyTiles(*q, crds, cappedZoom, destinationZoom);
}

quint64 DEMFetcherPrivate::requestCoverage(const QList<QGeoCoordinate> &crds, const quint8 zoom, bool clip)
{
    Q_Q(DEMFetcher);
    return NetworkManager::instance().requestCoverage(*q, crds, zoom, clip);
}

std::shared_ptr<CompressedTextureData> ASTCFetcherPrivate::tileASTC(quint64 id, const TileKey k)
{
    const auto it = m_tileCacheASTC[id].find(k);
    if (it != m_tileCacheASTC[id].end()) {
        std::shared_ptr<CompressedTextureData> res = std::move(it->second);
        m_tileCacheASTC[id].erase(it);
        return res;
    }
    return nullptr;
}

quint64 ASTCFetcherPrivate::requestSlippyTiles(const QList<QGeoCoordinate> &crds,
                                               const quint8 zoom,
                                               quint8 destinationZoom,
                                               bool compound)
{
    Q_Q(ASTCFetcher);
    int cappedZoom = qMin<int>(zoom, m_maximumZoomLevel);
    return NetworkManager::instance().requestSlippyTiles(*q, crds, cappedZoom, destinationZoom, compound);
}

quint64 ASTCFetcherPrivate::requestCoverage(const QList<QGeoCoordinate> &crds, const quint8 zoom, bool clip)
{
    Q_Q(ASTCFetcher);
    return NetworkManager::instance().requestCoverage(*q, crds, zoom, clip);
}

ASTCFetcher::ASTCFetcher(QObject *parent)
:   MapFetcher(*new ASTCFetcherPrivate, parent) {}


std::shared_ptr<CompressedTextureData> ASTCFetcher::tile(quint64 id, const TileKey k)
{
    Q_D(ASTCFetcher);
    return d->tileASTC(id, k);
}

std::shared_ptr<CompressedTextureData> ASTCFetcher::tileCoverage(quint64 id)
{
    Q_D(ASTCFetcher);
    auto it = d->m_coveragesASTC.find(id);
    if (it == d->m_coveragesASTC.end())
        return {};
    auto res = std::move(it->second);
    d->m_coveragesASTC.erase(it);
    return res;
}

const QAtomicInt &ASTCFetcher::forwardUncompressedTiles() const
{   // TODO: add mutex
    Q_D(const ASTCFetcher);
    return d->m_forwardUncompressed;
}

void ASTCFetcher::onInsertTile(const quint64 id, const TileKey k, std::shared_ptr<QImage> i)
{
    Q_D(const ASTCFetcher);

    if (!d->m_forwardUncompressed || !NetworkConfiguration::astcEnabled)
        return;

    auto ctd = std::make_shared<ASTCCompressedTextureData>();
    ctd->m_image = i;
    ASTCFetcher::onInsertASTCTile(id, k, std::static_pointer_cast<CompressedTextureData>(ctd));
}

void ASTCFetcher::setForwardUncompressedTiles(bool enabled)
{
    Q_D(ASTCFetcher);
    if (enabled == d->m_forwardUncompressed)
        return;

    d->m_forwardUncompressed = enabled;
    emit forwardUncompressedTilesChanged(enabled);
}

void ASTCFetcher::onInsertASTCTile(const quint64 id,
                                   const TileKey k,
                                   std::shared_ptr<CompressedTextureData> i)
{
    Q_D(ASTCFetcher);
    d->m_tileCacheASTC[id][k] = std::move(i);
    emit tileReady(id, k);
}

void ASTCFetcher::onInsertASTCCoverage(const quint64 id,
                                       std::shared_ptr<CompressedTextureData> i)
{
    Q_D(ASTCFetcher);
    d->m_coveragesASTC[id] = std::move(i);
    emit coverageReady(id);
}

std::vector<QImage> ASTCCompressedTextureData::m_white256;
std::vector<QTextureFileData> ASTCCompressedTextureData::m_white8x8ASTC;
std::vector<QTextureFileData> ASTCCompressedTextureData::m_transparent8x8ASTC;

namespace  {
void loadASTCMips(const QString &baseName, std::vector<QTextureFileData> &container) {
    for (int i : {256,128,64,32,16,8}) {
        QString fname = ":/" + baseName + QString::number(i) + "_8x8.astc";
        QFile f(fname);
        bool res = f.open(QIODevice::ReadOnly);
        if (!res) {
            qWarning()<<"Failed opening " <<f.fileName();
        }
        QTextureFileReader fr(&f);
        if (!fr.canRead())
            qWarning()<<"TFR cannot read texture!";

        container.push_back(fr.read());
        f.close();
    }
}
}

void ASTCCompressedTextureData::initStatics() {
    if (m_white256.size())
        return;
    Q_INIT_RESOURCE(qmake_mapfetcher_res);
    m_white256.resize(1);
    m_white256[0].load(":/white256.png");
    loadASTCMips("white", m_white8x8ASTC);
    loadASTCMips("transparent", m_transparent8x8ASTC);
}


URLTemplate extractTemplates(QString urlTemplate) {
    URLTemplate res;
    // TODO: support multiple subsets?
    int setStart = urlTemplate.indexOf('[');
    if (setStart < 6) {// account for the scheme
        QUrl u(urlTemplate);
        if (!u.isValid()) {
            qWarning() << "NetworkSqliteCache::addEquivalenceClass: invalid url template "<<u << u.errorString();
            return res;
        }
        res.alternatives.append(urlTemplate);
        return res;
    }

    int setEnd = urlTemplate.indexOf(']', setStart);

    QString setString = urlTemplate.mid(setStart + 1, setEnd - setStart - 1);
    if (setString.isEmpty()) {
        res.alternatives.append(urlTemplate);
        return res;
    }

    for (const auto &c: setString.split(',')) {
        auto t = urlTemplate;
        t.replace(setStart, setEnd - setStart + 1, c);
        res.alternatives.append(t);
        res.hostAlternatives.append(QUrl(t).host());
    }
    auto w = urlTemplate;
    w.replace(setStart, setEnd - setStart + 1, setString.split(',').join(QLatin1String("-")));
    res.hostWildcarded = QUrl(w).host();

    return res;
}

bool CompressedTextureData::isFormatCompressed(GLint format) {
    static std::set<GLint> compressedFormats {
        QOpenGLTexture::RGBA_ASTC_4x4,
                QOpenGLTexture::RGBA_ASTC_5x4,
                QOpenGLTexture::RGBA_ASTC_5x5,
                QOpenGLTexture::RGBA_ASTC_6x5,
                QOpenGLTexture::RGBA_ASTC_6x6,
                QOpenGLTexture::RGBA_ASTC_8x5,
                QOpenGLTexture::RGBA_ASTC_8x6,
                QOpenGLTexture::RGBA_ASTC_8x8,
                QOpenGLTexture::RGBA_ASTC_10x5,
                QOpenGLTexture::RGBA_ASTC_10x6,
                QOpenGLTexture::RGBA_ASTC_10x8,
                QOpenGLTexture::RGBA_ASTC_10x10,
                QOpenGLTexture::RGBA_ASTC_12x10,
                QOpenGLTexture::RGBA_ASTC_12x12,
                QOpenGLTexture::SRGB8_Alpha8_ASTC_4x4,
                QOpenGLTexture::SRGB8_Alpha8_ASTC_5x4,
                QOpenGLTexture::SRGB8_Alpha8_ASTC_5x5,
                QOpenGLTexture::SRGB8_Alpha8_ASTC_6x5,
                QOpenGLTexture::SRGB8_Alpha8_ASTC_6x6,
                QOpenGLTexture::SRGB8_Alpha8_ASTC_8x5,
                QOpenGLTexture::SRGB8_Alpha8_ASTC_8x6,
                QOpenGLTexture::SRGB8_Alpha8_ASTC_8x8,
                QOpenGLTexture::SRGB8_Alpha8_ASTC_10x5,
                QOpenGLTexture::SRGB8_Alpha8_ASTC_10x6,
                QOpenGLTexture::SRGB8_Alpha8_ASTC_10x8,
                QOpenGLTexture::SRGB8_Alpha8_ASTC_10x10,
                QOpenGLTexture::SRGB8_Alpha8_ASTC_12x10,
                QOpenGLTexture::SRGB8_Alpha8_ASTC_12x12
    };
    return compressedFormats.find(format) != compressedFormats.end();
}

void DEMFetcherWorkerPrivate::insertNeighbors(quint64 id,
                                              const TileKey &k,
                                              Heightmap::Neighbors n,
                                              std::map<Heightmap::Neighbor, std::shared_ptr<QImage> > boundaryRasters) {
    std::pair< Heightmap::Neighbors,
            std::map<Heightmap::Neighbor, std::shared_ptr<QImage>>> nm = {n, std::move(boundaryRasters)};
    m_request2Neighbors[id][k] = std::move(nm);
}
