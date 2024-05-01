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

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <sqlite_adapter.h>

namespace  {
constexpr const quint16 defaultPort{1234};
const QString serverObjectName{"dbserver"};
}

class Server : public QObject
{
Q_OBJECT
public:
    Server(quint16 port = defaultPort, QObject *parent = nullptr)
    : QObject(parent) {
        node.reset(new QRemoteObjectHost(QStringLiteral("tcp://:") + QString::number(port)));
        adapter.reset(new SqliteAdapter);
        adapter->setObjectName(serverObjectName);
        node->enableRemoting(adapter.get()); // enable remoting/sharing
    }
    ~Server() override {

    }

    void serve(const QString &astcCachePath, const QString &networkCachePath) {
        if (!networkCachePath.isEmpty())
            SQLiteManager::initDb(networkCachePath, {});
        else if (!astcCachePath.isEmpty())
            SQLiteManager::initDb(astcCachePath, {});
        else
            qFatal("Server::serve No valid paths provided!");
    }

    QScopedPointer<QRemoteObjectHost> node;
    QScopedPointer<SqliteAdapter> adapter;
};

class Client : public QObject
{
Q_OBJECT
public:
    Client(const QString &host,
           bool network,
           QDateTime ts,
           QString dbPath,
           quint16 port = defaultPort,
           QObject *parent = nullptr)
        : QObject(parent), m_network(network), m_timestamp(ts)
    {
        const QUrl url = QUrl(QStringLiteral("tcp://")
                              + host
                              + QStringLiteral(":")
                              + QString::number(port));
        qInfo() << "Client: connecting to" << url;
        client.reset(new DbClient(url));
        QObject::connect(client.data(), &DbClient::initializedChanged,
                         this, &Client::onInitialized);
        QObject::connect(client.data(), &DbClient::error,
                         this, &Client::onError);
        QObject::connect(client.data(), &DbClient::queryFinished,
                         this, &Client::onQueryFinished);
        if (m_network)
            QObject::connect(client.data(), &DbClient::rowReceived,
                             this, &Client::onNetworkRowReceived);
        else
            QObject::connect(client.data(), &DbClient::rowReceived,
                             this, &Client::onASTCRowReceived);
        SQLiteManager::initDb(dbPath, {});
    }

    ~Client() override {

    }

    void updateASTC()
    {
        if (m_network)
            qFatal("Error: Client configured for updating network cache!");
        if (!m_initialized) {
            qWarning() << "Client not initialized!";
            return;
        }

        static constexpr char queryString[] = R"(
SELECT tileHash, blockX, blockY, quality, width, height, tile, ts, x, y, z
FROM Tile
WHERE ts > :clientmaxts
ORDER BY ts ASC
)";

        Query querySyncOff{"PRAGMA synchronous = OFF",
                    {},
                    "",
                    1};

        Query queryJournalOff{"PRAGMA journal_mode = OFF",
                    {},
                    "",
                    2};

        // disable sync from local cache
        auto res = SQLiteManager::instance().sqliteSelect(querySyncOff.toMap());
        res = SQLiteManager::instance().sqliteSelect(queryJournalOff.toMap());

        static constexpr char clientQuery[] = R"(
SELECT MAX(ts) from Tile
)";
        if (!m_timestamp.isValid()) {
            QVariantMap clientq{{"query" , clientQuery},
                                {"args" , QVariantMap()},
                                {"query_id" , 123}};

            auto res = SQLiteManager::instance().sqliteSelect(clientq);
            m_timestamp =
                    res.value("query_result").toList().first().toMap().value("MAX(ts)").toDateTime();

            if (!m_timestamp.isValid())
                qFatal("Invalid m_timestamp in updateASTC");
        }

        Query query{queryString,
                    QVariantMap{{":clientmaxts", m_timestamp}},
                    "",
                    42};

        client->submitSelect(querySyncOff);
        client->submitSelect(queryJournalOff);
        client->submitSelectProgressive(query);
    }

    void updateNetwork()
    {
        if (!m_network)
            qFatal("Error: Client configured for updating astc cache!");
        if (!m_initialized) {
            qWarning() << "Client not initialized!";
            return;
        }

        static constexpr char queryString[] = R"(
SELECT url, metadata, data, lastAccess
FROM Document
WHERE lastAccess > :clientmaxlastaccess
ORDER BY lastAccess ASC
)";

        Query querySyncOff{"PRAGMA synchronous = OFF",
                    {},
                    "",
                    1};
        Query queryJournalOff{"PRAGMA journal_mode = OFF",
                    {},
                    "",
                    2};

        // disable sync from local cache
        auto res = SQLiteManager::instance().sqliteSelect(querySyncOff.toMap());
        res = SQLiteManager::instance().sqliteSelect(queryJournalOff.toMap());

        static constexpr char clientQuery[] = R"(
