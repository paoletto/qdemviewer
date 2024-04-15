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

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QApplication>
#include <QSettings>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtQml>
#include <QImage>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/private/qwebmercator_p.h>
#include <QtLocation/private/qgeocameratiles_p_p.h>
#include <QRandomGenerator>
#include <QRandomGenerator64>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QByteArray>

#include <iostream>
#include <string>
#include <cstdlib>
#include <vector>
#include <set>
#include <unordered_map>

#include <QQuickItem>
#include <QVariantMap>
#include <QAtomicInt>
#include <QQuickFramebufferObject>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShader>
#include <QOpenGLShaderProgram>
#include <QPointer>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLFunctions_4_0_Core>
#include <QOpenGLExtraFunctions>
#include <QOpenGLTexture>
#include <QMatrix4x4>
#include <QQuaternion>
#include <QVector3D>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QEasingCurve>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

#include <map>
#include <QMap>
#include <math.h>
#include <qmath.h>
#include "mapfetcher.h"
#include "shaders.h"

template <typename T> int sgn(T val) {
    return (T(0) < val) - (val < T(0));
}

namespace  {
QOpenGLVertexArrayObject datalessVao;
TileKey superTile(const TileKey &k, quint8 z2) {
    Q_ASSERT(z2 <= k.z);

    int denominator = 1 << (k.z - z2);
    quint64 x = k.x / denominator;
    quint64 y = k.y / denominator;
    return {x,y,z2};
}
int keyToLayers(const TileKey &superk, const TileKey &subk) {
    const int subdivisionLength = 1<<qMax(0, (int(subk.z) - int(superk.z)));
    return subdivisionLength * subdivisionLength;
}
int keyToLayer(const TileKey &superk, const TileKey &subk) {
    const int subdivisionLength = 1<<qMax(0, (int(subk.z) - int(superk.z)));
    const quint64 xorig = superk.x * subdivisionLength;
    const quint64 yorig = superk.y * subdivisionLength;
    return (subk.y - yorig) * subdivisionLength + (subk.x - xorig);
}
std::array<QVector3D, 4> quad{{{0.,0.,0.},{1.,0.,0.},{1.,1.,0.},{0.,1.,0.}}};
float screenSpaceTileSize(const QMatrix4x4 &m) {
    const auto p0 = m * quad[0];
    const auto p1 = m * quad[1];
    const auto p2 = m * quad[2];
    const auto p3 = m * quad[3];
    const auto d0 = p2.toVector2D() - p0.toVector2D();
    const auto d1 = p3.toVector2D() - p1.toVector2D();
    const auto tileSizeNormalized = qMax(d0.length(), d1.length());

    const float radius = 1. + tileSizeNormalized;
    if (   p0.x() < -radius || p0.x() > radius || p0.y() < -radius || p0.y() > radius
        || p1.x() < -radius || p1.x() > radius || p1.y() < -radius || p1.y() > radius
        || p2.x() < -radius || p2.x() > radius || p2.y() < -radius || p2.y() > radius
        || p3.x() < -radius || p3.x() > radius || p3.y() < -radius || p3.y() > radius) {
        return -1; // TODO: broken! needs to keep elevation * scale into account!!!
    }
    return tileSizeNormalized;
}
//QPointF tileCenterScreen(const QMatrix4x4 &m) {
//    return (m * QVector3D(.5,.5,0)).toPointF();
//}
//bool viewportContains(const QPointF &p, float tileSizeNormalized) {
//    float radius = 1. + tileSizeNormalized * .5;
//    return p.x() >= -radius && p.x() <= radius && p.y() >= -radius && p.y() <= radius;
//}
}
struct Origin {
    static void draw(const QMatrix4x4 &transformation,
                     const qreal scale) {
        if (!m_shader) {
            m_shader = new QOpenGLShaderProgram;
            // Create shaders
            m_shader->addShaderFromSourceCode(QOpenGLShader::Vertex,
                                              QByteArray(vertexShaderOrigin));
            m_shader->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                              QByteArray(fragmentShaderOrigin));
            m_shader->link();
        }
        if (!m_shader) {
            qWarning() << "Failed creating origin shader!!";
            return;
        }
        QOpenGLVertexArrayObject::Binder vaoBinder(&datalessVao);
        QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();
        m_shader->bind();
        m_shader->setUniformValue("matrix", transformation);
        m_shader->setUniformValue("scale", float(scale));
        f->glLineWidth(3);
        f->glDrawArrays(GL_LINES, 0, 12);
        f->glLineWidth(1);
        m_shader->release();
    }

    static QOpenGLShaderProgram *m_shader;
};
QOpenGLShaderProgram *Origin::m_shader{nullptr};

class Utilities : public QObject {
    Q_OBJECT
public:
    Utilities(QObject *parent=nullptr) : QObject(parent) {};
    ~Utilities() override {
        init();
        if (m_logFile) {
            m_logFile->write(QJsonDocument::fromVariant(m_requests).toJson());
            m_logFile->close();
        }
    }

    Q_INVOKABLE void setLogPath(const QString &path)
    {
        m_logPath = path;
    }

    Q_INVOKABLE void logRequest(MapFetcher *f,
                                const QList<QGeoCoordinate> &coords,
                                int zoom,
                                int destZoom)
    {
        if (!f)
            return;
        QVariantMap request;
        request["fetcher"] = f->objectName();
        QVariantList crds;
        for (auto c: coords) {
            QVariantMap r;
            r["latitude"] = c.latitude();
            r["longitude"] = c.longitude();
            crds.append(r);
        }
        request["coordinates"] = crds;
        request["zoom"] = zoom;
        request["destZoom"] = destZoom;
        m_requests.append(request);
    }

    Q_INVOKABLE void replay(QVariantList fetchers,
                            QString jsonPath)
    {
        if (fetchers.empty())
            return;

        if (jsonPath.startsWith("file://"))
            jsonPath = jsonPath.mid(7);
        QFileInfo fi(jsonPath);
        if (!fi.exists() || !fi.isReadable()) {
            qWarning() << "Utilities::replay: "<<jsonPath<< " does not exist or is not readable.";
            return;
        }

        QFile f(jsonPath);
        f.open(QIODevice::ReadOnly);
        QJsonParseError err;
        QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error != QJsonParseError::NoError) {
            qWarning() << "Utilities::replay: "<<jsonPath<< " does not parse -- "<<err.errorString();
            return;
        }

