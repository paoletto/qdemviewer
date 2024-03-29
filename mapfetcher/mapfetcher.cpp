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
#include "astccache_p.h"

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
#include "astcenc.h"

QAtomicInt NetworkConfiguration::offline{false};
QAtomicInt NetworkConfiguration::astcEnabled{false};

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
//bool isPowerOf2(size_t x) {
//    return (x & (x - 1)) == 0;
//}
bool isEven(const QSize &s) {
    return (s.width() % 2) == 0 && (s.height() % 2) == 0;
}
}

// to use a single astc context
class ASTCEncoder
{
public:
    struct astc_header
    {
        uint8_t magic[4];
        uint8_t block_x;
        uint8_t block_y;
        uint8_t block_z;
        uint8_t dim_x[3];			// dims = dim[0] + (dim[1] << 8) + (dim[2] << 16)
        uint8_t dim_y[3];			// Sizes are given in texels;
        uint8_t dim_z[3];			// block count is inferred
    };

    static ASTCEncoder& instance()
    {
        static ASTCEncoder instance;
        return instance;
    }

    QTextureFileData compress(QImage ima) { // Check whether astcenc_compress_image modifies astcenc_image::data. If not, consider using const & and const_cast.
        // Compute the number of ASTC blocks in each dimension
        unsigned int block_count_x = (ima.width() + block_x - 1) / block_x;
        unsigned int block_count_y = (ima.height() + block_y - 1) / block_y;

        // Compress the image
        astcenc_image image;
        image.dim_x = ima.width();
        image.dim_y = ima.height();
        image.dim_z = 1;
        image.data_type = ASTCENC_TYPE_U8;
        uint8_t* slices = ima.bits();
        image.data = reinterpret_cast<void**>(&slices);

        // Space needed for 16 bytes of output per compressed block
        size_t comp_len = block_count_x * block_count_y * 16;

        QByteArray data;
        data.resize(comp_len);

        astcenc_error status = astcenc_compress_image(m_ctx,
                                                      &image,
                                                      &swizzle,
                                                      reinterpret_cast<uint8_t *>(data.data()),
                                                      comp_len,
                                                      0);
        if (status != ASTCENC_SUCCESS || !data.size()) {
            qWarning() << "ERROR: Codec compress failed: "<< astcenc_get_error_string(status);
            qFatal("Terminating");
        }

        astc_header hdr;
        hdr.magic[0] =  ASTC_MAGIC_ID        & 0xFF;
        hdr.magic[1] = (ASTC_MAGIC_ID >>  8) & 0xFF;
        hdr.magic[2] = (ASTC_MAGIC_ID >> 16) & 0xFF;
        hdr.magic[3] = (ASTC_MAGIC_ID >> 24) & 0xFF;

        hdr.block_x = static_cast<uint8_t>(block_x);
        hdr.block_y = static_cast<uint8_t>(block_y);
        hdr.block_z = static_cast<uint8_t>(1);

        hdr.dim_x[0] =  image.dim_x        & 0xFF;
        hdr.dim_x[1] = (image.dim_x >>  8) & 0xFF;
        hdr.dim_x[2] = (image.dim_x >> 16) & 0xFF;

        hdr.dim_y[0] =  image.dim_y       & 0xFF;
        hdr.dim_y[1] = (image.dim_y >>  8) & 0xFF;
        hdr.dim_y[2] = (image.dim_y >> 16) & 0xFF;

        hdr.dim_z[0] =  image.dim_z        & 0xFF;
        hdr.dim_z[1] = (image.dim_z >>  8) & 0xFF;
        hdr.dim_z[2] = (image.dim_z >> 16) & 0xFF;

        QByteArray header(reinterpret_cast<char*>(&hdr)
                          ,sizeof(astc_header));
        data.insert(0, header);
        QBuffer buf(&data);
        buf.open(QIODevice::ReadOnly);
        if (!buf.isReadable())
            qFatal("QBuffer not readable");
        QTextureFileReader reader(&buf);
        if (!reader.canRead())
            qFatal("QTextureFileReader failed reading");
        QTextureFileData res = reader.read();
        return res;
    }