SELECT MAX(lastAccess) from Document
)";
        if (!m_timestamp.isValid()) {
            QVariantMap clientq{{"query" , clientQuery},
                                {"args" , QVariantMap()},
                                {"query_id" , 123}};

            auto res = SQLiteManager::instance().sqliteSelect(clientq);
            m_timestamp =
                    res.value("query_result").toList().first().toMap().value("MAX(lastAccess)").toDateTime();

            if (!m_timestamp.isValid())
                qFatal("Invalid m_timestamp in updateNetwork");
        }

        Query query{queryString,
                    QVariantMap{{":clientmaxlastaccess", m_timestamp}},
                    "",
                    42};


        client->submitSelect(querySyncOff);
        client->submitSelect(queryJournalOff);
        client->submitSelectProgressive(query);
    }


public slots:
    void onError(QString error) {
        qWarning() << error;
    }
    void onQueryFinished(const QVariantMap) {
        // TODO
    }
    void onInitialized() {
        m_initialized = true;
        update();
    }

    void update() {
        qDebug() << "Updating";
        if (m_network)
            updateNetwork();
        else
            updateASTC();
    }

    void onNetworkRowReceived(const QVariantMap data) {
        static quint64 receivedNetworkRowsCount = 0;
        const auto row = data["row"].toMap();
        static constexpr char insertQuery[] = R"(
INSERT INTO Document(metadata, data, url, lastAccess) VALUES (:metadata, :data, :url, :lastaccess)
)";
        QVariantMap insertq{{"query" , insertQuery},
                            {"args", QVariantMap{{":metadata", row["metadata"]},
                                                 {":data", row["data"]},
                                                 {":url", row["url"]},
                                                 {":lastaccess", row["lastAccess"]}}},
                            {"query_id" , 123}};

        auto res = SQLiteManager::instance().sqliteSelect(insertq);
        if (!res["error"].isNull())
            qWarning() << "onNetworkRowReceived: " << res["error"];
        if (!(++receivedNetworkRowsCount % 1000))
            qInfo() << "onNetworkRowReceived "<< receivedNetworkRowsCount << " TS: "<<row["lastAccess"].toDateTime().toString();
    }

    void onASTCRowReceived(const QVariantMap data) {
        static quint64 receivedASTCRowsCount = 0;
        const auto row = data["row"].toMap();
        static constexpr char insertQuery[] = R"(
INSERT INTO Tile(tileHash, blockX, blockY, quality, width, height, tile, ts, x, y, z)
VALUES (:tileHash, :blockX, :blockY, :quality, :width, :height, :tile, :ts, :x, :y, :z)
)";
        Query insertq{insertQuery,
                    QVariantMap{{":tileHash", row["tileHash"]},
                                {":blockX", row["blockX"]},
                                {":blockY", row["blockY"]},
                                {":quality", row["quality"]},
                                {":width", row["width"]},
                                {":height", row["height"]},
                                {":tile", row["tile"]},
                                {":ts", row["ts"]},
                                {":x", row["x"]},
                                {":y", row["y"]},
                                {":z", row["z"]}},
                                "",
                                321};

        auto res = SQLiteManager::instance().sqliteSelect(insertq.toMap());
        if (!res["error"].isNull())
            qWarning() << "onASTCRowReceived: " << res["error"];
        if (!(++receivedASTCRowsCount % 1000))
            qInfo() << "onASTCRowReceived "<< receivedASTCRowsCount << " TS: "<<row["ts"].toDateTime().toString();

    }

