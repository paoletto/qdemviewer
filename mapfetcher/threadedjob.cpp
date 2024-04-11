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
#include <QImage>
#include <QBuffer>
#include <QByteArray>

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
} // namespace

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


        astcenc_error status = astcenc_compress_image(m_ctx.get(),
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

    void generateMips(const QImage &ima, std::vector<QTextureFileData> &out, QByteArray md5) {
        if (!md5.size()) {
            QCryptographicHash ch(QCryptographicHash::Md5);
            ch.addData(reinterpret_cast<const char *>(ima.constBits()), ima.sizeInBytes());
            md5 = ch.result();
        }
        QSize size = ima.size();
        QByteArray cached = m_tileCache.tile(md5,
                                               block_x,
                                               block_y,
                                               quality,
                                               size.width(),
                                               size.height());
        QImage halved;
        if (cached.size()) {
            out.push_back(fromCached(cached));

            while (isEven(size)) {
                size = QSize(size.width() / 2, size.height() / 2);
                if (size.width() < ASTCEncoder::blockSize())
                    break;
                cached = m_tileCache.tile(md5,
                                           block_x,
                                           block_y,
                                           quality,
                                           size.width(),
                                           size.height());
                if (!cached.size()) { // generateMips was probably aborted during operation. Recover the missing mips
                    halved = ima;
                    QByteArray next;
                    while (isEven(halved.size()) && halved.size().width() > size.width()) {
                        halved = ASTCEncoder::halve(halved);
                    }
                    while (isEven(halved.size())) {
                        if (halved.size().width() < ASTCEncoder::blockSize())
                            break;
                        QByteArray compressed = ASTCEncoder::instance().compress(halved).data();
                        if (!next.size())
                            next = compressed;
                        m_tileCache.insert(md5,
                                           block_x,
                                           block_y,
                                           quality,
                                           halved.size().width(),
                                           halved.size().height(),
                                           compressed);
                        halved = ASTCEncoder::halve(halved);
                    }
                    out.push_back(fromCached(next));
                } else {
                    out.push_back(fromCached(cached));
                }
            }
        } else {
            out.emplace_back(ASTCEncoder::instance().compress(ima));
            m_tileCache.insert(md5,
                               block_x,
                               block_y,
                               quality,
                               size.width(),
                               size.height(),
                               out.back().data());
            halved = ima;
            while (isEven(size)) {
                size = QSize(size.width() / 2, size.height() / 2);
                if (size.width() < ASTCEncoder::blockSize())
                    break;
                halved = ASTCEncoder::halve(halved);
                out.emplace_back(ASTCEncoder::instance().compress(halved));
                m_tileCache.insert(md5,
                                   block_x,
                                   block_y,
                                   quality,
                                   size.width(),
                                   size.height(),
                                   out.back().data());
            }
        }
    }

    bool isCached(const QByteArray &md5) {
        return m_tileCache.contains(md5, block_x, block_y, quality);
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

        astcenc_context *c;
        status = astcenc_context_alloc(&config, thread_count, &c);
        m_ctx.reset(c);
        if (status != ASTCENC_SUCCESS) {
            qWarning() << "ERROR: Codec context alloc failed: "<< astcenc_get_error_string(status);
            qFatal("Terminating");
        }
    }

    ~ASTCEncoder() {}

    struct astcenc_context_deleter {
        static inline void cleanup(astcenc_context *c)
        {
            if (c)
                astcenc_context_free(c);
            c = nullptr;
        }
    };

    QScopedPointer<astcenc_context, astcenc_context_deleter> m_ctx;
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

bool ThreadedJobQueue::JobComparator::operator()(const ThreadedJobData *l, const ThreadedJobData *r) const {
    if (l && r)
        return r->priority() < l->priority();
    return false;

//    const DEMReadyHandler *lDEM = qobject_cast<const DEMReadyHandler *>(l);
//    const DEMReadyHandler *rDEM = qobject_cast<const DEMReadyHandler *>(r);
//    return (rDEM && lDEM && rDEM->tileKey() < lDEM->tileKey()) || (rDEM && !lDEM);
}

ThreadedJobQueue::ThreadedJobQueue(QObject *parent): QObject(parent) {
//    connect(&m_thread, &QThread::finished,
//            this, &ThreadedJobQueue::next, Qt::QueuedConnection);
    m_thread.setObjectName("ThreadedJobQueue Thread");
}

ThreadedJobQueue::~ThreadedJobQueue() {

}

void ThreadedJobQueue::schedule(ThreadedJobData *handler) {
    if (!handler) {
        qWarning() << "ThreadedJobQueue::schedule: null handler!";
        return;
    }
    m_jobs.push(handler);

    if (!m_currentJob) {
        next();
    }
}

void ThreadedJobQueue::next() {
//    if (m_thread.isRunning())
//        return;
    if (m_jobs.empty()) {
        m_currentJob = nullptr;
        return;
    }
    auto jobData = m_jobs.top();
    auto job = ThreadedJob::fromData(jobData);
    if (!job) {// impossible?
        qWarning() << "ThreadedJobQueue::next : null next job!";
        next();
    }  else  {
        job->move2thread(m_thread);
        connect(job, &ThreadedJob::destroyed,
                this, &ThreadedJobQueue::next, Qt::QueuedConnection);
        m_currentJob = job;
        m_jobs.pop();

        if (!m_thread.isRunning())
            m_thread.start();

        QMetaObject::invokeMethod(m_currentJob,
                                  "process",
                                  Qt::QueuedConnection);
    }
    QCoreApplication::processEvents(); // Somehow not helping
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
//    connect(this, &ThreadedJob::finished, &t, &QThread::quit);
}

TileReplyHandler::TileReplyHandler(QNetworkReply *reply,
                                   MapFetcherWorker &mapFetcher)
    : m_reply(reply), m_mapFetcher(&mapFetcher)
{
    reply->setParent(this);
//    connect(this,
//            SIGNAL(insertTile(quint64,TileKey,std::shared_ptr<QImage>)),
//            &mapFetcher,
//            SLOT(onInsertTile(quint64,TileKey,std::shared_ptr<QImage>)), Qt::QueuedConnection);
//    connect(this,
//            SIGNAL(insertCoverage(quint64,std::shared_ptr<QImage>)),
//            &mapFetcher,
//            SLOT(onInsertCoverage(quint64,std::shared_ptr<QImage>)), Qt::QueuedConnection);
    connect(this, &TileReplyHandler::insertTile,
            &mapFetcher, &MapFetcherWorker::onInsertTile, Qt::QueuedConnection);
    connect(this, &TileReplyHandler::insertCoverage,
            &mapFetcher, &MapFetcherWorker::onInsertCoverage, Qt::QueuedConnection);
    connect(this, &TileReplyHandler::insertTile, this, &ThreadedJob::finished);
    connect(this, &TileReplyHandler::insertCoverage, this, &ThreadedJob::finished);
    connect(this, &TileReplyHandler::expectingMoreSubtiles, this, &ThreadedJob::finished);
    connect(this, &ThreadedJob::error, this, &ThreadedJob::finished);
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
        auto tile = std::make_shared<QImage>(QImage::fromData(std::move(data)).mirrored(false, m_computeHash));
        if (m_computeHash)
            md5 = md5QImage(*tile);
        emit insertTile(id,
                        k,
                        std::move(tile),
                        std::move(md5));
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
            QImage image = assembleTileFromSubtiles(subCache).mirrored(false, m_computeHash);
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
//        k = TileKey{x,y,z};
        auto tile = QImage::fromData(std::move(data)).mirrored(false, m_computeHash);
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

    emit insertCoverage(id, std::make_shared<QImage>(res.mirrored(false, m_computeHash)));
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
    connect(this, &ThreadedJob::error, this, &ThreadedJob::finished);
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
    std::shared_ptr<Heightmap> h =
            std::make_shared<Heightmap>(Heightmap::fromImage(*m_demImage, m_neighbors));

    if (m_coverage)
        emit insertHeightmapCoverage(m_requestId, h);
    else
        emit insertHeightmap(m_requestId, m_key, h);
}


Raster2ASTCHandler::Raster2ASTCHandler(std::shared_ptr<QImage> rasterImage,
                                       const TileKey k,
                                       ASTCFetcherWorker &fetcher,
                                       quint64 id,
                                       bool coverage,
                                       QByteArray md5)
    : m_fetcher(&fetcher)
    , m_rasterImage(std::move(rasterImage))
    , m_requestId(id)
    , m_coverage(coverage)
    , m_key(k)
    , m_md5(md5)
    , m_forwardUncompressed(fetcher.forwardUncompressed())
{
    connect(this, &Raster2ASTCHandler::insertTileASTC, m_fetcher, &ASTCFetcherWorker::onInsertTileASTC, Qt::QueuedConnection);
    connect(this, &Raster2ASTCHandler::insertCoverageASTC, m_fetcher, &ASTCFetcherWorker::onInsertCoverageASTC, Qt::QueuedConnection);
    connect(this, &Raster2ASTCHandler::insertCoverageASTC, this, &ThreadedJob::finished);
    connect(this, &Raster2ASTCHandler::insertTileASTC, this, &ThreadedJob::finished);
}

void Raster2ASTCHandler::process()
{
    if (!m_rasterImage) {
        qWarning() << "NULL image in Compressed Texture Generation!";
        return;
    }
    // Do this in ASTCFetcher::onInsertTile instead, to prevent ready compound tiles from being shown
    // due to encoding tasks blocking the job queue
//    if (m_forwardUncompressed && !ASTCEncoder::instance().isCached(m_md5)) {
//        std::shared_ptr<ASTCCompressedTextureData> uncompressed =
//                std::make_shared<ASTCCompressedTextureData>();
//        uncompressed->m_image = m_rasterImage;
//        auto t = std::static_pointer_cast<CompressedTextureData>(uncompressed);
//        if (m_coverage)
//            emit insertCoverageASTC(m_requestId, t);
//        else
//            emit insertTileASTC(m_requestId, m_key, t);
//        QCoreApplication::processEvents();
//    }

    std::shared_ptr<CompressedTextureData> t =
            std::static_pointer_cast<CompressedTextureData>(
                ASTCCompressedTextureData::fromImage(m_rasterImage, m_md5));

    if (m_coverage)
        emit insertCoverageASTC(m_requestId, t);
    else
        emit insertTileASTC(m_requestId, m_key, t);
}

DEMTileReplyHandler::DEMTileReplyHandler(QNetworkReply *reply, MapFetcherWorker &mapFetcher)
:   TileReplyHandler(reply, mapFetcher) {
    m_computeHash = false;
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

// TODO: move this code out of the library and into the application
quint64 ASTCCompressedTextureData::uploadTo2DArray(QSharedPointer<QOpenGLTexture> &texArray,
                                                   int layer,
                                                   int layers)
{
    initStatics();
    if (!NetworkConfiguration::astcEnabled) { // || !m_mips.size()) {
        if (!m_image)
            return 0;
        QOpenGLPixelTransferOptions uploadOptions;
        uploadOptions.setAlignment(1);
        auto &t = texArray;
        if (!t
            || t->width() != m_image->size().width()
            || t->height() != m_image->size().height()
            || t->layers() != layers
            || isFormatCompressed(t->format()))
        {
            t.reset(new QOpenGLTexture(QOpenGLTexture::Target2DArray));
            t->setLayers(layers);
            t->setMaximumAnisotropy(16);
            t->setMinMagFilters(QOpenGLTexture::LinearMipMapLinear,
                                       QOpenGLTexture::Linear);
            t->setWrapMode(QOpenGLTexture::ClampToEdge);
            t->setAutoMipMapGenerationEnabled(true);
            t->setFormat(QOpenGLTexture::RGBA8_UNorm);
            t->setSize(m_image->size().width(), m_image->size().height());
            t->allocateStorage();

            for (int i = 0; i < layers; ++i) {
                t->setData(0,
                           i,
                           QOpenGLTexture::RGBA,
                           QOpenGLTexture::UInt8,
                           m_white256.constBits(),
                           &uploadOptions);
            }
        }

        QImage glImage = m_image->convertToFormat(QImage::Format_RGBA8888);
        t->setData(0,
                   layer,
                   QOpenGLTexture::RGBA,
                   QOpenGLTexture::UInt8,
                   glImage.constBits(),
                   &uploadOptions);
        return glImage.sizeInBytes(); // TODO: this is incorrect, lacks mips size. do we care?
    } else {
        const int maxLod = m_mips.size() - 1;
        auto &t = texArray;
        QOpenGLPixelTransferOptions uploadOptions;
        uploadOptions.setAlignment(1);
        if (!t
            || t->width() != m_mips.front().size().width()
            || t->height() != m_mips.front().size().height()
            || t->layers() != layers
            || !isFormatCompressed(t->format())) {
            t.reset(new QOpenGLTexture(QOpenGLTexture::Target2DArray));
            t->setLayers(layers);
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
            // initialize everything with white
            for (int i = 0; i < layers; ++i) {
                for (int mip = 0; mip < m_white8x8ASTC.size(); ++ mip) {
                    t->setCompressedData(mip,
                               i,
                               m_white8x8ASTC.at(mip).dataLength(),
                               m_white8x8ASTC.at(mip).data().constData()
                                         + m_white8x8ASTC.at(mip).dataOffset(),
                               &uploadOptions);
                }
            }
        }

        quint64 sz{0};
        for (int i  = 0; i <= maxLod; ++i) {
            t->setCompressedData(i,
                                 layer,
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

std::shared_ptr<ASTCCompressedTextureData>  ASTCCompressedTextureData::fromImage(
        const std::shared_ptr<QImage> &i,
        QByteArray md5)
{
    std::shared_ptr<ASTCCompressedTextureData> res = std::make_shared<ASTCCompressedTextureData>();
    res->m_image = i;

    if (!NetworkConfiguration::astcEnabled)
        return res;

    QSize size(i->width(), i->height());
    if (!isEven(size)) {
        qWarning() << "Warning: cannot generate mips for size"<<size;
    }

    ASTCEncoder::instance().generateMips(*res->m_image, res->m_mips, std::move(md5));

    return res;
}

ThreadedJob *ThreadedJob::fromData(ThreadedJobData *data)
{
    struct ScopeExit {
            ScopeExit(ThreadedJobData &data) : d{ &data } {}
            ~ScopeExit() { delete d; }
            ThreadedJobData *d;
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
        return new Raster2ASTCHandler(std::move(d->m_rasterImage),
                                      d->m_k,
                                      d->m_fetcher,
                                      d->m_id,
                                      d->m_coverage,
                                      std::move(d->m_md5));
    }
    default:
        return nullptr;
    }
}