    static QImage halve(const QImage &src) { // TODO: change into move once m_image is gone from CompressedTextureData?
        if ((src.width() % 2) != 0  || (src.height() % 2) != 0) {
            qWarning() << "Requested halving of size "<< QSize(src.width(), src.height()) <<" not supported";
            return src; // only do square power of 2 textures
        }

        const QSize size = QSize(src.width() / 2, src.height() / 2);
        const float hMultiplier = src.width() / float(size.width());
        const float vMultiplier = src.height() / float(size.height());
        QImage res(size, src.format());

        float pixelsPerPatch = int(hMultiplier) * int(vMultiplier);
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                float sumR = 0;
                float sumG = 0;
                float sumB = 0;
                float sumA = 0;

                for (int iy = 0; iy < int(vMultiplier); ++iy) {
                    for (int ix = 0; ix < int(hMultiplier); ++ix) {
                        auto p = src.pixel(x * int(hMultiplier) + ix,
                                           y * int(vMultiplier) + iy);
                        sumR += qRed(p);
                        sumG += qGreen(p);
                        sumB += qBlue(p);
                        sumA += qAlpha(p);
                    }
                }
                QRgb avg = qRgba(sumR / pixelsPerPatch
                                ,sumG / pixelsPerPatch
                                ,sumB / pixelsPerPatch
                                ,sumA / pixelsPerPatch);
                res.setPixel(x,y,avg);
            }
        }
        return res;
    }

    static int blockSize() {
        return block_x;
    }

    static QTextureFileData fromCached(QByteArray &cached) {
        QBuffer buf(&cached);
        buf.open(QIODevice::ReadOnly);
        if (!buf.isReadable())
            qFatal("QBuffer not readable");
        QTextureFileReader reader(&buf);
        if (!reader.canRead())
            qFatal("QTextureFileReader failed reading");
        QTextureFileData res = reader.read();
        return res;
    }

    void generateMips(const QImage &ima, std::vector<QTextureFileData> &out) {
        QCryptographicHash ch(QCryptographicHash::Md5);
        ch.addData(reinterpret_cast<const char *>(ima.constBits()), ima.sizeInBytes());
        QByteArray hash = ch.result();
        QSize size = ima.size();
        QByteArray cached = m_tileCache.tile(hash,
                                               block_x,
                                               block_y,
                                               quality,
                                               size.width(),
                                               size.height());
        if (cached.size()) {
            out.push_back(fromCached(cached));

            while (isEven(size)) {
                size = QSize(size.width() / 2, size.height() / 2);
                if (size.width() < ASTCEncoder::blockSize())
                    break;
                cached = m_tileCache.tile(hash,
                                           block_x,
                                           block_y,
                                           quality,
                                           size.width(),
                                           size.height());
                if (!cached.size())
                    break;
                out.push_back(fromCached(cached));
            }
        } else {
            out.emplace_back(ASTCEncoder::instance().compress(ima));
            m_tileCache.insert(hash,
                               block_x,
                               block_y,
                               quality,
                               size.width(),
                               size.height(),
                               out.back().data());
            QImage halved = ima;
            while (isEven(size)) {
                size = QSize(size.width() / 2, size.height() / 2);
                if (size.width() < ASTCEncoder::blockSize())
                    break;
                halved = ASTCEncoder::halve(halved);
                out.emplace_back(ASTCEncoder::instance().compress(halved));
                m_tileCache.insert(hash,
                                   block_x,
                                   block_y,
                                   quality,
                                   size.width(),
                                   size.height(),
                                   out.back().data());
            }
        }
    }

private:
    ASTCEncoder()
    : m_cacheDirPath(QStringLiteral("%1/astcCache.sqlite").arg(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)))
    , m_tileCache(m_cacheDirPath) {
        swizzle = { // QImage::Format_RGB32 == 0xffRRGGBB
            ASTCENC_SWZ_B,
            ASTCENC_SWZ_G,
            ASTCENC_SWZ_R,
            ASTCENC_SWZ_A
        };

        astcenc_error status;

        config.block_x = block_x;
        config.block_y = block_y;
        config.profile = profile;

        status = astcenc_config_init(profile, block_x, block_y, block_z, quality, 0, &config);
        if (status != ASTCENC_SUCCESS) {
            qWarning() << "ERROR: Codec config init failed: " << astcenc_get_error_string(status);
            qFatal("Terminating");
        }

        status = astcenc_context_alloc(&config, thread_count, &m_ctx);
        if (status != ASTCENC_SUCCESS) {
            qWarning() << "ERROR: Codec context alloc failed: "<< astcenc_get_error_string(status);
            qFatal("Terminating");
        }
    }

    ~ASTCEncoder() {
        if (m_ctx)
            astcenc_context_free(m_ctx);
    }

    astcenc_context *m_ctx{nullptr};
    astcenc_swizzle swizzle;
    astcenc_config config;
    QString m_cacheDirPath;
    ASTCCache m_tileCache;

    static const astcenc_profile profile = ASTCENC_PRF_LDR;