public:
    QScopedPointer<DbClient> client;
    bool m_initialized{false};
    bool m_network{true};
    QDateTime m_timestamp;
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("MapFetcher Cache Updater");
    QCoreApplication::setApplicationVersion("1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("MapFetcher Cache Updater");
    parser.addHelpOption();
    parser.addVersionOption();

    // A boolean option with a single name (-s)
    QCommandLineOption serverOption("l", "Listen to incoming request retrieving new data");
    parser.addOption(serverOption);

    QCommandLineOption portOption(QStringList() << "p" << "port",
                                    "Port to use for networking");
    parser.addOption(portOption);

    QCommandLineOption hostOption("host", "Connect to host to pull data", "address");
    parser.addOption(hostOption);


    // date -I'seconds' -u -d'now - 1 day' -> 2024-04-29T15:01:34+00:00  -- Qt::ISODate
    QCommandLineOption dateOption("date", "Pull data newer than this timestamp. Format like 2024-04-29T15:01:34+00:00", "date");
    parser.addOption(dateOption);

    // An option with a value
    QCommandLineOption astcCacheOption("astcCache",
                                       "The astc cache sqlite file used", "file");
    parser.addOption(astcCacheOption);

    QCommandLineOption nwCacheOption("networkCache",
                                     "The network cache sqlite file used", "file");
    parser.addOption(nwCacheOption);

    if (!parser.parse(QCoreApplication::arguments())) {
        auto error = parser.errorText();
        const std::string errorString = error.toStdString();
        qFatal(errorString.c_str());
    }
    // Process the actual command line arguments given by the user
    parser.process(app);

    const bool serve = parser.isSet(serverOption);
    const QString astcCachePath = parser.value(astcCacheOption);
    const QString networkCachePath = parser.value(nwCacheOption);

    QFileInfo fiAstcCache(astcCachePath);
    QFileInfo fiNetworkCache(networkCachePath);

    if (   (astcCachePath.isEmpty() && networkCachePath.isEmpty())
        || (!astcCachePath.isEmpty() && !fiAstcCache.exists())
        || (!networkCachePath.isEmpty() && !fiNetworkCache.exists())) {
        qFatal("Invalid database paths. Exiting.");
    }

    if (!astcCachePath.isEmpty() && !networkCachePath.isEmpty()) {
        qWarning() << "Warning: Both astc cache and network cache are provided. Using network cache.";
    }

    QString host;
    if (!serve) {
        host = parser.value(hostOption);
        if (host.isEmpty())
            qFatal("Invalid host. Exiting.");
        else
            qInfo() << "Connecting to "<<host;
    }
    bool ok;
    quint16 port = defaultPort;
    if (parser.isSet(portOption)) {
        qint64 oport = parser.value(portOption).toInt(&ok);
        if (!ok || oport < 1 || oport > 65535) {
            QString fatal = QStringLiteral("Failed to parse port ") + parser.value(portOption);
            auto cfatal = fatal.toStdString();
            qFatal(cfatal.c_str());
        }
        port = quint16(oport);
    }

    QDateTime ts;
    QString timestamp = parser.value(dateOption);
    if (timestamp.size()) {
        ts = QDateTime::fromString(timestamp, Qt::ISODate);
        if (!ts.isValid()) {
            QString fatal = QStringLiteral("Timestamp ") + timestamp + " provided cannot be parsed. exiting.";
            auto cfatal = fatal.toStdString();
            qFatal(cfatal.c_str());
        } else {
            qInfo() << "Requesting data newer than "<<ts;
        }
    }

    qDebug() << fiAstcCache << fiNetworkCache;
    qDebug() << serve << fiAstcCache.absoluteFilePath()
                      << fiNetworkCache.absoluteFilePath();

    QScopedPointer<Server> server;
    QScopedPointer<Client> client;

    QTimer::singleShot(100, [&server,
                             &client,
                             serve,
                             &ts,
                             &fiAstcCache,
                             &fiNetworkCache,
                             &networkCachePath,
                             &host,
                             &port]() {
        if (serve) {
            server.reset(new Server(port));
            server->serve(fiAstcCache.absoluteFilePath(), fiNetworkCache.absoluteFilePath());
        } else {
            client.reset(new Client(host,
                                    !networkCachePath.isEmpty(),
                                    ts,
                                    (!networkCachePath.isEmpty())
                                        ? fiNetworkCache.absoluteFilePath()
                                        : fiAstcCache.absoluteFilePath(),
                                    port
                                    ));
        }
    });
    app.exec();
}

#include "main.moc"