        QMap<QString, QObject *> mFetchers;
        for (auto f: qAsConst(fetchers)) {
            QObject *fetcher = qvariant_cast<QObject *>(f);
            mFetchers[fetcher->objectName()] = fetcher;
        }

        const QVariantList queries = d.toVariant().toList();
        for (const auto &q: queries) {
            auto m = q.toMap();
            auto fetcher = m["fetcher"].toString();
            const auto vcoordinates = m["coordinates"].toList();
            auto zoom = m["zoom"].toInt();
            auto destZoom = m["destZoom"].toInt();
            QList<QGeoCoordinate> coordinates;
            for (auto vc: vcoordinates) {
                auto vcm = vc.toMap();
                double lat = vcm["latitude"].toDouble();
                double lon = vcm["longitude"].toDouble();
                coordinates.append(QGeoCoordinate(lat, lon));
            }

            if (!mFetchers.contains(fetcher))
                continue;
            qobject_cast<MapFetcher *>(mFetchers[fetcher])->requestSlippyTiles(
                            coordinates,
                            zoom,
                            destZoom,
                            false);
        }
    }

private:
    void init()
    {
        if (m_logFile)
            m_logFile->close();

        if (m_logPath.isEmpty())
            m_logFile = {};

        QSharedPointer<QIODevice> f(new QFile (m_logPath));
        f->open(QIODevice::WriteOnly | QIODevice::Truncate);
        if (!f->isOpen() || !f->isWritable())
            m_logFile = {};
        m_logFile = f;
    }

    QString m_logPath;
    QSharedPointer<QIODevice> m_logFile;
    QVariantList m_requests;
};

class ArcBall : public QObject {
    Q_OBJECT

    Q_PROPERTY(QVariant modelTransformation READ modelTransformation WRITE setModelTransformation)
public:
    ArcBall(QObject *parent = nullptr) : QObject(parent) {
        m_view.lookAt(QVector3D(0,0,10), QVector3D(0,0,0), QVector3D(0,1,0));
        m_projection.ortho(-1,1,-1,1,-2000,2000);
    }

    Q_INVOKABLE void reset() {
        m_transformation.setToIdentity();
    }

    Q_INVOKABLE void setSize(QSize winSize) {
        m_winSize = winSize;
        if (m_winSize.isEmpty())
            return;
        QVector3D vecBottomLeft = QVector3D(-1.0f,-1.0f,0.0f);
        QVector3D vecTopRight   = QVector3D(1.0f,1.0f,0.0f);

        float fExtentX = vecTopRight.x() - vecBottomLeft.x();
        float fExtentY = vecTopRight.y() - vecBottomLeft.y();

        float fRatioCanvas = float(m_winSize.width()) / float(m_winSize.height());

        float fMissingRatio = 1. / fRatioCanvas;
        float fRescaleFactor = 1.0f;

        if (fMissingRatio < 1.0f)
            fRescaleFactor = 1.0f / fMissingRatio;

        const float fFrustumWidth = fExtentX * fRescaleFactor;
        const float fHorizontalSpacing = (fFrustumWidth - fExtentX) / 2.0f;

        const float fFrustumHeight = fExtentY * fRescaleFactor * fMissingRatio;
        const float fVerticalSpacing = (fFrustumHeight - fExtentY) / 2.0f;

        float fHorizontalSign = 1.0f;
        if (vecBottomLeft.x() > vecTopRight.x())
            fHorizontalSign = -fHorizontalSign;

        float fVerticalSign = 1.0f;
        if (vecBottomLeft.x() > vecTopRight.x())
            fVerticalSign = -fVerticalSign;


        vecBottomLeft = QVector3D(	vecBottomLeft.x() - fHorizontalSign * fHorizontalSpacing
                                  , vecBottomLeft.y() - fVerticalSign * fVerticalSpacing
                                  , 0.0f);
        vecTopRight   = QVector3D(	vecTopRight.x() + fHorizontalSign * fHorizontalSpacing
                                  , vecTopRight.y() +  fVerticalSign * fVerticalSpacing
                                  , 0.0f);

        m_projection.setToIdentity();
        m_projection.ortho(vecBottomLeft.x(),
                           vecTopRight.x(),
                           vecBottomLeft.y(),
                           vecTopRight.y(),
                           -2000,
                           2000);
    }

    Q_INVOKABLE void pressed(QPoint m) {
        if (m_winSize.isEmpty() || m_panActive)
            return;
        m_active = true;
        m_pressedPoint = QVector3D(2.0f * m.x() / m_winSize.width() - 1.0,
                                   1.0 - 2.0f * m.y() / m_winSize.height(), 0);
    }

    Q_INVOKABLE void midPressed(QPoint m) {
        if (m_winSize.isEmpty() || m_active)
            return;
        m_panActive = true;
        m_pressedPoint = QVector3D(2.0f * m.x() / m_winSize.width() - 1.0,
                                   1.0 - 2.0f * m.y() / m_winSize.height(), 0);
    }

    Q_INVOKABLE void zoom(float delta) {
        QMatrix4x4 matScale;
        float scale = 1.0 + sgn(delta) * 0.1;
        matScale.scale(scale);

        m_transformation = matScale * m_transformation;
        emit transformationChanged();
    }

    Q_INVOKABLE void moved(QPoint m) {
        if (m_winSize.isEmpty() || (!m_active && !m_panActive))
            return;
        //https://stackoverflow.com/questions/54498361/opengl-arcball-for-rotating-mesh
        m_current.setToIdentity();
        QVector3D p(2.0f * m.x() / m_winSize.width() - 1.0,
                    1.0 - 2.0f * m.y() / m_winSize.height(), 0);
        QVector3D direction = p - m_pressedPoint;
        if (m_active) {
            QVector3D rotationAxis = QVector3D(-direction.y(), direction.x(), 0.0).normalized();
            float angle = (float)qRadiansToDegrees(direction.length() * M_PI);

            m_current.rotate(angle, rotationAxis.x(), rotationAxis.y(), rotationAxis.z());
        } else if (m_panActive) {
            m_current.translate(direction);
        }
        emit transformationChanged();
    }