//    static const float ASTCENC_PRE_FASTEST = 0.0f;
//    static const float ASTCENC_PRE_FAST = 10.0f;
//    static const float ASTCENC_PRE_MEDIUM = 60.0f;
//    static const float ASTCENC_PRE_THOROUGH = 98.0f;
//    static const float ASTCENC_PRE_VERYTHOROUGH = 99.0f;
//    static const float ASTCENC_PRE_EXHAUSTIVE = 100.0f;

    constexpr static const float quality = 85.0f;

    static const unsigned int thread_count = 1;
    static const unsigned int block_x = 8;
    static const unsigned int block_y = 8;
//    static const unsigned int block_x = 4;
//    static const unsigned int block_y = 4;
//    static const unsigned int block_x = 6;
//    static const unsigned int block_y = 6;
    static const unsigned int block_z = 1;
    static const uint32_t ASTC_MAGIC_ID = 0x5CA1AB13;

public:
    ASTCEncoder(ASTCEncoder const&)            = delete;
    void operator=(ASTCEncoder const&)         = delete;
};

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
        qRegisterMetaType<std::shared_ptr<CompressedTextureData>>("CompressedTextureDataShared");
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
//const quint64 tileSize{256};
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
                     const bool coverageRequest,
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
                        coverageRequest,
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

quint64 MapFetcher::requestSlippyTiles(const QGeoCoordinate &ctl,
                                     const QGeoCoordinate &cbr,
                                     const quint8 zoom,
                                     quint8 destinationZoom) {
    Q_D(MapFetcher);
    return d->requestSlippyTiles(ctl, cbr, zoom, destinationZoom);
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
    return NetworkManager::instance().cacheSize();
}

QString MapFetcher::cachePath() {
    return NetworkManager::instance().cachePath();
}

// returns request id. 0 is invalid
quint64 MapFetcher::requestCoverage(const QGeoCoordinate &ctl,
                                    const QGeoCoordinate &cbr,
                                    const quint8 zoom,
                                    const bool clip)
{
    Q_D(MapFetcher);
    return d->requestCoverage(ctl, cbr, zoom, clip);
}

void MapFetcher::onInsertTile(const quint64 id, const TileKey k, std::shared_ptr<QImage> i) {
    Q_D(MapFetcher);
    d->m_tileCache[id].emplace(k, std::move(i));
    emit tileReady(id, k);
}

