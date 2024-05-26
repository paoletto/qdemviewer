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

#include "utils_p.h"
#include "astcencoder.h"
#include <QOpenGLTexture>
#include <QOpenGLPixelTransferOptions>
#include <QOpenGLContext>
//#include <QOpenGLFunctions>
//#include <QOpenGLExtraFunctions>
//#include <QOpenGLFunctions_4_5_Core>
#include <private/qtexturefilereader_p.h>

std::vector<QImage> OpenGLTextureUtils::m_white256;
std::vector<QTextureFileData> OpenGLTextureUtils::m_white8x8ASTC;
std::vector<QTextureFileData> OpenGLTextureUtils::m_transparent8x8ASTC;

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

void OpenGLTextureUtils::init() {
    if (m_white256.size())
        return;

    Q_INIT_RESOURCE(qmake_mapfetcher_res);
    m_white256.resize(1);
    m_white256[0].load(":/white256.png");
    loadASTCMips("white", m_white8x8ASTC);
    loadASTCMips("transparent", m_transparent8x8ASTC);
    QImage i256 = std::move(m_white256[0]);
    m_white256.clear();
    ASTCEncoder::generateMips(i256, m_white256);
}

quint64 OpenGLTextureUtils::fillSingleTextureUncompressed(QSharedPointer<QOpenGLTexture> &t,
                                                          std::shared_ptr<QImage> &ima)
{
    if (!ima)
        return 0;
    t.reset(new QOpenGLTexture(QOpenGLTexture::Target2D));
    t->setMaximumAnisotropy(16);
    t->setAutoMipMapGenerationEnabled(true);
    t->setMinMagFilters(QOpenGLTexture::LinearMipMapLinear,
                               QOpenGLTexture::Linear);
    t->setWrapMode(QOpenGLTexture::ClampToEdge);
    t->setData(*ima);
    return ima->sizeInBytes() * 1.333;
}

// TODO: Rename/remove. terrarium conversion must not happen here.
quint64 OpenGLTextureUtils::fillSingleTextureBPTC(QSharedPointer<QOpenGLTexture> &t,
                                                  std::shared_ptr<QImage> &ima)
{
    if (!ima)
        return 0;

    // Upload pixel data and dont generate mipmaps
    QImage glImage = ima->convertToFormat(QImage::Format_RGBA8888);
    std::vector<float> data;
    data.reserve(ima->width() * ima->height());
    for (int y = 0; y < ima->height(); ++y) {
        for (int x = 0; x < ima->width(); ++x) {
            auto rgb = ima->pixel(x,y);
            float decodedMeters = (qRed(rgb) * 256.
                                   + qGreen(rgb)
                                   + qBlue(rgb) * 0.00390625) - 32768.;
            data.push_back(decodedMeters);
        }
    }
    return fillSingleTextureBPTC(t, {ima->width(), ima->height()}, data);
}

quint64 OpenGLTextureUtils::fillSingleTextureBPTC(QSharedPointer<QOpenGLTexture> &t,
                                                  QSize texSize,
                                                  std::vector<float> &data,
                                                  float min)
{
    std::vector<float> data2 = data;
    for (auto &f: data2)
        f = float(int(f-min)); // even doing this doesn't help much..
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (!context) {
        qWarning("fillSingleTextureBPTC requires a valid current context");
        return 0;
    }
    t.reset(new QOpenGLTexture(QOpenGLTexture::Target2D));
    t->setMaximumAnisotropy(16);
    t->setAutoMipMapGenerationEnabled(false);
    t->setMinMagFilters(QOpenGLTexture::Nearest,
                        QOpenGLTexture::Nearest);
    t->setFormat(QOpenGLTexture::RGB_BP_SIGNED_FLOAT);

    t->setSize(texSize.width(), texSize.height());
    t->setMipLevels(1);
    t->allocateStorage(QOpenGLTexture::RGB, QOpenGLTexture::UInt8);
    t->setData(QOpenGLTexture::Red,
               QOpenGLTexture::Float32,
               (const void *) &data2.front());

    return data.size(); // BPTC uses 128bit per 4x4 block.
}

quint64 OpenGLTextureUtils::fillSingleTextureUncompressed(QSharedPointer<QOpenGLTexture> &t,
                                                         const QSize &size,
                                                         std::vector<float> &data)
{
    if (!t
        || QSize(t->width(), t->height()) != size
        || t->target() != QOpenGLTexture::Target2D
        || t->format() != QOpenGLTexture::R32F)
    {
        t.reset(new QOpenGLTexture(QOpenGLTexture::Target2D));
        t->setAutoMipMapGenerationEnabled(false);
        t->setFormat(QOpenGLTexture::R32F);
        t->setSize(size.width(), size.height());
        t->allocateStorage(QOpenGLTexture::Red, QOpenGLTexture::Float32);
    }
    t->setData(QOpenGLTexture::Red,
               QOpenGLTexture::Float32,
               (const void *) &data.front());
    return data.size() * sizeof(float);
}

