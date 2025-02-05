#include <QGuiApplication>
#include <QQmlApplicationEngine>

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
#include <QtPositioning/private/qlocationutils_p.h>
#include <QtLocation/private/qgeocameratiles_p_p.h>
#include <QtLocation/private/qdeclarativegeomap_p.h>
#include <QtLocation/private/qdeclarativepolygonmapitem_p.h>
#include <QtLocation/private/qgeomap_p.h>
#include <QtLocation/private/qgeoprojection_p.h>
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
#include <array>
#include <QMap>
#include <QSaveFile>
#include <math.h>
#include <qmath.h>

#include "mapfetcher.h"

class Utilities : public QObject {
    Q_OBJECT
public:
    struct Coverages {
        std::shared_ptr<QImage> raster;
        std::shared_ptr<Heightmap> heightmap;
    };

    struct RequestID {
        quint64 rasterID;
        quint64 demID;

        bool operator<(const RequestID& other) const {
            if (rasterID != other.rasterID) {
                return rasterID < other.rasterID;
            }
            return demID < other.demID;
        }
    };

    Utilities(DEMFetcher *demFetcher,
              MapFetcher *rasterFetcher,
              QObject *parent=nullptr)
    : QObject(parent),
      m_demFetcher(demFetcher),
      m_rasterFetcher(rasterFetcher)
    {
        if (!m_demFetcher || !m_rasterFetcher)
            qFatal("Map and Terrain fetchers are NULL!");

        QObject::connect(m_demFetcher, &DEMFetcher::heightmapCoverageReady,
                         this, &Utilities::onDTMCoverageReady);
        QObject::connect(m_demFetcher, &MapFetcher::requestHandlingFinished,
                         this, &Utilities::onRequestHandlingFinished);

        QObject::connect(m_rasterFetcher, &MapFetcher::coverageReady,
                         this, &Utilities::onMapCoverageReady);
        QObject::connect(m_rasterFetcher, &MapFetcher::requestHandlingFinished,
                         this, &Utilities::onRequestHandlingFinished);

        m_rasterFetcher->setOverzoom(true);
        m_rasterFetcher->setMaximumZoomLevel(22);
        m_demFetcher->setOverzoom(true);
        m_demFetcher->setMaximumZoomLevel(15);
    };

    ~Utilities() override {}


    Q_INVOKABLE void setURLTemplate(const QString &t) {
        m_rasterFetcher->setURLTemplate(t);
    }

    Q_INVOKABLE void download(const QString downloadDirectory,
                              const QList<QGeoCoordinate> &selectionPolygon,
                              const quint8 demZoom,
                              const quint8 mapZoom) {
        auto demID = m_demFetcher->requestCoverage(selectionPolygon,
                                                 demZoom,
                                                 true);

        auto rasterID = m_rasterFetcher->requestCoverage(selectionPolygon,
                                              mapZoom,
                                              true);

        RequestID requestID{rasterID, demID};
        m_rasterIDs[rasterID] = requestID;
        m_demIDs[demID] = requestID;
        m_destination[requestID] = downloadDirectory;
        m_numResponses[requestID] = 0;
    }

protected slots:
    void onDTMCoverageReady(const quint64 id) {
        if (!m_demFetcher)
            return;

        auto heightmap = m_demFetcher->heightmapCoverage(id);
        auto requestID = m_demIDs[id];
        m_coverage[requestID].heightmap = std::move(heightmap);
        m_numResponses[requestID]++;
        finalizeRequest(requestID);
    }

    void onMapCoverageReady(const quint64 id) {
        if (!m_rasterFetcher)
            return;

        auto raster = m_rasterFetcher->tileCoverage(id);
        auto requestID = m_rasterIDs[id];
        m_coverage[requestID].raster = std::move(raster);
        m_numResponses[requestID]++;
        finalizeRequest(requestID);
    }