    Q_INVOKABLE void released() {
        m_transformation = m_current * m_transformation;
        m_current.setToIdentity();
        m_active = m_panActive = false;
    }

    QMatrix4x4 transformation() const {
        return m_projection * m_view * (m_current * m_transformation) ;
    }

    QVariant modelTransformation() const {
        return QVariant::fromValue(m_transformation);
    }

    void setModelTransformation(const QVariant &modelTrafo) {
        m_transformation = qvariant_cast<QMatrix4x4>(modelTrafo);
    }

    Q_INVOKABLE QPointF normalizeIfNeeded(const QPointF p) {
        QVector2D v(p);
        if (v.length() > 1.)
            v.normalize();
        return QPointF(v.x(), v.y());
    }

signals:
    void transformationChanged();

private:
    // TODO: fixme
    QVector3D getArcBallVector(QPoint p) {
        QVector3D pt = QVector3D((2.0 * p.x() / m_winSize.width() - 1.0) ,
                                 (2.0 * p.y() / m_winSize.height() - 1.0) * -1., 0);

        // compute z-coordinates
        qreal xySquared = pt.x() * pt.x() + pt.y() * pt.y();
        if (xySquared <= 1.0)
            pt.setZ(std::sqrt(1.0 - xySquared));
        else
            pt.normalize();

        return pt;
    }

    QSize m_winSize;
    QVector3D m_pressedPoint;
    bool m_active{false};
    bool m_panActive{false};

    QMatrix4x4 m_projection;
    QMatrix4x4 m_view;
    QMatrix4x4 m_transformation;
    QMatrix4x4 m_current;
};

struct Tile
{
    Tile(const TileKey k,
         const QSize resolution)
        : m_key(k), m_resolution(resolution)
    {
    }

    Tile(Tile&& mE) = default;
    Tile(const Tile& mE) = default;

    bool operator==(const Tile& o) const {
        return m_key == o.m_key;
    }

    bool operator<(const Tile& o) const {
        return m_key < o.m_key;
    }

    void setDem(std::shared_ptr<Heightmap> dem) {
        // TODO: do this in a better way
        if (m_texDem && m_hasBorders)
            return;
        m_dem = std::move(*dem);
        m_resolution = m_dem.size();
    }

    void setMap(std::shared_ptr<CompressedTextureData> map) {
        m_map = map;
        if (!m_map)
            m_rasterBytes = 0;
    }

    void setRasterSubtile(const TileKey &k,
                          std::shared_ptr<CompressedTextureData> tileRaster) {
        if (!tileRaster) {
            if (m_rasterSubtiles.find(k) == m_rasterSubtiles.end())
                return;
            else
                m_rasterSubtiles.erase(k);
        }
        m_rasterSubtiles[k] = tileRaster;
    }

    void setNeighbors(std::shared_ptr<Tile> demBottom = {},
                      std::shared_ptr<Tile> demRight = {},
                      std::shared_ptr<Tile> demBottomRight = {}) {
        if (demBottom)
            m_bottom = demBottom;
        if (demRight)
            m_right = demRight;
        if (demBottomRight)
            m_bottomRight = demBottomRight;
    }


    void init()
    {
        if (m_initialized)
            return;
        m_initialized = true;
        // VAOs
        QOpenGLVertexArrayObject::Binder vaoBinder(&datalessVao); // creates it when bound the first time
    }

    QSharedPointer<QOpenGLTexture> demTexture() {
        if (!m_dem.size().isEmpty() /*&& !m_texDem*/) {
            Heightmap &h = m_dem;
            m_hasBorders = h.m_hasBorders;

            int maxHSize = std::max(h.m_size.width(), h.m_size.height());
            if (m_maxTexSize && maxHSize > m_maxTexSize) {
                h.rescale(m_maxTexSize);
//            h.rescale(QSize(32,32));
                m_resolution = h.size();
            }

            if (!m_texDem || QSize(m_texDem->width(), m_texDem->height()) != h.m_size) {
                m_texDem.reset(new QOpenGLTexture(QOpenGLTexture::Target2D));
                m_texDem->setFormat(QOpenGLTexture::R32F);
                m_texDem->setSize(h.m_size.width(), h.m_size.height());
                m_texDem->allocateStorage(QOpenGLTexture::Red, QOpenGLTexture::Float32);
            }
            m_texDem->setData(QOpenGLTexture::Red,
                              QOpenGLTexture::Float32,
                              (const void *) &(h.elevations.front()));

            m_dem = Heightmap();
        }
        return m_texDem;
    }

    QSharedPointer<QOpenGLTexture> mapTexture()
    {
        if (m_map) {

            m_rasterBytes = m_map->upload(m_texMap);
            m_compressedRaster = m_map->hasCompressedData();
            m_map = nullptr;
        } else if (m_rasterSubtiles.size()) { // assume they are the correct subcontent for this tile.
            m_compressedRaster = true;
            std::map<TileKey, std::shared_ptr<CompressedTextureData>> subtiles;
            subtiles.swap(m_rasterSubtiles);
            for (auto &st: subtiles) {
                const int layers = keyToLayers(m_key, st.first);
                const int layer = keyToLayer(m_key, st.first);
                m_rasterBytes += st.second->uploadTo2DArray(m_texMap,layer, layers);
            }
        }
        return m_texMap;
    }

    void draw(const QMatrix4x4 &transformation,
              const Tile& origin,
              const QSize viewportSize,
              const float elevationScale,
              const float brightness,
              const int tessellationDirection,
              const QVector3D &lightDirection,
              bool interactive,
              bool joinTiles,
              bool autoStride,
              int downsamplingRate)
    {
        QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();

        if (!m_shader) {            
            f->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_maxTexSize);
            f->glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &m_maxTexLayers);

