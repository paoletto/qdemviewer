/****************************************************************************
**
** Copyright (C) 2024- Paolo Angelelli <paoletto@gmail.com>
**
** This file is part of qdemviewer.
**
** This work is licensed under the terms of the Creative Commons
** Attribution-NonCommercial-ShareAlike 4.0 International License,
** with additional clauses. See the LICENSE file in the project root
** for full terms.
**
****************************************************************************/

#include "astcencoder.h"
#include "astccache.h"

#include <iostream>
#include <string>
#include <cstdlib>
#include <vector>
#include <map>
#include <QThreadPool>
#include <QThread>

#include <QStandardPaths>
#include <QDirIterator>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QScopedPointer>
#include <QBuffer>

#include <private/qtexturefilereader_p.h>


#include "astcenc.h"

struct ASTCEncoderPrivate {
    ASTCEncoderPrivate()
    : m_cacheDirPath(QStringLiteral("%1/astcCache.sqlite").arg(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)))
    , m_tileCache(m_cacheDirPath)
    {
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
};

namespace {
bool isEven(const QSize &s) {
    return (s.width() % 2) == 0 && (s.height() % 2) == 0;
}
} // namespace

struct astc_header
{
    uint8_t magic[4];
    uint8_t block_x;
    uint8_t block_y;
    uint8_t block_z;
    uint8_t dim_x[3];
    uint8_t dim_y[3];
    uint8_t dim_z[3];
};

ASTCEncoder &ASTCEncoder::instance()
{
    thread_local ASTCEncoder instance;
    return instance;
}

QTextureFileData ASTCEncoder::compress(QImage ima) {
    if (ima.hasAlphaChannel())
       ima = ima.convertToFormat(QImage::Format_ARGB32);
    else
        ima = ima.convertToFormat(QImage::Format_RGB32);
    unsigned int block_count_x = (ima.width() + d->block_x - 1) / d->block_x;
    unsigned int block_count_y = (ima.height() + d->block_y - 1) / d->block_y;

    astcenc_image image;
    image.dim_x = ima.width();
    image.dim_y = ima.height();
    image.dim_z = 1;
    image.data_type = ASTCENC_TYPE_U8;
    uint8_t* slices = ima.bits();
    image.data = reinterpret_cast<void**>(&slices);

    size_t comp_len = block_count_x * block_count_y * 16;

    QByteArray data;
    data.resize(comp_len);


    astcenc_error status = astcenc_compress_image(d->m_ctx.get(),
                                                  &image,
                                                  &d->swizzle,
                                                  reinterpret_cast<uint8_t *>(data.data()),
                                                  comp_len,
                                                  0);
    if (status != ASTCENC_SUCCESS || !data.size()) {
        qWarning() << "ERROR: Codec compress failed: "
                        << astcenc_get_error_string(status)
                        <<  " " << data.size() << " " << ima.size();
        qFatal("Terminating");
    }

    astc_header hdr;
    hdr.magic[0] =  d->ASTC_MAGIC_ID        & 0xFF;
    hdr.magic[1] = (d->ASTC_MAGIC_ID >>  8) & 0xFF;
    hdr.magic[2] = (d->ASTC_MAGIC_ID >> 16) & 0xFF;
    hdr.magic[3] = (d->ASTC_MAGIC_ID >> 24) & 0xFF;

    hdr.block_x = static_cast<uint8_t>(d->block_x);
    hdr.block_y = static_cast<uint8_t>(d->block_y);
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

QImage ASTCEncoder::halve(const QImage &src) {
    if ((src.width() % 2) != 0  || (src.height() % 2) != 0) {
        qWarning() << "Requested halving of size "<< QSize(src.width(), src.height()) <<" not supported";
        return src;
    }

    const QSize size = QSize(src.width() / 2, src.height() / 2);
    const float hMultiplier = src.width() / float(size.width());
    const float vMultiplier = src.height() / float(size.height());
    QImage res(size, QImage::Format_RGB32);

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

int ASTCEncoder::blockSize() {
    return ASTCEncoderPrivate::block_x;
}

QTextureFileData ASTCEncoder::fromCached(QByteArray &cached) {
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

void ASTCEncoder::generateMips(QImage ima, std::vector<QImage> &out)
{
    QSize size = ima.size();
    QImage halved = (ima.hasAlphaChannel())
                        ? ima.convertToFormat(QImage::Format_ARGB32)
                        : ima.convertToFormat(QImage::Format_RGB32);
    out.emplace_back(halved.convertToFormat(QImage::Format_RGBA8888));

    while (isEven(size)) {
        size = QSize(size.width() / 2, size.height() / 2);
        if (size.width() < ASTCEncoder::blockSize())
            break;
        halved = ASTCEncoder::halve(halved);
        out.emplace_back(halved.convertToFormat(QImage::Format_RGBA8888));
    }
}

void ASTCEncoder::generateMips(const QImage &ima,
                               quint64 x,
                               quint64 y,
                               quint64 z,
                               std::vector<QTextureFileData> &out,
                               QByteArray md5) {
    if (!md5.size()) {
        QCryptographicHash ch(QCryptographicHash::Md5);
        ch.addData(reinterpret_cast<const char *>(ima.constBits()), ima.sizeInBytes());
        md5 = ch.result();
    }
    QSize size = ima.size();
    QImage halved;

    QByteArray cached = d->m_tileCache.tile(md5,
                                         d->block_x,
                                         d->block_y,
                                         d->quality,
                                         size.width(),
                                         size.height());

    if (cached.size()) {
        out.push_back(fromCached(cached));

        while (isEven(size)) {
            size = QSize(size.width() / 2, size.height() / 2);
            if (size.width() < ASTCEncoder::blockSize())
                break;
            cached = d->m_tileCache.tile(md5,
                                      d->block_x,
                                      d->block_y,
                                      d->quality,
                                      size.width(),
                                      size.height());
            if (!cached.size()) {
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
                    d->m_tileCache.insert(md5,
                                       d->block_x,
                                       d->block_y,
                                       d->quality,
                                       halved.size().width(),
                                       halved.size().height(),
                                       x,y,z,
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
        d->m_tileCache.insert(md5,
                           d->block_x,
                           d->block_y,
                           d->quality,
                           size.width(),
                           size.height(),
                           x,y,z,
                           out.back().data());
        halved = ima;
        while (isEven(size)) {
            size = QSize(size.width() / 2, size.height() / 2);
            if (size.width() < ASTCEncoder::blockSize())
                break;
            halved = ASTCEncoder::halve(halved);
            out.emplace_back(ASTCEncoder::instance().compress(halved));
            d->m_tileCache.insert(md5,
                               d->block_x,
                               d->block_y,
                               d->quality,
                               size.width(),
                               size.height(),
                               x,y,z,
                               out.back().data());
        }
    }
}

bool ASTCEncoder::isCached(const QByteArray &md5) {
    return d->m_tileCache.contains(md5, d->block_x, d->block_y, d->quality);
}

ASTCEncoder::ASTCEncoder(): d(new ASTCEncoderPrivate){}

ASTCEncoder::~ASTCEncoder() {}