    void onRequestHandlingFinished(quint64 id) {
        qInfo() << "Request "<<id<< " finished. sender: "<<sender();
    }

protected:
    void finalizeRequest(RequestID id) {
        if (m_numResponses[id] < 2)
            return;
        QString msg;
        auto dst = m_destination[id];
        if (dst.startsWith("file://"))
#if defined(Q_OS_WINDOWS)
            dst = dst.mid(8);
#else
            dst = dst.mid(7);
#endif
        if (!QDir("/").mkpath(dst)) {
            msg = "Failed creating path to store coverages at " + dst;
            qFatal("%s", msg.toStdString().c_str());
        }

        auto res = m_coverage[id].raster->mirrored(false, true).save(dst + "/raster.png" );

        if (!res) {
            msg = "failed to save " + dst + "/raster.png";
            qFatal("%s", msg.toStdString().c_str());
        }

        // https://stackoverflow.com/a/2774014/962856
        auto &h = m_coverage[id].heightmap;
        QByteArray dem(reinterpret_cast<const char*>(&h->elevations.front()),
                       sizeof(float) * m_coverage[id].heightmap->elevations.size());
        QString demDst = dst + "/dem_" +
                QString::number(h->size().width()) + "x" +
                QString::number(h->size().height()) + ".bin";
        QSaveFile demFile(demDst);
        res = demFile.open(QIODevice::WriteOnly);
        if (!res) {
            msg = "failed to save " + demDst;
            qFatal("%s", msg.toStdString().c_str());
        }
        res = demFile.write(dem);
        if (!res) {
            msg = "failed to save " + demDst;
            qFatal("%s", msg.toStdString().c_str());
        }
        res = demFile.commit();
        if (!res) {
            msg = "failed to save " + demDst;
            qFatal("%s", msg.toStdString().c_str());
        }

        m_coverage.remove(id);
        m_destination.remove(id);
        m_numResponses.remove(id);
        m_demIDs.remove(id.demID);
        m_rasterIDs.remove(id.rasterID);
    }

    QPointer<QQuickWindow> m_window;
    DEMFetcher *m_demFetcher;
    MapFetcher *m_rasterFetcher;

    QMap<quint64, RequestID> m_rasterIDs;
    QMap<quint64, RequestID> m_demIDs;
    QMap<RequestID, quint64> m_numResponses;
    QMap<RequestID, QString> m_destination;
    QMap<RequestID, Coverages> m_coverage;
};



int main(int argc, char *argv[])
{
    qInfo() << "downloader starting ...";
    bool windows = true;
#if defined(Q_OS_LINUX)
    qputenv("QT_QPA_PLATFORMTHEME", QByteArrayLiteral("gtk3"));

    qputenv("QT_QUICK_CONTROLS_STYLE", QByteArrayLiteral("Material"));
    qputenv("QT_STYLE_OVERRIDE", QByteArrayLiteral("Material"));
    qputenv("QT_QUICK_CONTROLS_MATERIAL_THEME", QByteArrayLiteral("Dark"));
    qputenv("QT_QUICK_CONTROLS_MATERIAL_PRIMARY", QByteArrayLiteral("#3d3d3d"));
    qputenv("QT_QUICK_CONTROLS_MATERIAL_ACCENT", QByteArrayLiteral("Red"));
    qputenv("QT_QUICK_CONTROLS_MATERIAL_VARIANT", QByteArrayLiteral("Dense")); // ToDo: add setting
    windows = false;
#endif


    QCoreApplication::setApplicationName(QStringLiteral("CoverageDownloader"));
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

    DEMFetcher *demFetcher = new DEMFetcher(&engine, true);
    demFetcher->setObjectName("DEM Fetcher");
    demFetcher->setURLTemplate(QLatin1String("https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"));
    demFetcher->setMaximumZoomLevel(15);
    demFetcher->setOverzoom(true);

    MapFetcher *rasterFetcher = new MapFetcher(&engine);
    rasterFetcher->setObjectName("Raster Fetcher");
    rasterFetcher->setURLTemplate(QLatin1String("https://tile.openstreetmap.org/{z}/{x}/{y}.png"));
//    rasterFetcher->setURLTemplate(QLatin1String("http://mt[0,1,2,3].google.com/vt/lyrs=y&x={x}&y={y}&z={z}")); // hybrid
//    rasterFetcher->setURLTemplate(QLatin1String("http://mt[0,1,2,3].google.com/vt/lyrs=s&x={x}&y={y}&z={z}")); // sat only
//    rasterFetcher->setURLTemplate(QLatin1String("https://services.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}"));


    engine.rootContext()->setContextProperty("demfetcher", demFetcher);
    engine.rootContext()->setContextProperty("mapFetcher", rasterFetcher);
    engine.rootContext()->setContextProperty("windows", windows);

    Utilities *utilities = new Utilities(demFetcher, rasterFetcher, &engine);
    engine.rootContext()->setContextProperty("utilities", utilities);

    qInfo() << "Network cache dir: " << MapFetcher::networkCachePath();
    qInfo() << "Compound tile cache dir: " << MapFetcher::compoundTileCachePath();

    const QUrl url(QStringLiteral("qrc:/main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);
    engine.load(url);
    return app.exec();
}

#include "main.moc"