            m_shader = new QOpenGLShaderProgram;
            m_shader->addShaderFromSourceCode(QOpenGLShader::Vertex,
                                              QByteArray(vertexShaderTile));
            m_shader->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                              QByteArray(fragmentShaderTile));
            m_shader->link();
            m_shader->setObjectName("shader");

            m_shaderTextured = new QOpenGLShaderProgram;
            m_shaderTextured->addShaderFromSourceCode(QOpenGLShader::Vertex,
                                              QByteArray(vertexShaderTile));
            m_shaderTextured->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                              QByteArray(fragmentShaderTileTextured));
            m_shaderTextured->link();
            m_shaderTextured->setObjectName("shaderTextured");

            m_shaderJoinedDownsampledTextured = new QOpenGLShaderProgram;
            // Create shaders
            m_shaderJoinedDownsampledTextured->addShaderFromSourceCode(QOpenGLShader::Vertex,
                                              QByteArray(vertexShaderTileJoinedDownsampled));
            m_shaderJoinedDownsampledTextured->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                              QByteArray(fragmentShaderTileTextured));
            m_shaderJoinedDownsampledTextured->link();
            m_shaderJoinedDownsampledTextured->setObjectName("shaderJoinedDownsampledTextured");

            m_shaderJoinedDownsampledTextureArrayd = new QOpenGLShaderProgram;
            // Create shaders
            m_shaderJoinedDownsampledTextureArrayd->addShaderFromSourceCode(QOpenGLShader::Vertex,
                                              QByteArray(vertexShaderTileJoinedDownsampled));
            m_shaderJoinedDownsampledTextureArrayd->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                              QByteArray(fragmentShaderTileTextureArrayd));
            m_shaderJoinedDownsampledTextureArrayd->link();
            m_shaderJoinedDownsampledTextureArrayd->setObjectName("shaderJoinedDownsampledTextureArrayd");

            m_texWhite.reset(new QOpenGLTexture(QOpenGLTexture::Target2D));
            m_texWhite->setMaximumAnisotropy(16);
            m_texWhite->setMinMagFilters(QOpenGLTexture::LinearMipMapLinear,
                                         QOpenGLTexture::Linear);
            m_texWhite->setWrapMode(QOpenGLTexture::ClampToEdge);
            QImage white(2,2,QImage::Format_RGB888);
            white.setPixelColor(0,0, QColorConstants::White);
            white.setPixelColor(0,1, QColorConstants::White);
            white.setPixelColor(1,0, QColorConstants::White);
            white.setPixelColor(1,1, QColorConstants::White);
            m_texWhite->setData(white);
        }
        if (!m_shader) {
            qWarning() << "Failed creating shader!";
            return;
        }
        if (m_resolution.isEmpty())
            return; // skip drawing the tile

        const auto tileMatrix = tileTransformation(origin);
        const QMatrix4x4 m = transformation * tileMatrix;
        const float tileSize = screenSpaceTileSize(m);

        if (tileSize < 0)
            return; // TODO: improve

        const float tileSizePixels = tileSize * viewportSize.width();

        int idealRate = qMax<int>(1, 256. / (1 << int(ceil(log2(tileSizePixels)))));


        auto rasterTxt = mapTexture();
        // TODO: streamline, fix, deduplicate
        QOpenGLShaderProgram *shader = (rasterTxt) ? m_shaderTextured
                                                   : m_shader;
        if (interactive && joinTiles) {
            if (rasterTxt && rasterTxt->layers() > 1)
                shader = m_shaderJoinedDownsampledTextureArrayd;
            else
                shader = m_shaderJoinedDownsampledTextured;
        }

        QOpenGLExtraFunctions *ef = QOpenGLContext::currentContext()->extraFunctions();
        ef->glBindImageTexture(0, (demTexture()) ? demTexture()->textureId() : 0, 0, 0, 0,  GL_READ_ONLY, GL_RGBA8); // TODO: test readonly

        shader->bind();
        f->glEnable(GL_TEXTURE_2D);
        if (rasterTxt)  {
            rasterTxt->bind();
        } else {
            m_texWhite->bind();
        }

        int stride = (interactive) ?
                        qBound(1 ,
                               autoStride ? qMax(downsamplingRate, idealRate) : downsamplingRate,
                               128)
                        : 1;

        auto resolution = m_resolution;
        if (joinTiles && interactive)
            resolution -= QSize(2,2);
        resolution /= stride;
        shader->setUniformValue("resolution", resolution);

        shader->setUniformValue("elevationScale", elevationScale);


        shader->setUniformValue("matrix", m);
        shader->setUniformValue("color", QColor(255,255,255,255));

//        if (!rasterTxt || rasterTxt->layers() == 1) {
//            shader->setUniformValue("raster", 0);
//        } else {
//            shader->setUniformValue("rasterArray",0);
//        }
        shader->setUniformValue("samplingStride", stride);
        shader->setUniformValue("brightness", (rasterTxt) ? brightness : 1.0f );
        shader->setUniformValue("quadSplitDirection", tessellationDirection);
        shader->setUniformValue("lightDirection", lightDirection);
        shader->setUniformValue("cOff", (joinTiles && !interactive) ? -0.5f: 0.5f);
        shader->setUniformValue("joined", int(joinTiles));
        shader->setUniformValue("numSubtiles", int((!rasterTxt) ? 1 : rasterTxt->layers()));

        QOpenGLVertexArrayObject::Binder vaoBinder(&datalessVao);
        const int numVertices = totVertices(joinTiles, stride);

        f->glDrawArrays(GL_TRIANGLES, 0, numVertices);
        shader->release();
        if (rasterTxt)
            rasterTxt->release();
        else
            m_texWhite->release();
    }

    inline int totVertices(bool joinTiles, int stride = 1) const {
        const int toSubtract = (joinTiles && stride > 1) ? 2 : 0;
//        const int toAdd = (joinTiles && stride == 1) ? 2 : 0;
        const int toAdd = (joinTiles) ? 2 : 0;
        return ((m_resolution.width() - toSubtract) / stride + toAdd  - 1)
                * ((m_resolution.height() - toSubtract) / stride + toAdd - 1) * 6;
    }

    QMatrix4x4 tileTransformation(const Tile& origin) const {
        qint64 xdiff = m_key.x - origin.m_key.x;
        qint64 ydiff = m_key.y - origin.m_key.y;
        QMatrix4x4 res;
        res.translate(xdiff, -ydiff -1); // We put 0,0 below the x-axis

        if (m_resolution.width() != m_resolution.height()) {
            float ypct = float(m_resolution.height()) / float(m_resolution.width());
            if (m_hasBorders)
                ypct = float(m_resolution.height() - 1) / float(m_resolution.width() - 1); // two data points draw at half size
            res.translate(0, -(ypct - 1));
        }

        return res;
    }

    quint64 allocatedGraphicsMemoryBytes() const {
        quint64 res = 0;
        if (m_texDem)
            res += m_texDem->width() * m_texDem->height() * 4; // r32f
        if (m_texMap)
            //res += (m_texMap->width() * m_texMap->height() * sizeof(GLfloat)) * 1.3333; //mipmapped
            res += m_rasterBytes;
        return res;
    }

    // reuse the same every time
    TileKey m_key;
    QSize m_resolution; // GLint bcz somehow setUniformValue is picking the wrong overload otherwise

    bool m_compressedRaster{false};
    quint64 m_rasterBytes{0};
    bool m_hasBorders{false};
    bool m_initialized{false};
    Heightmap m_dem;
    QSharedPointer<QOpenGLTexture> m_texDem; // To make it easily copyable

    std::shared_ptr<CompressedTextureData> m_map;
    std::map<TileKey, std::shared_ptr<CompressedTextureData>> m_rasterSubtiles;
    QSharedPointer<QOpenGLTexture> m_texMap;

    std::shared_ptr<Tile> m_right;
    std::shared_ptr<Tile> m_bottom;
    std::shared_ptr<Tile> m_bottomRight;

    static GLint m_maxTexSize;
    static GLint m_maxTexLayers;
    static QOpenGLShaderProgram *m_shader;
    static QOpenGLShaderProgram *m_shaderTextured;
    static QOpenGLShaderProgram *m_shaderJoinedDownsampledTextured;
    static QOpenGLShaderProgram *m_shaderJoinedDownsampledTextureArrayd;
    static QScopedPointer<QOpenGLTexture> m_texWhite;
};
QOpenGLShaderProgram *Tile::m_shader{nullptr};
QOpenGLShaderProgram *Tile::m_shaderTextured{nullptr};
QOpenGLShaderProgram *Tile::m_shaderJoinedDownsampledTextured{nullptr};
QOpenGLShaderProgram *Tile::m_shaderJoinedDownsampledTextureArrayd{nullptr};
QScopedPointer<QOpenGLTexture> Tile::m_texWhite;
GLint Tile::m_maxTexSize{0};
GLint Tile::m_maxTexLayers{0};

