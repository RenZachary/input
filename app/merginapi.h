#ifndef MERGINAPI_H
#define MERGINAPI_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QEventLoop>
#include <memory>
#include <QFile>
#include <QFileInfo>

#include "merginapistatus.h"
#include "merginprojectmetadata.h"
#include "localprojectsmanager.h"


#include <QPointer>


struct ProjectDiff
{
  // changes that should be pushed (uploaded)
  QSet<QString> localAdded;
  QSet<QString> localUpdated;
  QSet<QString> localDeleted;

  // changes that should be pulled (downloaded)
  QSet<QString> remoteAdded;
  QSet<QString> remoteUpdated;
  QSet<QString> remoteDeleted;

  // to resolve conflict: we make a copy of the file
  QSet<QString> conflictRemoteUpdatedLocalUpdated;
  QSet<QString> conflictRemoteAddedLocalAdded;

  // to resolve conflict: we keep the updated version
  QSet<QString> conflictRemoteDeletedLocalUpdated;
  QSet<QString> conflictRemoteUpdatedLocalDeleted;

  // TODO: non-conflicting changes? R-A/L-A, R-U/L-U, R-D/L-D

  QString dump() const
  {
    QStringList lines;
    lines << "--- project diff ---";
    lines << QString( "local: %1 added, %2 updated, %3 deleted" )
          .arg( localAdded.count() )
          .arg( localUpdated.count() )
          .arg( localDeleted.count() );
    lines << QString( "remote: %1 added, %2 updated, %3 deleted" )
          .arg( remoteAdded.count() )
          .arg( remoteUpdated.count() )
          .arg( remoteDeleted.count() );
    lines << QString( "conflicts: %1 RU-LU, %2 RA-LA, %3 RD-LU, %4 RU-LD" )
          .arg( conflictRemoteUpdatedLocalUpdated.count() )
          .arg( conflictRemoteAddedLocalAdded.count() )
          .arg( conflictRemoteDeletedLocalUpdated.count() )
          .arg( conflictRemoteUpdatedLocalDeleted.count() );
    return lines.join( "\n" );
  }
};


struct TransactionStatus
{
  qreal totalSize = 0;     //!< total size (in bytes) of files to be uploaded or downloaded
  int transferedSize = 0;  //!< size (in bytes) of amount of data transferred so far
  QString transactionUUID; //!< only for upload. Initially dummy non-empty string, after server confirms a valid UUID, on finish/cancel it is empty

  // download replies
  QPointer<QNetworkReply> replyProjectInfo;
  QPointer<QNetworkReply> replyDownloadFile;

  // upload replies
  QPointer<QNetworkReply> replyUploadProjectInfo;
  QPointer<QNetworkReply> replyUploadStart;
  QPointer<QNetworkReply> replyUploadFile;
  QPointer<QNetworkReply> replyUploadFinish;

  QList<MerginFile> files; // either to upload or download

  QString projectDir;
  QByteArray projectMetadata;  //!< metadata of the new project (not parsed)
  bool firstTimeDownload = false;   //!< only for update. whether this is first time to download the project (on failure we would also remove the project folder)

  int version = -1;  //!< version to which we are updating / the version which we have uploaded

  ProjectDiff diff;
};


struct MerginProjectListEntry
{
  bool isValid() const { return !projectName.isEmpty() && !projectNamespace.isEmpty(); }

  QString projectName;
  QString projectNamespace;
  int version = -1;

  int creator; // server-side user ID of the project owner (creator)
  QList<int> writers; // server-side user IDs of users having write access to the project
  QDateTime serverUpdated; // available latest version of project files on server

};

typedef QList<MerginProjectListEntry> MerginProjectList;


class MerginApi: public QObject
{
    Q_OBJECT
    Q_PROPERTY( QString username READ username NOTIFY authChanged )
    Q_PROPERTY( int userId READ userId NOTIFY authChanged )
    Q_PROPERTY( int storageLimit READ storageLimit NOTIFY userInfoChanged )
    Q_PROPERTY( int diskUsage READ diskUsage NOTIFY userInfoChanged )
    Q_PROPERTY( QString apiRoot READ apiRoot WRITE setApiRoot NOTIFY apiRootChanged )
    Q_PROPERTY( int apiVersionStatus READ apiVersionStatus NOTIFY apiVersionStatusChanged )
  public:
    explicit MerginApi( LocalProjectsManager &localProjects, QObject *parent = nullptr );
    ~MerginApi() = default;

