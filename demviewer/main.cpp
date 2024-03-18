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
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLExtraFunctions>
#include <QOpenGLTexture>
#include <QMatrix4x4>
#include <QQuaternion>
#include <QVector3D>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QEasingCurve>

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
}

float *lightingCurve() {
    static bool initialized{false};
    static float curve[256];
    if (!initialized) {
        QEasingCurve ec(QEasingCurve::InQuad);
        initialized = true;
        for (int i = 0; i < 256; ++i) {
            float pct = float(i) / 255.;
            //curve[i] = ec.valueForProgress(pct);
            curve[i] = 1.0 - ec.valueForProgress(pct);
            curve[i] = 2. * pct - curve[i];
        }
    }

    return curve;
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

    void setDem(std::shared_ptr<Heightmap> dem) const {
        m_dem = std::move(*dem);
        m_resolution = m_dem.size();
    }

    void setMap(std::shared_ptr<QImage> map) const {
        m_map = map->mirrored(false, true);
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


    void init() const
    {
        if (m_initialized)
            return;
        m_initialized = true;
        // vtx
        // texCoords

        // VAOs
        QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();
        QOpenGLVertexArrayObject::Binder vaoBinder(&datalessVao); // creates it when bound the first time
    }

    QSharedPointer<QOpenGLTexture> demTexture() const {
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

    QSharedPointer<QOpenGLTexture> mapTexture() const
    {
        if (!m_map.isNull()) {
            {
                m_texMap.reset(new QOpenGLTexture(QOpenGLTexture::Target2D));
                m_texMap->setMaximumAnisotropy(16);
                m_texMap->setMinMagFilters(QOpenGLTexture::LinearMipMapLinear,
                                           QOpenGLTexture::Linear);
                m_texMap->setWrapMode(QOpenGLTexture::ClampToEdge);
            }
            m_texMap->setData(m_map);
            m_map = QImage();
        }
        return m_texMap;
    }

    void draw(const QMatrix4x4 &transformation,
              const Tile& origin,
              const float elevationScale,
              const float brightness,
              const int tessellationDirection,
              const QVector3D &lightDirection,
              bool interactive,
              bool joinTiles,
              int downsamplingRate) const
    {
        QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();
        if (!m_shader) {
            glEnable(GL_TEXTURE_2D);
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_maxTexSize);
            m_shader = new QOpenGLShaderProgram;
            m_shader->addShaderFromSourceCode(QOpenGLShader::Vertex,
                                              QByteArray(vertexShaderTile));
            m_shader->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                              QByteArray(fragmentShaderTile));
            m_shader->link();

            m_shaderTextured = new QOpenGLShaderProgram;
            m_shaderTextured->addShaderFromSourceCode(QOpenGLShader::Vertex,
                                              QByteArray(vertexShaderTile));
            m_shaderTextured->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                              QByteArray(fragmentShaderTileTextured));
            m_shaderTextured->link();

            m_shaderJoinedDownsampledTextured = new QOpenGLShaderProgram;
            // Create shaders
            m_shaderJoinedDownsampledTextured->addShaderFromSourceCode(QOpenGLShader::Vertex,
                                              QByteArray(vertexShaderTileJoinedDownsampled));
            m_shaderJoinedDownsampledTextured->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                              QByteArray(fragmentShaderTileTextured));
            m_shaderJoinedDownsampledTextured->link();
        }
        if (!m_shader) {
            qWarning() << "Failed creating shader!";
            return;
        }
        if (m_resolution.isEmpty())
            return; // skip drawing the tile

        QOpenGLShaderProgram *shader = (mapTexture()) ? m_shaderTextured
                                                      : m_shader;
        if (interactive && joinTiles)
            shader = m_shaderJoinedDownsampledTextured;

        QOpenGLExtraFunctions *ef = QOpenGLContext::currentContext()->extraFunctions();
        const auto tileMatrix = tileTransformation(origin);
        ef->glBindImageTexture(0, (demTexture()) ? demTexture()->textureId() : 0, 0, 0, 0,  GL_READ_WRITE, GL_RGBA8); // TODO: test readonly

        if (mapTexture())  {
            mapTexture()->bind();
        }
        shader->bind();

        int stride = (interactive) ? downsamplingRate : 1;

        auto resolution = m_resolution;
        if (joinTiles && interactive)
            resolution -= QSize(2,2);
        resolution /= stride;
        shader->setUniformValue("resolution", resolution);