QDebug operator<<(QDebug d, const  Tile &t) {
        QDebug nsp = d.nospace();
        nsp << t.m_key;
        return d;
}

class TileRenderer : public QQuickFramebufferObject::Renderer
{
public:
    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override {
        return createFbo(size);
    }

    void addTile(const TileKey k,
                 const QSize resolution) {
        std::shared_ptr<Tile> t = std::make_shared<Tile>(k, resolution);
        if (m_tiles.find(k) != m_tiles.end())
            return;
        m_tiles.insert({k, std::move(t)});
    }

    void updateTileDem( const TileKey k,
                        std::shared_ptr<Heightmap> dem) {

        auto t = m_tiles.find(k);
        if (t == m_tiles.end()) {
            addTile(k, dem->size());
            return updateTileDem(k, std::move(dem));
        }
        t->second->setDem(std::move(dem));
    }

    void updateTileRaster(const TileKey k,
                          std::shared_ptr<CompressedTextureData> raster)
    {
#if 1
        auto t = m_tiles.find(k);
        if (t != m_tiles.end()) {
            t->second->setMap(std::move(raster));
            return;
        }
        if (k.z == 0) // no parent tiles possible
            return;

        // As this is a demo, it makes a strong assumption: if a raster tile of zoom level
        // z2 > k.z arrives, it is expected that all other subtiles for k will come.

        auto superZ = hasSuperTile(k);
        if (superZ < 0)
            return;

        m_tiles.find(superTile(k, superZ))->second->setRasterSubtile(k, std::move(raster));
#else
        auto t = m_tiles.find(k);
        if (t == m_tiles.end())
            return;

        t->second->setMap(std::move(raster));
#endif
    }

    void updateNeighbors() {
        for (auto &tt: m_tiles) {
            std::shared_ptr<Tile> &t = tt.second;
            std::shared_ptr<Tile> r = tile({t->m_key.x + 1, t->m_key.y, t->m_key.z});
            std::shared_ptr<Tile> b = tile({t->m_key.x, t->m_key.y + 1, t->m_key.z});
            std::shared_ptr<Tile> br = tile({t->m_key.x + 1, t->m_key.y + 1, t->m_key.z});

            t->setNeighbors(b,r,br);
        }
    }

    bool hasTile(const TileKey &k) const {
        return m_tiles.find(k) != m_tiles.end();
    }

    int hasSuperTile(const TileKey &k) const {
        const int endRange = qMax(0, k.z - 5); // Using up to 5 zoom levels up. 5 is arbitrary.
                                               // 2^5 = 32 (x32 = 1024). Intel graphics supports 2048 layers.
                                               // TODO: make it depend on actual raster size vs 3D texture max layers
        for (quint8 z = k.z - 1; z >= endRange; z--) {
            auto t = m_tiles.find(superTile(k, z));
            if (t == m_tiles.end())
                continue;
            return z;
        }

        return -1;
    }

    std::shared_ptr<Tile> tile(const TileKey &k) const {
        auto it = m_tiles.find(k);
        if (it != m_tiles.end())
            return it->second;
        return {};
    }

    void clearTiles() {
        m_tiles.clear();
    }