void MapFetcher::onInsertCoverage(const quint64 id, std::shared_ptr<QImage> i)
{
    Q_D(MapFetcher);
    d->m_coverages.emplace(id, std::move(i));
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

float Heightmap::elevation(int x, int y) const {
    return elevations[y*m_size.width()+x];
}

void Heightmap::setElevation(int x, int y, float e) {
    elevations[y*m_size.width()+x] = e;
}

ThreadedJobQueue::ThreadedJobQueue(QObject *parent): QObject(parent) {
    connect(&m_thread, &QThread::finished,
            this, &ThreadedJobQueue::next, Qt::QueuedConnection);
    m_thread.setObjectName("ThreadedJobQueue Thread");
}

ThreadedJobQueue::~ThreadedJobQueue() {

}

void ThreadedJobQueue::schedule(ThreadedJob *handler) {
    if (!handler) {
        qWarning() << "ThreadedJobQueue::schedule: null handler!";
        return;
    }

    handler->move2thread(m_thread);
    connect(handler, &ThreadedJob::finished, &m_thread, &QThread::quit);
    m_jobs.push(handler);

    if (!m_thread.isRunning()) {
        next();
    }
}

void ThreadedJobQueue::next() {
    if (m_thread.isRunning())
        return;
    if (m_jobs.empty())
        return;
    m_currentJob = m_jobs.top();
    m_jobs.pop();
    if (!m_currentJob) {// impossible?
        qWarning() << "ThreadedJobQueue::next : null next job!";
        next();
    } else  {
        connect(&m_thread, SIGNAL(started()), m_currentJob, SLOT(process()));
        m_thread.start();
    }
    QCoreApplication::processEvents(); // Somehow not helping
}

MapFetcher::MapFetcher(QObject *parent)
: QObject(*new MapFetcherPrivate, parent) {
    initTileKey();
}

MapFetcher::MapFetcher(MapFetcherPrivate &dd, QObject *parent)
:   QObject(dd, parent)
{
    initTileKey();
}

MapFetcherPrivate::MapFetcherPrivate()
: QObjectPrivate()
{
}

MapFetcherPrivate::~MapFetcherPrivate()
{
}

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

quint64 MapFetcherPrivate::requestSlippyTiles(const QGeoCoordinate &ctl, const QGeoCoordinate &cbr, const quint8 zoom, quint8 destinationZoom)
{
    Q_Q(MapFetcher);
    return NetworkManager::instance().requestSlippyTiles(*q, ctl, cbr, zoom, destinationZoom);
}

quint64 MapFetcherPrivate::requestCoverage(const QGeoCoordinate &ctl, const QGeoCoordinate &cbr, const quint8 zoom, bool clip)
{
    Q_Q(MapFetcher);
    return NetworkManager::instance().requestCoverage(*q, ctl, cbr, zoom, clip);
}

QString MapFetcherPrivate::objectName() const
{
    Q_Q(const MapFetcher);
    return q->objectName();
}

TileReplyHandler::TileReplyHandler(QNetworkReply *reply,
                                   MapFetcherWorker &mapFetcher)
    : m_reply(reply), m_mapFetcher(&mapFetcher)
{
    reply->setParent(this);
    connect(this,
            SIGNAL(insertTile(quint64,TileKey,std::shared_ptr<QImage>)),
            &mapFetcher,
            SLOT(onInsertTile(quint64,TileKey,std::shared_ptr<QImage>)), Qt::QueuedConnection);
    connect(this,
            SIGNAL(insertCoverage(quint64,std::shared_ptr<QImage>)),
            &mapFetcher,
            SLOT(onInsertCoverage(quint64,std::shared_ptr<QImage>)), Qt::QueuedConnection);
    connect(this, &TileReplyHandler::insertTile, this, &ThreadedJob::finished);
    connect(this, &TileReplyHandler::insertCoverage, this, &ThreadedJob::finished);
    connect(this, &TileReplyHandler::expectingMoreSubtiles, this, &ThreadedJob::finished);
    connect(this, &ThreadedJob::error, this, &ThreadedJob::finished);
}

ThreadedJob::ThreadedJob()
{
    connect(this, &ThreadedJob::finished, this, &QObject::deleteLater);
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
    connect(this, &ThreadedJob::finished, &t, &QThread::quit);
}

int ThreadedJob::priority() const
{
    return 10;
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

    if (z == dz) {
        k = TileKey{x,y,z};
        emit insertTile(id, k, std::make_shared<QImage>(QImage::fromData(std::move(data))));
    } else {
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
            QImage image = assembleTileFromSubtiles(subCache);
            emit insertTile(id, TileKey{dx,dy,dz}, std::make_shared<QImage>(std::move(image)));
            m_mapFetcher->d_func()->m_tileCacheCache[id].erase(dk);
        } else {
            emit expectingMoreSubtiles();
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
    ++m_active;
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
    d->m_heightmapCache[id].emplace(k, std::move(h));
    emit heightmapReady(id, k);
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

int DEMReadyHandler::priority() const
{
    return 8;
}

void DEMReadyHandler::process()
{    
    if (!m_demImage) {
        qWarning() << "NULL image in DEM Generation!";
        return;
    }
    std::shared_ptr<Heightmap> h =
            std::make_shared<Heightmap>(Heightmap::fromImage(*m_demImage, m_neighbors));

    if (m_coverage)
        emit insertHeightmapCoverage(m_requestId, h);
    else
        emit insertHeightmap(m_requestId, m_key, h);
}

DEMFetcherPrivate::DEMFetcherPrivate() : MapFetcherPrivate() {}

DEMFetcherPrivate::~DEMFetcherPrivate() {}

quint64 DEMFetcherPrivate::requestSlippyTiles(const QGeoCoordinate &ctl,
                                              const QGeoCoordinate &cbr,
                                              const quint8 zoom,
                                              quint8)
{
    Q_Q(DEMFetcher);
    return NetworkManager::instance().requestSlippyTiles(*q, ctl, cbr, zoom);
}

quint64 DEMFetcherPrivate::requestCoverage(const QGeoCoordinate &ctl, const QGeoCoordinate &cbr, const quint8 zoom, bool clip)
{
    Q_Q(DEMFetcher);
    return NetworkManager::instance().requestCoverage(*q, ctl, cbr, zoom, clip);
}

NetworkIOManager::NetworkIOManager() {}

void NetworkIOManager::requestSlippyTiles(MapFetcher *f,
                                          quint64 requestId,
                                          const QGeoCoordinate &ctl,
                                          const QGeoCoordinate &cbr,
                                          const quint8 zoom,
                                          quint8 destinationZoom)
{
    if (!ctl.isValid() || !cbr.isValid()) { // TODO: verify cbr is actually br of ctl
        qWarning() << "requestSlippyTiles: Invalid bounds";
        return;
    }

    MapFetcherWorker *w = getMapFetcherWorker(f);
    w->setURLTemplate(f->urlTemplate()); // it might change in between requests
    w->requestSlippyTiles(requestId, ctl, cbr, zoom, destinationZoom);
}

void NetworkIOManager::requestSlippyTiles(DEMFetcher *f,
                                          quint64 requestId,
                                          const QGeoCoordinate &ctl,
                                          const QGeoCoordinate &cbr,
                                          const quint8 zoom)
{
    if (!ctl.isValid() || !cbr.isValid()) { // TODO: verify cbr is actually br of ctl
        qWarning() << "requestSlippyTiles: Invalid bounds";
        return;
    }
    DEMFetcherWorker *w = getDEMFetcherWorker(f);
    w->setURLTemplate(f->urlTemplate()); // it might change in between requests
    w->requestSlippyTiles(requestId, ctl, cbr, zoom, zoom);
}

void NetworkIOManager::requestCoverage(MapFetcher *f, quint64 requestId, const QGeoCoordinate &ctl, const QGeoCoordinate &cbr, const quint8 zoom, const bool clip) {
    if (!ctl.isValid() || !cbr.isValid()) { // TODO: verify cbr is actually br of ctl
        qWarning() << "requestSlippyTiles: Invalid bounds";
        return;
    }
    MapFetcherWorker *w = getMapFetcherWorker(f);
    w->setURLTemplate(f->urlTemplate()); // it might change in between requests
    w->requestCoverage(requestId, ctl, cbr, zoom, clip);
}

void NetworkIOManager::requestCoverage(DEMFetcher *f, quint64 requestId, const QGeoCoordinate &ctl, const QGeoCoordinate &cbr, const quint8 zoom, const bool clip) {
    if (!ctl.isValid() || !cbr.isValid()) { // TODO: verify cbr is actually br of ctl
        qWarning() << "requestSlippyTiles: Invalid bounds";
        return;
    }
    DEMFetcherWorker *w = getDEMFetcherWorker(f);
    w->setURLTemplate(f->urlTemplate()); // it might change in between requests
    w->requestCoverage(requestId, ctl, cbr, zoom, clip);
}

void NetworkIOManager::requestSlippyTiles(ASTCFetcher *f,
                                          quint64 requestId,
                                          const QGeoCoordinate &ctl,
                                          const QGeoCoordinate &cbr,
                                          const quint8 zoom,
                                          quint8 destinationZoom)
{
    if (!ctl.isValid() || !cbr.isValid()) { // TODO: verify cbr is actually br of ctl
        qWarning() << "requestSlippyTiles: Invalid bounds";
        return;
    }
    ASTCFetcherWorker *w = getASTCFetcherWorker(f);
    w->setURLTemplate(f->urlTemplate()); // it might change in between requests
    w->requestSlippyTiles(requestId, ctl, cbr, zoom, destinationZoom);
}

void NetworkIOManager::requestCoverage(ASTCFetcher *f,
                                       quint64 requestId,
                                       const QGeoCoordinate &ctl,
                                       const QGeoCoordinate &cbr,
                                       const quint8 zoom,
                                       const bool clip)
{
    if (!ctl.isValid() || !cbr.isValid()) { // TODO: verify cbr is actually br of ctl
        qWarning() << "requestSlippyTiles: Invalid bounds";
        return;
    }
    ASTCFetcherWorker *w = getASTCFetcherWorker(f);
    w->setURLTemplate(f->urlTemplate()); // it might change in between requests
    w->requestCoverage(requestId, ctl, cbr, zoom, clip);
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
        w = new ASTCFetcherWorker(this, f, m_worker);
        m_astcFetcher2Worker.insert({f, w});
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
    d->m_tileCache[id].emplace(k, std::move(i)); // f->onInsertTile also emits
    auto t = d->peekTile(id, k); // redundant block
    if (!t) {
        qWarning() << "DEMFetcher::onTileReady: "<<k<< " not ready!";
        return;
    }
    if (!d->m_borders) {
        t.reset();
        auto h = new DEMReadyHandler(d->tile(id, k),
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
        auto neighborsComplete = [&tileNeighbors](const TileKey &k) {
            auto &nm = tileNeighbors[k];
            for (const auto n: neighbors) {
                if (nm.first.testFlag(n) && !nm.second[n])
                    return false;
            }
            return true;
        };
        auto existsNeighbor = [k,&tileNeighbors](Heightmap::Neighbor n) {
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
            auto h = new DEMReadyHandler(d->tile(id, tk),
                                         tk,
                                         *this,
                                         id,
                                         false,
                                         tileNeighbors_);
            d->m_worker->schedule(h);
        };

        if (neighborsComplete(k))
            handleNeighborsComplete(k);

        // Propagate tile into neighbors
        for (const auto n: neighbors) {
            if (existsNeighbor(n)) {
                TileKey nk = k + neighborOffsets.at(n);
                // TODO: add flag check?
                tileNeighbors[nk].second[neighborReciprocal.at(n)] = t;
                if (neighborsComplete(nk))
                    handleNeighborsComplete(nk);
            }
        }
    }
}

void DEMFetcherWorker::onCoverageReady(quint64 id,  std::shared_ptr<QImage> i)
{
    Q_D(DEMFetcherWorker);
    auto h = new DEMReadyHandler(i,
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
//    emit requestHandlingFinished(id);
}

MapFetcherWorker::MapFetcherWorker(QObject *parent, MapFetcher *f, QSharedPointer<ThreadedJobQueue> worker)
    : QObject(*new MapFetcherWorkerPrivate, parent) {
    Q_D(MapFetcherWorker);
    d->m_fetcher = f;
    d->m_worker = worker;
}

void MapFetcherWorker::requestSlippyTiles(quint64 requestId, const QGeoCoordinate &ctl, const QGeoCoordinate &cbr, const quint8 zoom, quint8 destinationZoom) {
    Q_D(MapFetcherWorker);
    if (!ctl.isValid() || !cbr.isValid()) { // TODO: verify cbr is actually br of ctl
        qWarning() << "requestSlippyTiles: Invalid bounds";
        return;
    }
    destinationZoom = std::min(zoom, destinationZoom); // TODO: implement the other way

    auto tiles = tilesFromBounds(ctl, cbr, destinationZoom); // Calculating using dz to avoid partial coverage
    for (const auto &t: tiles) {
        d->trackNeighbors(requestId,
                          {quint64(t.ts.x()), quint64(t.ts.y()), destinationZoom},
                          t.nb);
    }
    const quint64 srcTilesSize = tiles.size();
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
    d->m_request2remainingTiles.emplace(requestId, tiles.size());
    d->m_request2remainingHandlers.emplace(requestId, tiles.size());
    // TODO: replace with polymorphism
    auto *df = qobject_cast<DEMFetcherWorker *>(this);
    if (df) {
        df->d_func()->m_request2remainingDEMHandlers.emplace(requestId, srcTilesSize); // TODO: deduplicate? m_request2remainingHandlers might be enough
    }
    auto *af = qobject_cast<ASTCFetcherWorker *>(this);
    if (af) {
        af->d_func()->m_request2remainingASTCHandlers.emplace(requestId, srcTilesSize); // TODO: deduplicate? m_request2remainingHandlers might be enough
    }

    requestMapTiles(tiles,
                    urlTemplate,
                    destinationZoom,
                    requestId,
                    false,
                    d->m_nm,
                    this, SLOT(onTileReplyFinished()),
                    this, SLOT(networkReplyError(QNetworkReply::NetworkError)));
}

void MapFetcherWorker::requestCoverage(quint64 requestId, const QGeoCoordinate &ctl, const QGeoCoordinate &cbr, const quint8 zoom, const bool clip) {
    Q_D(MapFetcherWorker);
    if (!ctl.isValid() || !cbr.isValid()) // ToDo: check more toroughly
    {
        qWarning() << "requestCoverage: Invalid bounds";
        return;
    }

    const auto tiles = tilesFromBounds(ctl, cbr, zoom);
    const QString urlTemplate = (d->m_urlTemplate.isEmpty()) ? urlTemplateTerrariumS3 : d->m_urlTemplate;

    requestMapTiles(tiles,
                    urlTemplate,
                    zoom,
                    requestId,
                    true,
                    d->m_nm,
                    this, SLOT(onTileReplyForCoverageFinished()),
                    this, SLOT(networkReplyError(QNetworkReply::NetworkError)));

    d->m_requests.insert({requestId,
                          {ctl, cbr, zoom, tiles.size(), clip}});
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

    disconnect(reply, SIGNAL(finished()), this, SLOT(onTileReplyFinished()));
    disconnect(reply, SIGNAL(errorOccurred(QNetworkReply::NetworkError)),
               this, SLOT(networkReplyError(QNetworkReply::NetworkError)));

    const quint64 id = reply->property("ID").toUInt();
    if (d->m_request2remainingTiles.find(id) == d->m_request2remainingTiles.end()) {
        qWarning() << "No tracked request with id "<<id;
    } else {
        d->m_request2remainingTiles[id]--;
    }
    auto *df = qobject_cast<DEMFetcherWorker *>(this);
    auto *af = qobject_cast<ASTCFetcherWorker *>(this);
    if (reply->error() != QNetworkReply::NoError) {
        reply->deleteLater();
        d->m_request2remainingHandlers[id]--;
        if (df) {
            if (!--df->d_func()->m_request2remainingDEMHandlers[id]) {// TODO: deduplicate? m_request2remainingHandlers might be enough
                emit requestHandlingFinished(id);
            }
        } else if (af) {
            if (!--af->d_func()->m_request2remainingASTCHandlers[id]) {// TODO: deduplicate? m_request2remainingHandlers might be enough
                emit requestHandlingFinished(id);
            }
        } else {
            if (!d->m_request2remainingHandlers[id]) {
                emit requestHandlingFinished(id);
            }
        }
        return; // Already handled in networkReplyError
    }

    auto *handler = (df) ? new DEMTileReplyHandler(reply, *this)
                         : new TileReplyHandler(reply, *this);
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

    auto *handler = new TileReplyHandler(reply,
                                         *this);
    d->m_worker->schedule(handler);
}

void MapFetcherWorker::onInsertTile(const quint64 id, const TileKey k, std::shared_ptr<QImage> i) {
    Q_D(MapFetcherWorker);
    emit tileReady(id, k, i);
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
    reply->deleteLater();
}

MapFetcherWorker::MapFetcherWorker(MapFetcherWorkerPrivate &dd, MapFetcher *f, QSharedPointer<ThreadedJobQueue> worker, QObject *parent)
:   QObject(dd, parent)
{
    Q_D(MapFetcherWorker);
    d->m_fetcher = f;
    d->m_worker = worker;
}

DEMFetcherWorkerPrivate::DEMFetcherWorkerPrivate() : MapFetcherWorkerPrivate() {}

void DEMFetcherWorkerPrivate::trackNeighbors(quint64 id, const TileKey &k, Heightmap::Neighbors n)
{
    m_request2Neighbors[id][k] = {n, boundaryRasters};
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

bool ThreadedJobQueue::JobComparator::operator()(const ThreadedJob *l, const ThreadedJob *r) const {
    if (l && r)
        return r->priority() < l->priority();
    return false;

//    const DEMReadyHandler *lDEM = qobject_cast<const DEMReadyHandler *>(l);
//    const DEMReadyHandler *rDEM = qobject_cast<const DEMReadyHandler *>(r);
//    return (rDEM && lDEM && rDEM->tileKey() < lDEM->tileKey()) || (rDEM && !lDEM);
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

quint64 ASTCFetcherPrivate::requestSlippyTiles(const QGeoCoordinate &ctl, const QGeoCoordinate &cbr, const quint8 zoom, quint8 destinationZoom)
{
    Q_Q(ASTCFetcher);
    return NetworkManager::instance().requestSlippyTiles(*q, ctl, cbr, zoom, destinationZoom);
}

quint64 ASTCFetcherPrivate::requestCoverage(const QGeoCoordinate &ctl, const QGeoCoordinate &cbr, const quint8 zoom, bool clip)
{
    Q_Q(ASTCFetcher);
    return NetworkManager::instance().requestCoverage(*q, ctl, cbr, zoom, clip);
}


ASTCFetcherWorker::ASTCFetcherWorker(QObject *parent,
                                     ASTCFetcher *f,
                                     QSharedPointer<ThreadedJobQueue> worker)
:   MapFetcherWorker(*new ASTCFetcherWorkerPrivate, f, worker, parent)
{
    init();
}

void ASTCFetcherWorker::init()
{
    connect(this, &MapFetcherWorker::tileReady, this, &ASTCFetcherWorker::onTileReady);
    connect(this, &MapFetcherWorker::coverageReady, this, &ASTCFetcherWorker::onCoverageReady);
}

void ASTCFetcherWorker::onTileReady(quint64 id,
                                    const TileKey k,
                                    std::shared_ptr<QImage> i)
{
    Q_D(ASTCFetcherWorker);
    auto h = new Raster2ASTCHandler(i,
                                 k,
                                 *this,
                                 id,
                                 false);
    d->m_worker->schedule(h);
}

void ASTCFetcherWorker::onCoverageReady(quint64 id,
                                        std::shared_ptr<QImage> i)
{
    Q_D(ASTCFetcherWorker);
    auto h = new Raster2ASTCHandler(i,
                                 {0,0,0},
                                 *this,
                                 id,
                                 true);
    d->m_worker->schedule(h);
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

Raster2ASTCHandler::Raster2ASTCHandler(std::shared_ptr<QImage> tileImage,
                                       const TileKey k,
                                       ASTCFetcherWorker &fetcher,
                                       quint64 id,
                                       bool coverage)
    : m_fetcher(&fetcher)
    , m_rasterImage(std::move(tileImage))
    , m_requestId(id)
    , m_coverage(coverage)
    , m_key(k)
{
    connect(this, &Raster2ASTCHandler::insertTileASTC, m_fetcher, &ASTCFetcherWorker::onInsertTileASTC, Qt::QueuedConnection);
    connect(this, &Raster2ASTCHandler::insertCoverageASTC, m_fetcher, &ASTCFetcherWorker::onInsertCoverageASTC, Qt::QueuedConnection);
    connect(this, &Raster2ASTCHandler::insertCoverageASTC, this, &ThreadedJob::finished);
    connect(this, &Raster2ASTCHandler::insertTileASTC, this, &ThreadedJob::finished);
}

int Raster2ASTCHandler::priority() const
{
    return 9;
}

void Raster2ASTCHandler::process()
{
    if (!m_rasterImage) {
        qWarning() << "NULL image in Compressed Texture Generation!";
        return;
    }
    std::shared_ptr<CompressedTextureData> t =
            std::static_pointer_cast<CompressedTextureData>(
                ASTCCompressedTextureData::fromImage(m_rasterImage));

    if (m_coverage)
        emit insertCoverageASTC(m_requestId, t);
    else
        emit insertTileASTC(m_requestId, m_key, t);
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

void ASTCFetcher::onInsertASTCTile(const quint64 id,
                                   const TileKey k,
                                   std::shared_ptr<CompressedTextureData> i)
{
    Q_D(ASTCFetcher);
    d->m_tileCacheASTC[id].emplace(k, std::move(i));
    emit tileReady(id, k);
}

void ASTCFetcher::onInsertASTCCoverage(const quint64 id,
                                       std::shared_ptr<CompressedTextureData> i)
{
    Q_D(ASTCFetcher);
    d->m_coveragesASTC.emplace(id, std::move(i));
    emit coverageReady(id);
}

#if 0
quint64 ASTCCompressedTextureData::upload(QSharedPointer<QOpenGLTexture> &t)
{

}
#else
quint64 ASTCCompressedTextureData::upload(QSharedPointer<QOpenGLTexture> &t)
{
    if (!NetworkConfiguration::astcEnabled || !m_mips.size()) {
        if (!m_image)
            return 0;
        t.reset(new QOpenGLTexture(QOpenGLTexture::Target2D));
        t->setMaximumAnisotropy(16);
        t->setMinMagFilters(QOpenGLTexture::LinearMipMapLinear,
                                   QOpenGLTexture::Linear);
        t->setWrapMode(QOpenGLTexture::ClampToEdge);
        t->setData(*m_image);
        return m_image->size().width() * m_image->size().height() * 4; // rgba8
    } else {
        if (!m_mips.size())
            return 0;
        const int maxLod = m_mips.size() - 1;

        t.reset(new QOpenGLTexture(QOpenGLTexture::Target2D));
        t->setAutoMipMapGenerationEnabled(false);
        t->setMaximumAnisotropy(16);
        t->setMipMaxLevel(maxLod);
        t->setMinMagFilters(QOpenGLTexture::LinearMipMapLinear,
                                   QOpenGLTexture::Linear);
        t->setWrapMode(QOpenGLTexture::ClampToEdge);


        t->setFormat(QOpenGLTexture::TextureFormat(m_mips.at(0).glInternalFormat()));
        t->setSize(m_mips.at(0).size().width(), m_mips.at(0).size().height());
        t->setMipLevels(m_mips.size());
        t->allocateStorage();

        QOpenGLPixelTransferOptions uploadOptions;
        uploadOptions.setAlignment(1);

        quint64 sz{0};
        for (int i  = 0; i <= maxLod; ++i) {
            t->setCompressedData(i,
                                 m_mips.at(i).dataLength(),
                                 m_mips.at(i).data().constData() + m_mips.at(i).dataOffset(),
                                 &uploadOptions);
            sz += m_mips.at(i).dataLength();
        }

        return sz; // astc
    }
}
#endif
QSize ASTCCompressedTextureData::size() const
{
    return QSize();
}

std::shared_ptr<ASTCCompressedTextureData>  ASTCCompressedTextureData::fromImage(
        const std::shared_ptr<QImage> &i)
{
    std::shared_ptr<ASTCCompressedTextureData> res = std::make_shared<ASTCCompressedTextureData>();
    res->m_image = std::make_shared<QImage>(i->mirrored(false, true));

    if (!NetworkConfiguration::astcEnabled)
        return res;

    QSize size(i->width(), i->height());
    if (!isEven(size)) {
        qWarning() << "Warning: cannot generate mips for size"<<size;
    }

    ASTCEncoder::instance().generateMips(*res->m_image, res->m_mips);

    return res;
}

DEMTileReplyHandler::DEMTileReplyHandler(QNetworkReply *reply, MapFetcherWorker &mapFetcher)
:   TileReplyHandler(reply, mapFetcher) {}

int DEMTileReplyHandler::priority() const
{
    return 7;
}