    /**
     * Returns path of the local directory in which all projects are stored.
     * Each project is one sub-directory.
     * \note returns the directory without a trailing slash
     */
    QString projectsPath() const { return mDataDir; }

    //! Returns reference to the cache of local projects
    LocalProjectsManager &localProjectsManager() const { return mLocalProjects; }

    /**
     * Sends non-blocking GET request to the server to listProjects. On listProjectsReplyFinished,
     * when a response is received, parses projects json and sets mMerginProjects.
     * Eventually emits listProjectsFinished on which ProjectPanel (qml component) updates content.
     * \param searchExpression Search filter on projects name.
     * \param user Mergin username used with flag
     * \param flag If defined, it is used to filter out projects tagged as 'created' or 'shared' with given username
     * \param withFilter If true, applies "input" tag in request.
     */
    Q_INVOKABLE void listProjects( const QString &searchExpression = QStringLiteral(), const QString &user = QStringLiteral(),
                                   const QString &flag = QStringLiteral(), const QString &filterTag = QStringLiteral( "input_use" ) );

    /**
     * Sends non-blocking POST request to the server to download/update a project with a given name. On downloadProjectReplyFinished,
     * when a response is received, parses data-stream to files and rewrites local files with them. Extra files which don't match server
     * files are removed. Eventually emits syncProjectFinished on which MerginProjectModel updates status of the project item.
     * If update has been successful, updates metadata file of the project.
     * Emits also notify signal with a message for the GUI.
     * \param projectNamespace Project's namespace used in request.
     * \param projectName  Project's name used in request.
     */
    Q_INVOKABLE void updateProject( const QString &projectNamespace, const QString &projectName );

    /**
     * Sends non-blocking POST request to the server to upload changes in a project with a given name.
     * Firstly updateProject is triggered to fetch new changes. If it was successful, sends update post request with list of local changes
     * and modified/newly added files in JSON. Eventually emits syncProjectFinished on which MerginProjectModel updates status of the project item.
     * Emits also notify signal with a message for the GUI.
     * \param projectNamespace Project's namespace used in request.
     * \param projectName  Project's name used in request.
     */
    Q_INVOKABLE void uploadProject( const QString &projectNamespace, const QString &projectName );

    /**
     * Sends non-blocking POST request to the server to cancel uploading of a project with a given name.
     * If upload has not started yet and a client is waiting for transaction UUID, it cancels the procedure just on client side
     * without sending cancel request to the server.
     * \param projectFullName Project's full name to cancel its upload
     */
    Q_INVOKABLE void uploadCancel( const QString &projectFullName );

    /**
     * To cancel update (1) either before downloading data started - all data related to the download are cleaned;
     * or (2) either when data transfer has begun - connections are aborted followed by calling function once again
     * to clean related data as in the case 1.;
     * \param projectFullName Project's full name to cancel its update
     */
    Q_INVOKABLE void updateCancel( const QString &projectFullName );

    /**
    * Currently no auth service is used, only "username:password" is encoded and asign to mToken.
    * \param username Login user name to Mergin - either username or registered email
    * \param password Password to given username to log in to Mergin
    */
    Q_INVOKABLE void authorize( const QString &login, const QString &password );
    Q_INVOKABLE void getUserInfo( const QString &username );
    Q_INVOKABLE void clearAuth();
    Q_INVOKABLE void resetApiRoot();
    Q_INVOKABLE bool hasAuthData();
    /**
    * Pings Mergin server and checks its version with required version defined in version.pri
    * Accordingly sets mApiVersionStatus variable (reset when mergin url is changed).
    * The function is skipped if version has been checked and passed.
    */
    Q_INVOKABLE void pingMergin();


    LocalProjectInfo getLocalProject( const QString &projectFullName );

    static const int MERGIN_API_VERSION_MAJOR = 2019;
    static const int MERGIN_API_VERSION_MINOR = 4;
    static const QString sMetadataFile;

    static QString defaultApiRoot() { return "https://public.cloudmergin.com/"; }

    /**
    * Finds project in merginProjects list according its full name.
    * \param projectPath Full path to project's folder
    * \param metadataFile Relative path of metafile to project's folder
    */
    static QString getFullProjectName( QString projectNamespace, QString projectName );

    // Test functions
    /**
    * Creates an empty project on Mergin server.
    * \param projectNamespace
    * \param projectName
    */
    void createProject( const QString &projectNamespace, const QString &projectName );
    /**
    * Deletes the project of given namespace and name on Mergin server.
    * \param projectNamespace
    * \param projectName
    */
    void deleteProject( const QString &projectNamespace, const QString &projectName );