    quint64 allocatedGraphicsMemoryBytes() const {
        quint64 res = 0;
        for (const auto &t: m_tiles)
            res += t.second->allocatedGraphicsMemoryBytes();
        return res;
    }

protected:
    QOpenGLFramebufferObject *createFbo(const QSize &size) {
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::Depth);
//        format.setSamples(4);
        m_fbo =  new QOpenGLFramebufferObject(size, format);
        return m_fbo;
    }

    void initBase(QOpenGLFunctions *f, QQuickWindow *w) {
        f->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_glMaxTexSize);
        m_window = w;
    }

    void init(QOpenGLFunctions *f, QQuickWindow *w) {
        if (std::numeric_limits<int>::max() == m_glMaxTexSize) {
            initBase(f, w);
        }
    }

    /// encapsulate common code in synchronize. returns false if rest is to be skipped
    void synchronize(QQuickFramebufferObject *item) override;

    void render() override
    {
        QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();
//        auto *f4 = QOpenGLContext::currentContext()->versionFunctions<QOpenGLFunctions_4_0_Core>();

        f->glClearColor(0, 0, 0, 0);
        f->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        f->glEnable(GL_DEPTH_TEST);
        f->glDepthFunc(GL_LESS);
        f->glDepthMask(true);


//        GLint polygonMode;
//        f->glGetIntegerv(GL_POLYGON_MODE, &polygonMode);
//        f4->glPolygonMode( GL_FRONT_AND_BACK, polygonMode );
//        QOpenGLExtraFunctions *ef = QOpenGLContext::currentContext()->extraFunctions();
//        f45->glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );


        const bool fast = true; //(m_interactive && m_fastInteraction) || (m_fastInteraction && !m_autoRefinement);

        Origin::draw(m_arcballTransform, 1);
        for (auto &t: m_tiles) {
            t.second->draw(m_arcballTransform,
                   *m_tiles.begin()->second,
                   m_fbo->size(),
                   m_elevationScale,
                   m_brightness,
                   m_tessellationDirection,
                   m_lightDirection,
                   fast,
                   m_joinTiles,
                   m_autoRefinement,
                   m_downsamplingRate);
        }

//        f4->glPolygonMode( GL_FRONT_AND_BACK, polygonMode );
        if (m_window)
            m_window->resetOpenGLState();
    }

    quint64 sceneTriangles() const {
        quint64 count = 0;
        for (auto &t: m_tiles) {
            count += t.second->totVertices(m_joinTiles);
        }
        return count / 3;
    }

    QPointer<QQuickWindow> m_window;
    QOpenGLFramebufferObject *m_fbo = nullptr;
    int m_glMaxTexSize = std::numeric_limits<int>::max();
    QMatrix4x4 m_mvp;
    QMatrix4x4 m_arcballTransform;
    float m_elevationScale{500};
    float m_brightness{1.};
    bool m_joinTiles{false};
    int m_tessellationDirection{0};
    QVector3D m_lightDirection{0,0,-1};
    bool m_interactive{false};
    bool m_fastInteraction{false};
    bool m_autoRefinement{false};
    int m_downsamplingRate{8};
    std::map<TileKey, std::shared_ptr<Tile>> m_tiles {};
    // TODO use a state struct
};

class TerrainViewer : public QQuickFramebufferObject
{
    Q_OBJECT

    Q_PROPERTY(QVariant interactor READ getArcball WRITE setArcball)
    Q_PROPERTY(QVariant demFetcher READ getDEMFetcher WRITE setDEMFetcher)
    Q_PROPERTY(QVariant rasterFetcher READ rasterFetcher WRITE setRasterFetcher)
    Q_PROPERTY(qreal elevationScale READ elevationScale WRITE setElevationScale)
    Q_PROPERTY(qreal brightness READ brightness WRITE setBrightness)
    Q_PROPERTY(bool joinTiles READ joinTiles WRITE setJoinTiles)
    Q_PROPERTY(int tessellationDirection READ tessellationDirection WRITE setTessellationDirection)
    Q_PROPERTY(quint64 triangles READ numTriangles NOTIFY numTrianglesChanged)
    Q_PROPERTY(quint64 allocatedGraphicsBytes READ allocatedGraphicsBytes NOTIFY allocatedGraphicsBytesChanged)
    Q_PROPERTY(QVariant lightDirection READ lightDirection WRITE setLightDirection)
    Q_PROPERTY(bool offline READ offline WRITE setOffline NOTIFY offlineChanged)
    Q_PROPERTY(bool logRequests READ logRequests WRITE setLogRequests NOTIFY logRequestsChanged)
    Q_PROPERTY(bool astcEnabled READ astc WRITE setAstc NOTIFY astcChanged)
    Q_PROPERTY(bool fastInteraction READ fastInteraction WRITE setFastInteraction NOTIFY fastInteractionChanged)
    Q_PROPERTY(bool autoRefinement READ autoRefinement WRITE setAutoRefinement NOTIFY autoRefinementChanged)
    Q_PROPERTY(int downsamplingRate READ downsamplingRate WRITE setDownsamplingRate)
    Q_PROPERTY(bool autoRefinement READ autoRefinement WRITE setAutoRefinement NOTIFY autoRefinementChanged)


public:
    TerrainViewer(QQuickItem *parent = nullptr) : QQuickFramebufferObject(parent) {
        setFlag(ItemHasContents);
        setTextureFollowsItemSize(true);
        setMirrorVertically(true);
        m_updateTimer.setInterval(1800);
        connect(&m_updateTimer, &QTimer::timeout, this, &TerrainViewer::fullUpdate);
        m_updateTimer.setSingleShot(true);
        m_provisioningUpdateTimer.setInterval(500);
        connect(&m_provisioningUpdateTimer, &QTimer::timeout, this, &TerrainViewer::interactiveUpdate);
        m_provisioningUpdateTimer.setSingleShot(true);
    }
    ~TerrainViewer() override {

    }

    Q_INVOKABLE void reset() {
        m_reset = true;
        interactiveUpdate();
    }

    void setArcball(QVariant value) {
        if (m_arcball)
            return;
        m_arcball = qobject_cast<ArcBall *>(qvariant_cast<QObject *>(value));
        if (m_arcball)
            QObject::connect(m_arcball, &ArcBall::transformationChanged,
                             this, &TerrainViewer::onTransformationChanged);
    }

    QVariant getArcball() const {
        return QVariant::fromValue(m_arcball);
    }

    void setDEMFetcher(QVariant value) {
        if (m_demFetcher)
            return;
        m_demFetcher = qobject_cast<DEMFetcher *>(qvariant_cast<QObject *>(value));
        if (m_demFetcher) {
            QObject::connect(m_demFetcher, &DEMFetcher::heightmapReady,
                             this, &TerrainViewer::onDtmReady);
            QObject::connect(m_demFetcher, &DEMFetcher::heightmapCoverageReady,
                             this, &TerrainViewer::onCoverageReady);
            QObject::connect(m_demFetcher, &MapFetcher::requestHandlingFinished,
                             this, &TerrainViewer::onRequestHandlingFinished);
        }
    }

    QVariant getDEMFetcher() const {
        return QVariant::fromValue(m_demFetcher);
    }