quint64 OpenGLTextureUtils::fill2DArrayUncompressed(QSharedPointer<QOpenGLTexture> &t,
                                                    std::shared_ptr<QImage> &ima,
                                                    int layer,
                                                    int layers)
{
    if (!ima)
        return 0;
    OpenGLTextureUtils::init();
    QOpenGLPixelTransferOptions uploadOptions;
    uploadOptions.setAlignment(1);
    if (!t
        || t->width() != ima->size().width()
        || t->height() != ima->size().height()
        || t->layers() != layers
        || isFormatCompressed(t->format()))
    {
        t.reset(new QOpenGLTexture(QOpenGLTexture::Target2DArray));
        t->setLayers(layers);
        t->setMaximumAnisotropy(16);
        t->setMinMagFilters(QOpenGLTexture::LinearMipMapLinear,
                                   QOpenGLTexture::Linear);
        t->setWrapMode(QOpenGLTexture::ClampToEdge);
        // generate the mips here only one at a time,
        // don't make the driver produce all of them when
        // initializing the array with white
        t->setAutoMipMapGenerationEnabled(false);
        t->setFormat(QOpenGLTexture::RGBA8_UNorm);
        t->setSize(ima->size().width(), ima->size().height());

        t->setMipLevels(m_white256.size());
        t->allocateStorage();

        for (int l = 0; l < layers; ++l) {
            for (int m = 0; m < m_white256.size(); ++m) {
                t->setData(m,
                           l,
                           QOpenGLTexture::RGBA,
                           QOpenGLTexture::UInt8,
                           m_white256[m].constBits(),
                           &uploadOptions);
            }
        }
    }

    QImage glImage = ima->convertToFormat(QImage::Format_RGBA8888);
    std::vector<QImage> mips;
    ASTCEncoder::generateMips(glImage, mips);
    quint64 sz{0};
    for (size_t m = 0; m < mips.size(); ++m) {
        t->setData(m,
                   layer,
                   QOpenGLTexture::RGBA,
                   QOpenGLTexture::UInt8,
                   mips[m].constBits(),
                   &uploadOptions);
        sz += mips[m].sizeInBytes();
    }

    return sz;
}

quint64 OpenGLTextureUtils::fill2DArrayASTC(QSharedPointer<QOpenGLTexture> &t,
                                            std::vector<QTextureFileData> mips,
                                            int layer,
                                            int layers)
{
    if (!mips.size())
        return 0;
    OpenGLTextureUtils::init();
    const int maxLod = mips.size() - 1;
    QOpenGLPixelTransferOptions uploadOptions;
    uploadOptions.setAlignment(1);
    if (!t
        || t->width() != mips.front().size().width()
        || t->height() != mips.front().size().height()
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
        t->setFormat(QOpenGLTexture::TextureFormat(mips.at(0).glInternalFormat()));
        t->setSize(mips.at(0).size().width(), mips.at(0).size().height());
        t->setMipLevels(mips.size());

        t->allocateStorage();
#if 1 // initialize everything with white
        for (int i = 0; i < layers; ++i) {
            for (int mip = 0; mip < m_white8x8ASTC.size(); ++mip) {
                t->setCompressedData(mip,
                           i,
                           m_white8x8ASTC.at(mip).dataLength(),
                           m_white8x8ASTC.at(mip).data().constData()
                                     + m_white8x8ASTC.at(mip).dataOffset(),
                           &uploadOptions);
            }
        }
#else // initialize everything with transparent
        for (int i = 0; i < layers; ++i) {
            for (int mip = 0; mip < m_transparent8x8ASTC.size(); ++mip) {
                t->setCompressedData(mip,
                           i,
                           m_transparent8x8ASTC.at(mip).dataLength(),
                           m_transparent8x8ASTC.at(mip).data().constData()
                                     + m_transparent8x8ASTC.at(mip).dataOffset(),
                           &uploadOptions);
            }
        }
#endif
    }

    quint64 sz{0};
    for (int i  = 0; i <= maxLod; ++i) {
        t->setCompressedData(i,
                             layer,
                             mips.at(i).dataLength(),
                             mips.at(i).data().constData() + mips.at(i).dataOffset(),
                             &uploadOptions);
        sz += mips.at(i).dataLength();
    }

    return sz; // astc
}

quint64 OpenGLTextureUtils::fillSingleTextureASTC(QSharedPointer<QOpenGLTexture> &t,
                                                  std::vector<QTextureFileData> &mips)
{
    if (!mips.size())
        return 0;
    const int maxLod = mips.size() - 1;

    t.reset(new QOpenGLTexture(QOpenGLTexture::Target2D));
    t->setAutoMipMapGenerationEnabled(false);
    t->setMaximumAnisotropy(16);
    t->setMipMaxLevel(maxLod);
    t->setMinMagFilters(QOpenGLTexture::LinearMipMapLinear,
                               QOpenGLTexture::Linear);
    t->setWrapMode(QOpenGLTexture::ClampToEdge);


    t->setFormat(QOpenGLTexture::TextureFormat(mips.at(0).glInternalFormat()));
    t->setSize(mips.at(0).size().width(), mips.at(0).size().height());
    t->setMipLevels(mips.size());
    t->allocateStorage();

    QOpenGLPixelTransferOptions uploadOptions;
    uploadOptions.setAlignment(1);

    quint64 sz{0};
    for (int i  = 0; i <= maxLod; ++i) {
        t->setCompressedData(i,
                             mips.at(i).dataLength(),
                             mips.at(i).data().constData() + mips.at(i).dataOffset(),
                             &uploadOptions);
        sz += mips.at(i).dataLength();
    }

    return sz; // astc
}
