#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QTimer>
#include <QThreadPool>

#include "seafile-applet.h"
#include "ui/tray-icon.h"
#include "configurator.h"
#include "account-mgr.h"
#include "rpc/rpc-client.h"
#include "rpc/local-repo.h"
#include "utils/file-utils.h"
#include "utils/utils.h"
#include "utils/uninstall-helpers.h"
#include "data-mgr.h"
#include "tasks.h"
#include "transfer-mgr.h"

#include "auto-update-mgr.h"

namespace {

const char *kFileCacheTopDirName = "file-cache";
const char *kFileCacheTempTopDirName = "file-cache-tmp";
const char *kFileCacheDBFileName = "file-cache.db";

inline bool addPath(QFileSystemWatcher *watcher, const QString &file) {
  if (watcher->files().contains(file))
      return true;
  bool ret = watcher->addPath(file);
  if (!ret) {
      qWarning("[AutoUpdateManager] failed to watch cache file %s", file.toUtf8().data());
  }
  return ret;
}

inline bool removePath(QFileSystemWatcher *watcher, const QString &file) {
  if (!watcher->files().contains(file))
      return true;
  bool ret = watcher->removePath(file);
  if (!ret) {
      qWarning("[AutoUpdateManager] failed to remove watch on cache file %s", file.toUtf8().data());
  }
  return ret;
}
} // anonymous namespace

SINGLETON_IMPL(AutoUpdateManager)


AutoUpdateManager::AutoUpdateManager()
{
    connect(&watcher_, SIGNAL(fileChanged(const QString&)),
            this, SLOT(onFileChanged(const QString&)));
}

void AutoUpdateManager::start()
{
    cleanCachedFile();
}

void AutoUpdateManager::watchCachedFile(const Account& account,
                                        const QString& repo_id,
                                        const QString& path)
{
    QString local_path = DataManager::getLocalCacheFilePath(repo_id, path);
    qDebug("[AutoUpdateManager] watch cache file %s", local_path.toUtf8().data());
    if (!QFileInfo(local_path).exists()) {
        qWarning("[AutoUpdateManager] unable to watch non-existent cache file %s", local_path.toUtf8().data());
        return;
    }

    // do we have it in deferred list ?
    // skip if yes
    Q_FOREACH(const WatchedFileInfo& info, deleted_files_infos_)
    {
        if (repo_id == info.repo_id && path == info.path_in_repo)
            return;
    }

    addPath(&watcher_, local_path);
    watch_infos_[local_path] = WatchedFileInfo(account, repo_id, path);
}

void AutoUpdateManager::cleanCachedFile()
{
    qDebug("[AutoUpdateManager] cancel all download tasks");
    TransferManager::instance()->cancelAllDownloadTasks();

    const Account cur_account = seafApplet->accountManager()->currentAccount();
    foreach(const QString& key, watch_infos_.keys()) {
        if (watch_infos_[key].account == cur_account)
            watch_infos_.remove(key);
    }

    FileCache::instance()->cleanCurrentAccountCache();

    CachedFilesCleaner *cleaner = new CachedFilesCleaner();
    QThreadPool::globalInstance()->start(cleaner);
}

void AutoUpdateManager::onFileChanged(const QString& local_path)
{
    qDebug("[AutoUpdateManager] detected cache file %s changed", local_path.toUtf8().data());
#ifdef Q_OS_MAC
    if (MacImageFilesWorkAround::instance()->isRecentOpenedImage(local_path)) {
        return;
    }
#endif
    removePath(&watcher_, local_path);
    QString repo_id, path_in_repo;
    if (!watch_infos_.contains(local_path)) {
        // filter unwanted events
        return;
    }

    WatchedFileInfo &info = watch_infos_[local_path];

    if (!QFileInfo(local_path).exists()) {
        qDebug("[AutoUpdateManager] detected cache file %s renamed or removed", local_path.toUtf8().data());
        WatchedFileInfo deferred_info = info;
        removeWatch(local_path);
        // Some application would deleted and recreate the file when saving.
        // We work around that by double checking whether the file gets
        // recreated after a short period
        QTimer::singleShot(5000, this, SLOT(checkFileRecreated()));
        deleted_files_infos_.enqueue(deferred_info);
        return;
    }

    DataManager data_mgr(info.account);
    FileNetworkTask *task = data_mgr.createUploadTask(
        info.repo_id, ::getParentPath(info.path_in_repo),
        local_path, ::getBaseName(local_path), true);

    connect(task, SIGNAL(finished(bool)),
            this, SLOT(onUpdateTaskFinished(bool)));

    qDebug("[AutoUpdateManager] start uploading new version of file %s", local_path.toUtf8().data());

    task->start();
    info.uploading = true;
}