    void setRasterFetcher(QVariant value) {
        if (m_rasterFetcher)
            return;
        m_rasterFetcher = qobject_cast<ASTCFetcher *>(qvariant_cast<QObject *>(value));
        if (m_rasterFetcher) {
            QObject::connect(m_rasterFetcher, &MapFetcher::tileReady,
                             this, &TerrainViewer::onMapTileReady);
            QObject::connect(m_rasterFetcher, &MapFetcher::coverageReady,
                             this, &TerrainViewer::onMapCoverageReady);
            QObject::connect(m_rasterFetcher, &MapFetcher::requestHandlingFinished,
                             this, &TerrainViewer::onRequestHandlingFinished);
        }
    }

    QVariant rasterFetcher() const {
        return QVariant::fromValue(m_rasterFetcher);
    }

    qreal elevationScale() const {
        return m_elevationScale;
    }

    void setElevationScale(qreal scale) {
        m_elevationScale = scale;
        interactiveUpdate();
    }

    qreal brightness() const {
        return m_brightness;
    }

    void setBrightness(qreal b) {
        m_brightness = b;
        interactiveUpdate();
    }

    bool joinTiles() const {
        return m_joinTiles;
    }

    void setJoinTiles(bool v) {
        m_joinTiles = v;
        m_demFetcher->setBorders(m_joinTiles);
        interactiveUpdate();
    }

    int tessellationDirection() const {
        return m_tessellationDirection;
    }

    void setTessellationDirection(int v) {
        m_tessellationDirection = v;
        interactiveUpdate();
    }

    quint64 numTriangles() const {
        return m_numTriangles;
    }

    void setNumTriangles(quint64 t) {
        if (m_numTriangles == t)
            return;
        m_numTriangles = t;
        emit numTrianglesChanged();
    }

    quint64 allocatedGraphicsBytes() const {
        return m_allocatedGraphicsBytes;
    }

    void setAllocatedGraphicsBytes(quint64 bytes) {
        if (bytes == m_allocatedGraphicsBytes)
            return;
        m_allocatedGraphicsBytes = bytes;
        emit allocatedGraphicsBytesChanged();
    }

    QVariant lightDirection() const {
        return m_lightDirection;
    }

    void setLightDirection(QVariant ld) {
        m_lightDirection = ld.toPointF();
        interactiveUpdate();
    }

    bool offline() const {
        return NetworkConfiguration::offline;
    }

    void setOffline(bool offline) {
        if (NetworkConfiguration::offline == offline)
            return;
        NetworkConfiguration::offline = offline;
        emit offlineChanged();
    }

    bool logRequests() const {
        return NetworkConfiguration::logNetworkRequests;
    }

    void setLogRequests(bool enabled) {
        if (NetworkConfiguration::logNetworkRequests == enabled)
            return;
        NetworkConfiguration::logNetworkRequests = enabled;
        emit logRequestsChanged();
    }

    bool astc() const {
        return NetworkConfiguration::astcEnabled;
    }

    void setAstc(bool enabled) {
        if (NetworkConfiguration::astcEnabled == enabled)
            return;
        NetworkConfiguration::astcEnabled = enabled;
        emit astcChanged();
    }

    bool fastInteraction() const {
        return m_fastInteraction;
    }

    void setFastInteraction(bool enabled) {
        if (m_fastInteraction == enabled)
            return;
        m_fastInteraction = enabled;
        interactiveUpdate();
        emit fastInteractionChanged();
    }


    bool autoRefinement() const {
        return m_autoRefinement;
    }

    void setAutoRefinement(bool enabled) {
        if (m_autoRefinement == enabled)
            return;
        m_autoRefinement = enabled;
        interactiveUpdate();
        emit autoRefinementChanged();
    }

    int downsamplingRate() const {
        return m_downsamplingRate;
    }

    void setDownsamplingRate(int rate) {
        if (rate == m_downsamplingRate)
            return;
        m_downsamplingRate = rate;
        interactiveUpdate();
    }

signals:
    void numTrianglesChanged();
    void allocatedGraphicsBytesChanged();
    void offlineChanged();
    void astcChanged();
    void fastInteractionChanged();
    void autoRefinementChanged();
    void logRequestsChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override {
        if (oldNode && m_recreateRenderer) {
            delete oldNode;
            oldNode = nullptr;
            releaseResources(); // nullifies d->node
            m_recreateRenderer = false;
        }

        if (!oldNode) {
            oldNode = QQuickFramebufferObject::updatePaintNode(oldNode, data);
            return oldNode;
        }

        return QQuickFramebufferObject::updatePaintNode(oldNode, data);
    }
    Renderer *createRenderer() const override {
        return new TileRenderer;
    }

protected slots:
    void delayedUpdate() {
        if (!m_provisioningUpdateTimer.isActive())
            m_provisioningUpdateTimer.start();
    }

    void interactiveUpdate() {
        m_interactive = true;
        update();
        m_updateTimer.start();
    }

    void fullUpdate() {
        m_interactive = false;
        update();
    }

    void onTransformationChanged() {
        interactiveUpdate();
    }

    void onDtmReady(quint64 id, const TileKey k) {
        m_newTiles[k] =  m_demFetcher->heightmap(id, k);
        delayedUpdate();
    }

    void onCoverageReady(const quint64 id) {
        if (!m_demFetcher)
            return;
        reset();
        m_newTiles.clear();
        m_newTiles[TileKey{0,0,0}] = m_demFetcher->heightmapCoverage(id);
        delayedUpdate();
    }

    void onMapTileReady(quint64 id, const TileKey k) {
        if (!m_rasterFetcher)
            return;
        m_newMapRasters[k] = m_rasterFetcher->tile(id, k);
        delayedUpdate();
    }

    void onMapCoverageReady(const quint64 id) {
        if (!m_rasterFetcher)
            return;
        m_newMapRasters.clear(); // prb useless
        auto raster = m_rasterFetcher->tileCoverage(id);

        m_newMapRasters[TileKey{0,0,0}] =  std::move(raster);
        delayedUpdate();
    }