//        if (shader == m_shaderJoinedDownsampledTextured)  {
//            resolution -= QSize(1,1);
//        }


        shader->setUniformValue("elevationScale", elevationScale);
        const QMatrix4x4 m = transformation * tileMatrix;
        shader->setUniformValue("matrix", m);
        shader->setUniformValue("color", QColor(255,255,255,255));
        if (mapTexture()) {
            shader->setUniformValue("raster", 0);
            shader->setUniformValueArray("lightingCurve", lightingCurve(), 256, 1);
        }
        shader->setUniformValue("brightness", brightness);
        shader->setUniformValue("quadSplitDirection", tessellationDirection);
        shader->setUniformValue("lightDirection", lightDirection);
        shader->setUniformValue("cOff", (joinTiles && !interactive) ? -0.5f: 0.5f);
        shader->setUniformValue("joined", int(joinTiles));
        shader->setUniformValue("samplingStride", stride);

        QOpenGLVertexArrayObject::Binder vaoBinder(&datalessVao);
        const int numVertices = totVertices(joinTiles, stride);

        f->glDrawArrays(GL_TRIANGLES, 0, numVertices);
        shader->release();
        if (mapTexture())
            mapTexture()->release();
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
            res += m_texDem->width() * m_texDem->height() * 4;
        if (m_texMap)
            res += (m_texMap->width() * m_texDem->height() * 4) * 1.3333; //mipmapped
        return res;
    }

    // reuse the same every time
    TileKey m_key;
    mutable QSize m_resolution; // GLint bcz somehow setUniformValue is picking the wrong overload otherwise

    mutable bool m_hasBorders{false};
    mutable bool m_initialized{false};
    mutable Heightmap m_dem;
    mutable QSharedPointer<QOpenGLTexture> m_texDem; // To make it easily copyable

    mutable QImage m_map;
    mutable QSharedPointer<QOpenGLTexture> m_texMap;

    mutable std::shared_ptr<Tile> m_right;
    mutable std::shared_ptr<Tile> m_bottom;
    mutable std::shared_ptr<Tile> m_bottomRight;

    static GLint m_maxTexSize;
    static QOpenGLShaderProgram *m_shader;
    static QOpenGLShaderProgram *m_shaderTextured;
    static QOpenGLShaderProgram *m_shaderJoinedDownsampledTextured;
};
QOpenGLShaderProgram *Tile::m_shader{nullptr};
QOpenGLShaderProgram *Tile::m_shaderTextured{nullptr};
QOpenGLShaderProgram *Tile::m_shaderJoinedDownsampledTextured{nullptr};
GLint Tile::m_maxTexSize{0};

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
                          std::shared_ptr<QImage> raster) {

        auto t = m_tiles.find(k);
        if (t == m_tiles.end())
            return;

        t->second->setMap(std::move(raster));
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

    bool hasTile(const TileKey k) const {
        return m_tiles.find(k) != m_tiles.end();
    }

    std::shared_ptr<Tile> tile(const TileKey k) const {
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
        format.setSamples(4);
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
        auto *f45 = QOpenGLContext::currentContext()->versionFunctions<QOpenGLFunctions_4_5_Core>();

        f->glClearColor(0, 0, 0, 0);
        f->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        f->glEnable(GL_DEPTH_TEST);
        f->glDepthFunc(GL_LESS);
        f->glDepthMask(true);


        GLint polygonMode;
        f->glGetIntegerv(GL_POLYGON_MODE, &polygonMode);
        f45->glPolygonMode( GL_FRONT_AND_BACK, polygonMode );
//        QOpenGLExtraFunctions *ef = QOpenGLContext::currentContext()->extraFunctions();
//        f45->glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );


        const bool fast = (m_interactive && m_fastInteraction) || (m_fastInteraction && !m_autoRefinement);

        Origin::draw(m_arcballTransform, 1);
        for (auto &t: m_tiles) {
            t.second->draw(m_arcballTransform,
                   *m_tiles.begin()->second,
                   m_elevationScale,
                   m_brightness,
                   m_tessellationDirection,
                   m_lightDirection,
                   fast,
                   m_joinTiles,
                   m_downsamplingRate);
        }

        f45->glPolygonMode( GL_FRONT_AND_BACK, polygonMode );
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
    Q_PROPERTY(QVariant demUtilities READ getUtilities WRITE setUtilities)
    Q_PROPERTY(QVariant rasterFetcher READ rasterFetcher WRITE setRasterFetcher)
    Q_PROPERTY(qreal elevationScale READ elevationScale WRITE setElevationScale)
    Q_PROPERTY(qreal brightness READ brightness WRITE setBrightness)
    Q_PROPERTY(bool joinTiles READ joinTiles WRITE setJoinTiles)
    Q_PROPERTY(int tessellationDirection READ tessellationDirection WRITE setTessellationDirection)
    Q_PROPERTY(quint64 triangles READ numTriangles NOTIFY numTrianglesChanged)
    Q_PROPERTY(quint64 allocatedGraphicsBytes READ allocatedGraphicsBytes NOTIFY allocatedGraphicsBytesChanged)
    Q_PROPERTY(QVariant lightDirection READ lightDirection WRITE setLightDirection)
    Q_PROPERTY(bool offline READ offline WRITE setOffline NOTIFY offlineChanged)
    Q_PROPERTY(bool fastInteraction READ fastInteraction WRITE setFastInteraction NOTIFY fastInteractionChanged)
    Q_PROPERTY(bool autoRefinement READ autoRefinement WRITE setAutoRefinement NOTIFY autoRefinementChanged)
    Q_PROPERTY(int downsamplingRate READ downsamplingRate WRITE setDownsamplingRate)

public:
    TerrainViewer(QQuickItem *parent = nullptr) {
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

    void setUtilities(QVariant value) {
        if (m_utilities)
            return;
        m_utilities = qobject_cast<DEMFetcher *>(qvariant_cast<QObject *>(value));
        if (m_utilities) {
            QObject::connect(m_utilities, &DEMFetcher::heightmapReady,
                             this, &TerrainViewer::onDtmReady);
            QObject::connect(m_utilities, &DEMFetcher::heightmapCoverageReady,
                             this, &TerrainViewer::onCoverageReady);
            QObject::connect(m_utilities, &MapFetcher::requestHandlingFinished,
                             this, &TerrainViewer::onRequestHandlingFinished);
        }
    }

    QVariant getUtilities() const {
        return QVariant::fromValue(m_utilities);
    }

    void setRasterFetcher(QVariant value) {
        if (m_rasterFetcher)
            return;
        m_rasterFetcher = qobject_cast<MapFetcher *>(qvariant_cast<QObject *>(value));
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
        m_utilities->setBorders(m_joinTiles);
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
    void fastInteractionChanged();
    void autoRefinementChanged();

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
        m_newTiles.emplace(k, m_utilities->heightmap(id, k));
        delayedUpdate();
    }

    void onCoverageReady(const quint64 id) {
        if (!m_utilities)
            return;
        reset();
        m_newTiles.clear();
        m_newTiles.emplace(TileKey{0,0,0},  m_utilities->heightmapCoverage(id));
        delayedUpdate();
    }

    void onMapTileReady(quint64 id, const TileKey k) {
        if (!m_rasterFetcher)
            return;
        m_newMapRasters.emplace(k, m_rasterFetcher->tile(id, k));
        delayedUpdate();
    }

    void onMapCoverageReady(const quint64 id) {
        if (!m_rasterFetcher)
            return;
        m_newMapRasters.clear(); // prb useless
        auto raster = m_rasterFetcher->tileCoverage(id);

        m_newMapRasters.emplace(TileKey{0,0,0},  std::move(raster));
        delayedUpdate();
    }

    void onRequestHandlingFinished(quint64 id) {
        qInfo() << "Request "<<id<< " finished.";
    }


private:
    ArcBall *m_arcball{nullptr};
    DEMFetcher *m_utilities{nullptr};
    MapFetcher *m_rasterFetcher{nullptr};
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
    std::map<TileKey, std::shared_ptr<QImage>> m_newMapRasters;

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
            if (hasTile(k)) {
                std::shared_ptr<QImage> raster = std::move(viewer->m_newMapRasters[k]);
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

#ifdef Q_OS_LINUX
    qint64 pid = QCoreApplication::applicationPid();
    QProcess process;
    process.setProgram(QLatin1String("prlimit"));
    process.setArguments(QStringList() << "--pid" << QString::number(pid) << "--nofile=1048576");
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    if (!process.startDetached(&pid)) {
        QLoggingCategory category("qmldebug");
        qCInfo(category) << QCoreApplication::applicationName()
                         <<": failed prlimit";
    }
#endif

    QGuiApplication app(argc, argv);

    QQmlApplicationEngine engine;

    qmlRegisterType<TerrainViewer>("DemViewer", 1, 0, "TerrainViewer");
    DEMFetcher *utilities = new DEMFetcher(&engine, true);
    utilities->setObjectName("DEM Fetcher");
    utilities->setURLTemplate(QLatin1String("https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"));
    ArcBall *arcball = new ArcBall(&engine);
    MapFetcher *rasterFetcher = new MapFetcher(&engine);
    rasterFetcher->setObjectName("Raster Fetcher");
    rasterFetcher->setURLTemplate(QLatin1String("https://tile.openstreetmap.org/{z}/{x}/{y}.png"));
//    rasterFetcher->setURLTemplate(QLatin1String("http://mt1.google.com/vt/lyrs=y&x={x}&y={y}&z={z}")); // hybrid
//    rasterFetcher->setURLTemplate(QLatin1String("http://mt1.google.com/vt/lyrs=s&x={x}&y={y}&z={z}")); // sat only
//    rasterFetcher->setURLTemplate(QLatin1String("https://services.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}"));


    engine.rootContext()->setContextProperty("utilities", utilities);
    engine.rootContext()->setContextProperty("arcball", arcball);
    engine.rootContext()->setContextProperty("mapFetcher", rasterFetcher);

    const QUrl url(QStringLiteral("qrc:/main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);

    qDebug() << "Network cache dir: " << MapFetcher::cachePath();

    engine.load(url);
    QThread::currentThread()->setObjectName("Main Thread");
    return app.exec();
}

#include "main.moc"