void AutoUpdateManager::onUpdateTaskFinished(bool success)
{
    FileUploadTask *task = qobject_cast<FileUploadTask *>(sender());
    if (task == NULL)
        return;
    const QString local_path = task->localFilePath();
    if (success) {
        qDebug("[AutoUpdateManager] uploaded new version of file %s", local_path.toUtf8().data());
        seafApplet->trayIcon()->showMessage(tr("Upload Success"),
                                            tr("File \"%1\"\nuploaded successfully.").arg(QFileInfo(local_path).fileName()),
                                            task->repoId());
        emit fileUpdated(task->repoId(), task->path());
        addPath(&watcher_, local_path);
        WatchedFileInfo& info = watch_infos_[local_path];
        info.uploading = false;
    } else {
        qWarning("[AutoUpdateManager] failed to upload new version of file %s", local_path.toUtf8().data());
        seafApplet->trayIcon()->showMessage(tr("Upload Failure"),
                                            tr("File \"%1\"\nfailed to upload.").arg(QFileInfo(local_path).fileName()),
                                            task->repoId());
        watch_infos_.remove(local_path);
        return;
    }
}

void AutoUpdateManager::removeWatch(const QString& local_path)
{
    watch_infos_.remove(local_path);
    removePath(&watcher_, local_path);
}

void AutoUpdateManager::checkFileRecreated()
{
    if (deleted_files_infos_.isEmpty()) {
        // impossible
        return;
    }

    const WatchedFileInfo info = deleted_files_infos_.dequeue();
    const QString path = DataManager::getLocalCacheFilePath(info.repo_id, info.path_in_repo);
    if (QFileInfo(path).exists()) {
        qDebug("[AutoUpdateManager] detected recreated file %s", path.toUtf8().data());
        addPath(&watcher_, path);
        watch_infos_[path] = info;
        // Some applications like MSOffice would remove the original file and
        // recreate it when the user modifies the file.
        onFileChanged(path);
    }
}

#ifdef Q_OS_MAC
SINGLETON_IMPL(MacImageFilesWorkAround)

MacImageFilesWorkAround::MacImageFilesWorkAround()
{
}

void MacImageFilesWorkAround::fileOpened(const QString& path)
{
    QString mimetype = ::mimeTypeFromFileName(path);
    if (mimetype.startsWith("image") || mimetype == "application/pdf") {
        images_[path] = QDateTime::currentMSecsSinceEpoch();
    }
}

bool MacImageFilesWorkAround::isRecentOpenedImage(const QString& path)
{
    qint64 ts = images_.value(path, 0);
    if (QDateTime::currentMSecsSinceEpoch() < ts + 1000 * 10) {
        return true;
    } else {
        return false;
    }
}
#endif

CachedFilesCleaner::CachedFilesCleaner()
{
}

void CachedFilesCleaner::run()
{
    QString file_cache_dir = pathJoin(seafApplet->configurator()->seafileDir(),
                               kFileCacheTopDirName);
    QString file_cache_tmp_dir = pathJoin(seafApplet->configurator()->seafileDir(),
                                   kFileCacheTempTopDirName);
    QString file_cache_db_file = pathJoin(seafApplet->configurator()->seafileDir(),
                                   kFileCacheDBFileName);

    qDebug("[AutoUpdateManager] removing cached files");
    if (QFile(file_cache_db_file).exists()) {
        if (!QFile(file_cache_db_file).remove()) {
            qWarning("[AutoUpdateManager] failed to remove db file");
        }
    }
    if (QDir(file_cache_tmp_dir).exists()) {
        delete_dir_recursively(file_cache_tmp_dir);
    }
    if (QDir(file_cache_dir).exists()) {
        QDir().rename(file_cache_dir, file_cache_tmp_dir);
        delete_dir_recursively(file_cache_tmp_dir);
    }
}