    void onRequestHandlingFinished(quint64 id) {
        qInfo() << "Request "<<id<< " finished. sender: "<<sender();
    }


private:
    ArcBall *m_arcball{nullptr};
    DEMFetcher *m_demFetcher{nullptr};
    ASTCFetcher *m_rasterFetcher{nullptr};
    bool m_recreateRenderer = false;
    bool m_reset = false;
    qreal m_elevationScale{500.0};
    qreal m_brightness{1.0};
    bool m_joinTiles{false};
    bool m_tessellationDirection{0};
    quint64 m_numTriangles{0};
    quint64 m_allocatedGraphicsBytes{0};
    bool m_interactive{false};
    bool m_fastInteraction{false};
    bool m_autoRefinement{false};
    int m_downsamplingRate{8};
    QPointF m_lightDirection;
    QTimer m_updateTimer;
    QTimer m_provisioningUpdateTimer;

    std::map<TileKey, std::shared_ptr<Heightmap>> m_newTiles;
    std::map<TileKey, std::shared_ptr<CompressedTextureData>> m_newMapRasters;

    friend class TileRenderer;
    Q_DISABLE_COPY(TerrainViewer)
};

void TileRenderer::synchronize(QQuickFramebufferObject *item)
{
    QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();
    init(f, item->window());
    TerrainViewer *viewer = qobject_cast<TerrainViewer *>(item);
    if (viewer && viewer->m_arcball)
        m_arcballTransform = viewer->m_arcball->transformation();

    if (viewer) {
            bool reset = viewer->m_reset;
        viewer->m_reset = false;

        m_elevationScale = viewer->m_elevationScale;
        m_brightness = viewer->m_brightness;
        const bool oldJoinTiles = m_joinTiles;
        m_joinTiles = viewer->joinTiles();

        m_tessellationDirection = viewer->tessellationDirection();
        const auto ld = viewer->lightDirection().toPointF();

        m_lightDirection = QVector3D(-ld.x(), ld.y(), 0);
        m_lightDirection.setZ(-sqrt(1. - m_lightDirection.lengthSquared()));
        m_interactive = viewer->m_interactive;
        m_fastInteraction = viewer->m_fastInteraction;
        m_autoRefinement = viewer->m_autoRefinement;
        m_downsamplingRate = viewer->m_downsamplingRate;

        std::map<TileKey, std::shared_ptr<Heightmap>> newTiles;
        newTiles.swap(viewer->m_newTiles);

        if ((   newTiles.size()
                && m_tiles.size()
                && m_tiles.begin()->second->m_key.z != newTiles.begin()->first.z)
            || reset
            || (m_joinTiles != oldJoinTiles))
            clearTiles();

        for (auto &t: newTiles)
            updateTileDem(t.first, std::move(t.second));
        updateNeighbors();

        std::vector<TileKey> keys;
        for (const auto &it: viewer->m_newMapRasters)
            keys.push_back(it.first);

        for (const auto &k: keys) {
            if (hasTile(k) || hasSuperTile(k) >= 0) {
                std::shared_ptr<CompressedTextureData> raster = std::move(viewer->m_newMapRasters[k]);
                viewer->m_newMapRasters.erase(k);
                updateTileRaster(k, std::move(raster));
            }
        }

        viewer->setNumTriangles(sceneTriangles());
        viewer->setAllocatedGraphicsBytes(allocatedGraphicsMemoryBytes());
    }
}

int main(int argc, char *argv[])
{    
#if defined(Q_OS_LINUX)
    qputenv("QT_QPA_PLATFORMTHEME", QByteArrayLiteral("gtk3"));
#endif
    qputenv("QT_QUICK_CONTROLS_STYLE", QByteArrayLiteral("Material"));
    qputenv("QT_STYLE_OVERRIDE", QByteArrayLiteral("Material"));
    qputenv("QT_QUICK_CONTROLS_MATERIAL_THEME", QByteArrayLiteral("Dark"));
    qputenv("QT_QUICK_CONTROLS_MATERIAL_PRIMARY", QByteArrayLiteral("#3d3d3d"));
    qputenv("QT_QUICK_CONTROLS_MATERIAL_ACCENT", QByteArrayLiteral("Red"));
    qputenv("QT_QUICK_CONTROLS_MATERIAL_VARIANT", QByteArrayLiteral("Dense")); // ToDo: add setting

//    qputenv("QT_DEBUG_PLUGINS", "1");
    QCoreApplication::setApplicationName(QStringLiteral("QDEMViewer"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("test"));
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);

    QSurfaceFormat fmt;
    fmt.setDepthBufferSize(24);
    fmt.setVersion(4, 5);
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(fmt);

    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;

    qmlRegisterType<TerrainViewer>("DemViewer", 1, 0, "TerrainViewer");
    DEMFetcher *demFetcher = new DEMFetcher(&engine, true);
    demFetcher->setObjectName("DEM Fetcher");
    demFetcher->setURLTemplate(QLatin1String("https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"));
    demFetcher->setMaximumZoomLevel(15);
    demFetcher->setOverzoom(true);
    ArcBall *arcball = new ArcBall(&engine);
    ASTCFetcher *rasterFetcher = new ASTCFetcher(&engine);
    rasterFetcher->setObjectName("Raster Fetcher");
    rasterFetcher->setURLTemplate(QLatin1String("https://tile.openstreetmap.org/{z}/{x}/{y}.png"));
//    rasterFetcher->setURLTemplate(QLatin1String("http://mt1.google.com/vt/lyrs=y&x={x}&y={y}&z={z}")); // hybrid
//    rasterFetcher->setURLTemplate(QLatin1String("http://mt1.google.com/vt/lyrs=s&x={x}&y={y}&z={z}")); // sat only
//    rasterFetcher->setURLTemplate(QLatin1String("https://services.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}"));

    Utilities *utilities = new Utilities(&engine);
    utilities->setLogPath("/tmp/demviewer.log");

    engine.rootContext()->setContextProperty("utilities", utilities);
    engine.rootContext()->setContextProperty("demfetcher", demFetcher);
    engine.rootContext()->setContextProperty("arcball", arcball);
    engine.rootContext()->setContextProperty("mapFetcher", rasterFetcher);

    const QUrl url(QStringLiteral("qrc:/main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);

    qInfo() << "Network cache dir: " << MapFetcher::networkCachePath();
    qInfo() << "Compound tile cache dir: " << MapFetcher::compoundTileCachePath();

    engine.load(url);
    QThread::currentThread()->setObjectName("Main Thread");
    return app.exec();
}

#include "main.moc"