    void clearTokenData();

    // Production and Test functions (therefore not private)

    /**
     * Compares project files from three sources:
     * - "old" server version (what was downloaded from server) - read from the project directory's stored metadata
     * - "new" server version (what is currently available on server) - newly fetched from the server
     * - local file version (what is currently in the project directory) - created on the fly from the local directory content
     *
     * The function assigns each of the files to the kind of change that happened to it.
     * Files that have not been changed are not present in the final diff.
     *
     * Without the three sources it is possible to miss some of the updates that need to be handled (e.g. conflicts)
     */
    static ProjectDiff compareProjectFiles( const QList<MerginFile> &oldServerFiles, const QList<MerginFile> &newServerFiles, const QList<MerginFile> &localFiles );

    //! Returns the most recent list of projects fetched from the server
    MerginProjectList projects();

    QList<MerginFile> getLocalProjectFiles( const QString &projectPath );

    QString username() const;

    QString apiRoot() const;
    void setApiRoot( const QString &apiRoot );

    //! Disk usage of current logged in user in Mergin instance in Bytes
    int diskUsage() const;

    //! Total storage limit of current logged in user in Mergin instance in Bytes
    int storageLimit() const;

    int userId() const;
    void setUserId( int userId );

    MerginApiStatus::VersionStatus apiVersionStatus() const;
    void setApiVersionStatus( const MerginApiStatus::VersionStatus &apiVersionStatus );

    //! Returns details about currently active transactions (both download and upload). Useful for tests
    QHash<QString, TransactionStatus> transactions() const { return mTransactionalStatus; }

    static bool isInIgnore( const QFileInfo &info );

  signals:
    void listProjectsFinished( const MerginProjectList &merginProjects );
    void listProjectsFailed();
    void syncProjectFinished( const QString &projectDir, const QString &projectFullName, bool successfully = true );
    /**
     * Emitted when sync starts/finishes or the progress changes - useful to give a clue in the GUI about the status.
     * Normally progress is in interval [0, 1] as data get uploaded or downloaded.
     * With no pending sync, progress is set to -1
     */
    void syncProjectStatusChanged( const QString &projectFullName, qreal progress );
    void reloadProject( const QString &projectDir );
    void networkErrorOccurred( const QString &message, const QString &additionalInfo, bool showAsDialog = false );
    void notify( const QString &message );
    void authRequested();
    void authChanged();
    void authFailed();
    void apiRootChanged();
    void apiVersionStatusChanged();
    void projectCreated( const QString &projectName, bool result );
    void serverProjectDeleted( const QString &projecFullName, bool result );
    void userInfoChanged();
    void pingMerginFinished( const QString &apiVersion, const QString &msg );
    void pullFilesStarted();
    //! Emitted when started to upload chunks (useful for unit testing)
    void pushFilesStarted();

  private slots:
    void listProjectsReplyFinished();

    // Pull slots
    void updateInfoReplyFinished();
    void continueDownloadFiles( const QString &projectName, const QString &version, int chunkNo = 0 );
    void downloadFileReplyFinished();

    // Push slots
    void uploadStartReplyFinished();
    void uploadInfoReplyFinished();
    void uploadFileReplyFinished();
    void uploadFinishReplyFinished();
    void uploadCancelReplyFinished();
    void continueWithUpload( const QString &projectDir, const QString &projectName, bool successfully = true );

    void getUserInfoFinished();
    void saveAuthData();
    void createProjectFinished();
    void deleteProjectFinished();
    void authorizeFinished();
    void pingMerginReplyFinished();
    void copyTempFilesToProject( const QString &projectDir, const QString &projectFullName );

  private:
    MerginProjectList parseListProjectsMetadata( const QByteArray &data );
    static QStringList generateChunkIdsForSize( qint64 fileSize );
    QJsonArray prepareUploadChangesJSON( const QList<MerginFile> &files );

    /**
     * Sends non-blocking GET request to the server to download a file (chunk).
     * \param projectFullName Namespace/name
     * \param filename Name of file to be downloaded
     * \param version version of file to be downloaded
     * \param chunkNo Chunk number of given file to be downloaded
     */
    void downloadFile( const QString &projectFullName, const QString &filename, const QString &version, int chunkNo = 0 );

