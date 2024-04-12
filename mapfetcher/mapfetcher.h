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

#ifndef TILEFETCHER_H
#define TILEFETCHER_H

#include <QImage>
#include <QtPositioning/QGeoCoordinate>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QOpenGLTexture>
#include <QMutex>
#include <QThread>
#include <QDebug>
#include <set>
#include <tuple>
#include <math.h>
#include <algorithm>
#include <unordered_map>
#include <private/qtexturefilereader_p.h>

struct TileKey {
    quint64 x;
    quint64 y;
    quint8 z;
    constexpr inline TileKey(quint64 x_, quint64 y_, quint8 zoom_) noexcept;
    TileKey(const TileKey &o) = default;
    TileKey() = default; // Unfortunately needed for the qmetatype system
    ~TileKey() = default;

    bool operator==(const TileKey& o) const;

    bool operator<(const TileKey& o) const;
    bool operator>(const TileKey& o) const;

    friend inline constexpr const TileKey operator+(const TileKey &, const TileKey &) noexcept;
};
constexpr inline TileKey::TileKey(quint64 x_, quint64 y_, quint8 zoom_) noexcept : x(x_), y(y_), z(zoom_) {}
constexpr inline const TileKey operator+(const TileKey & s1, const TileKey & s2) noexcept
{ return TileKey(s1.x+s2.x, s1.y+s2.y, s1.z); }

namespace std {
template <>
struct hash<TileKey> {
    size_t operator()(const TileKey& id) const;
};
} // namespace std

QDebug operator<<(QDebug d, const  TileKey &k);

struct TileData {
    TileKey k;
    QImage img;

    bool operator<(const TileData& o) const;
};

struct NetworkConfiguration {
    static QAtomicInt offline;
    static QAtomicInt astcEnabled;
    static QAtomicInt logNetworkRequests;
};

struct Heightmap {
    enum Neighbor {
        Top = 1 << 0,
        Bottom = 1 << 1,
        Left = 1 << 2,
        Right = 1 << 3,
        TopLeft = 1 << 4,
        TopRight = 1 << 5,
        BottomLeft = 1 << 6,
        BottomRight = 1 << 7
    };
    Q_DECLARE_FLAGS(Neighbors, Neighbor)

    static Heightmap fromImage(const QImage &dem,
                               const std::map<Neighbor, std::shared_ptr<QImage> > &borders = {});

    void rescale(QSize size);
    void rescale(int size);
    void setSize(QSize size, float initialValue = .0f);
    QSize size() const;

    float elevation(int x, int y) const;
    void setElevation(int x, int y, float e);

    QSize m_size;
    std::vector<float> elevations;
    bool m_hasBorders{false};
};
Q_DECLARE_OPERATORS_FOR_FLAGS(Heightmap::Neighbors)

struct CompressedTextureData {
    CompressedTextureData() = default;
    virtual ~CompressedTextureData() = default;

    virtual quint64 upload(QSharedPointer<QOpenGLTexture> &) = 0;
    virtual quint64 uploadTo2DArray(QSharedPointer<QOpenGLTexture> &texArray,
                                              int layer,
                                              int layers) = 0;
    virtual QSize size() const = 0;
    virtual bool hasCompressedData() const = 0;

    static bool isFormatCompressed(GLint format) {
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
};

class MapFetcherPrivate;
class MapFetcher : public QObject {
    Q_DECLARE_PRIVATE(MapFetcher)
    Q_OBJECT

    Q_PROPERTY(QString urlTemplate READ urlTemplate WRITE setURLTemplate NOTIFY urlTemplateChanged)
    Q_PROPERTY(int maximumZoomLevel
                   READ maximumZoomLevel
                   WRITE setMaximumZoomLevel
                   NOTIFY maximumZoomLevelChanged)
    Q_PROPERTY(bool overzoom
                   READ overzoom
                   WRITE setOverzoom
                   NOTIFY overzoomChanged)

public:
    MapFetcher(QObject *parent);
    ~MapFetcher() override = default;

    // destinationZoom is the zoom level of the physical geometry
    // onto which the tile is used as texture.
    // This API allows to fetch a different (larger) ZL and assemble it
    // into an image that matches the destinationZoom
    Q_INVOKABLE quint64 requestSlippyTiles(const QList<QGeoCoordinate> &crds,
                                        const quint8 zoom,
                                        quint8 destinationZoom);

    Q_INVOKABLE quint64 requestCoverage(const QList<QGeoCoordinate> &crds,
                                        const quint8 zoom,
                                        const bool clip = false);