    /**
     * Sends non-blocking POST request to the server to upload a file (chunk).
     * \param projectFullName Namespace/name
     * \param json project info containing metadata for upload
     */
    void uploadStart( const QString &projectFullName, const QByteArray &json );

    /**
     * Sends non-blocking POST request to the server to upload a file (chunk).
     * \param projectFullName Namespace/name
     * \param transactionUUID Transaction ID which servers sends on uploadStart
     * \param file Mergin file to upload
     * \param chunkNo Chunk number of given file to be uploaded
     */
    void uploadFile( const QString &projectFullName, const QString &transactionUUID, MerginFile file, int chunkNo = 0 );

    /**
     * Closing request after successful upload.
     * \param projectFullName Namespace/name
     * \param transactionUUID transaction UUID to match upload process on the server
     */
    void uploadFinish( const QString &projectFullName, const QString &transactionUUID );

    void sendUploadCancelRequest( const QString &projectFullName, const QString &transactionUUID );

    bool writeData( const QByteArray &data, const QString &path );
    void handleOctetStream( const QByteArray &data, const QString &projectDir, const QString &filename, bool closeFile, bool overwrite );
    bool saveFile( const QByteArray &data, QFile &file, bool closeFile, bool overwrite = false );
    void createPathIfNotExists( const QString &filePath );
    void createEmptyFile( const QString &path );
    void takeFirstAndDownload( const QString &projectFullName, const QString &version );

    static QByteArray getChecksum( const QString &filePath );
    QSet<QString> listFiles( const QString &projectPath );

    void loadAuthData();
    bool validateAuthAndContinute();
    void checkMerginVersion( QString apiVersion, QString msg = QStringLiteral() );
    /**
    * Sets projectNamespace and projectName from sourceString - url or any string from which takes last (name)
    * and the previous of last (namespace) substring after splitting sourceString with slash.
    * \param sourceString QString either url or fullname of a project
    * \param projectNamespace QString to be set as namespace, might not change original value
    * \param projectName QString to be set to name of a project
    */
    bool extractProjectName( const QString &sourceString, QString &projectNamespace, QString &projectName );
    /**
    * Extracts detail (message) of an error json. If its not json or detail cannot be parsed, the whole data are return;
    * \param data Data received from mergin server on a request failed.
    */
    QString extractServerErrorMsg( const QByteArray &data );
    /**
    * Returns a temporary project path.
    * \param projectFullName
    */
    QString getTempProjectDir( const QString &projectFullName );
    /**
    * Returns given path if doesn't exists, otherwise the slightly modified non-existing path by adding a number to given path.
    * \param QString path
    */
    QString findUniqueProjectDirectoryName( QString path );
    QNetworkReply *getProjectInfo( const QString &projectFullName );

    //! Creates a unique project directory for given project name (used for initial download of a project)
    QString createUniqueProjectDirectory( const QString &projectName );

    //! Called when download/update of project data has finished to finalize things and emit sync finished signal
    void finalizeProjectUpdate( const QString &projectFullName );

    //! Takes care of removal of the transaction, writing new metadata and emits syncProjectFinished()
    void finishProjectSync( const QString &projectFullName, bool syncSuccessful );

    QNetworkAccessManager mManager;
    QString mApiRoot;
    LocalProjectsManager &mLocalProjects;
    MerginProjectList mRemoteProjects;
    QString mDataDir; // dir with all projects
    QString mUsername;
    QString mPassword;
    int mUserId = -1;
    QByteArray mAuthToken;
    QDateTime mTokenExpiration;
    int mDiskUsage = 0; // in Bytes
    int mStorageLimit = 0; // in Bytes

    enum CustomAttribute
    {
      AttrProjectFullName = QNetworkRequest::User,
      AttrChunkNo         = QNetworkRequest::User + 1,
    };

    QHash<QString, TransactionStatus> mTransactionalStatus; //projectFullname -> transactionStatus
    static const QSet<QString> sIgnoreExtensions;
    static const QSet<QString> sIgnoreFiles;
    QEventLoop mAuthLoopEvent;
    MerginApiStatus::VersionStatus mApiVersionStatus = MerginApiStatus::VersionStatus::UNKNOWN;

    static const int CHUNK_SIZE = 65536;
    static const int UPLOAD_CHUNK_SIZE = 10 * 1024 * 1024; // Should be the same as on Mergin server
    const QString TEMP_FOLDER = QStringLiteral( ".temp/" );

    friend class TestMerginApi;
};

#endif // MERGINAPI_H