    std::shared_ptr<QImage> tile(quint64 id, const TileKey k);
    std::shared_ptr<QImage> tileCoverage(quint64 id);

    void setURLTemplate(const QString &urlTemplate);
    QString urlTemplate() const;

    void setMaximumZoomLevel(int maxzl);
    int maximumZoomLevel() const;

    void setOverzoom(bool enabled);
    int overzoom() const;

    static quint8 zoomForCoverage(const QList<QGeoCoordinate> &crds,
                                  const size_t tileResolution,
                                  const size_t maxCoverageResolution,
                                  bool rectangular = false);

    static quint64 networkCacheSize();
    static QString networkCachePath();
    static QString compoundTileCachePath();
    static quint64 compoundTileCacheSize();

signals:
    void tileReady(quint64 id, const TileKey k);
    void progress(quint64 id, QPair<quint64, quint64> operations);
    void coverageReady(quint64 id);
    void urlTemplateChanged();
    void requestHandlingFinished(quint64 id);
    void maximumZoomLevelChanged();
    void overzoomChanged();

protected slots:
    virtual void onInsertTile(const quint64 id, const TileKey k, std::shared_ptr<QImage> i);
    void onInsertCoverage(const quint64 id, std::shared_ptr<QImage> i);

protected:
    MapFetcher(MapFetcherPrivate &dd, QObject *parent = nullptr);

private:
    Q_DISABLE_COPY(MapFetcher)

friend class TileReplyHandler;
friend class NetworkIOManager;
};

class DEMFetcherPrivate;
class DEMFetcher : public MapFetcher {
    Q_DECLARE_PRIVATE(DEMFetcher)
    Q_OBJECT

public:
    DEMFetcher(QObject *parent, bool borders=false);
    ~DEMFetcher() override = default;

    std::shared_ptr<Heightmap> heightmap(quint64 id, const TileKey k);
    std::shared_ptr<Heightmap> heightmapCoverage(quint64 id);

    void setBorders(bool borders);

signals:
    void heightmapReady(quint64 id, const TileKey k);
    void heightmapCoverageReady(quint64 id);

protected slots:
    void onInsertHeightmap(quint64 id, const TileKey k, std::shared_ptr<Heightmap> h);
    void onInsertHeightmapCoverage(quint64 id, std::shared_ptr<Heightmap> h);

protected:
    DEMFetcher(DEMFetcherPrivate &dd, QObject *parent = nullptr);

private:
    Q_DISABLE_COPY(DEMFetcher)
friend class DEMReadyHandler;
friend class NetworkIOManager;
};

class ASTCFetcherPrivate;
class ASTCFetcher : public MapFetcher {
    Q_DECLARE_PRIVATE(ASTCFetcher)
    Q_OBJECT

    Q_PROPERTY(bool forwardUncompressedTiles
               READ forwardUncompressedTiles
               WRITE setForwardUncompressedTiles
               NOTIFY forwardUncompressedTilesChanged)
public:
    ASTCFetcher(QObject *parent);
    ~ASTCFetcher() override = default;

    std::shared_ptr<CompressedTextureData> tile(quint64 id, const TileKey k);
    std::shared_ptr<CompressedTextureData> tileCoverage(quint64 id);

    void setForwardUncompressedTiles(bool enabled);
    const QAtomicInt &forwardUncompressedTiles() const;

signals:
    void forwardUncompressedTilesChanged(bool enabled);

protected slots:
    void onInsertTile(const quint64 id, const TileKey k, std::shared_ptr<QImage> i) override;
    void onInsertASTCTile(const quint64 id,
                          const TileKey k,
                          std::shared_ptr<CompressedTextureData> i);
    void onInsertASTCCoverage(const quint64 id,
                              std::shared_ptr<CompressedTextureData> i);

protected:
    ASTCFetcher(ASTCFetcherPrivate &dd, QObject *parent = nullptr);

private:
    Q_DISABLE_COPY(ASTCFetcher)
friend class Raster2ASTCHandler;
friend class NetworkIOManager;
};

Q_DECLARE_METATYPE(TileKey)
Q_DECLARE_METATYPE(std::shared_ptr<QImage>)
Q_DECLARE_METATYPE(std::shared_ptr<QByteArray>)
Q_DECLARE_METATYPE(std::shared_ptr<CompressedTextureData>)
Q_DECLARE_METATYPE(std::shared_ptr<Heightmap>)

#endif
