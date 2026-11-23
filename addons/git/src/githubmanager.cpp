#include "githubmanager.h"
#include "gitmanagedfiles.h"
#include "gitfolderguard.h"
#include "../../../src/core/workspace/pilonworkspace.h"
#include "../../../src/core/network/networksnapshot.h"
#include "../platform/android/androidgit.h"
#ifdef Q_OS_ANDROID
#include "gitcredentialserver.h"
#include "../../../src/platform/android/androidcredentials.h"
#endif

#include <QDesktopServices>
#include <QCoreApplication>
#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QJsonObject>
#include <QMetaMethod>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include "../../../src/core/workspace/pilonsettings.h"
#include <QStandardPaths>
#include <QUrl>
#include <QTemporaryFile>
#include <QtEndian>
#include <QUuid>
#include <utility>
#include <algorithm>
#include <QtConcurrentRun>

namespace {
QString repositoryCachePath(const QString &provider) {
    return NetworkSnapshot::path("git-repositories", {provider});
}

QVariantList readRepositoryCache(const QVariantMap &provider) {
    if (provider.value("account").toString().isEmpty()) return {};
    const auto snapshot = NetworkSnapshot::read(repositoryCachePath(provider.value("id").toString()));
    if (snapshot.value("host").toString() != provider.value("host").toString()
        || snapshot.value("account").toString() != provider.value("account").toString()) return {};
    return snapshot.value("repositories").toArray().toVariantList();
}

QString gitLine(const QByteArray &output) {
    auto line=QString::fromUtf8(output);
    if (line.endsWith('\n')) line.chop(1);
    return line;
}
GitCredentials::Result restoreCredential(QByteArray scope,const QByteArray &backup) {
    if (backup.isEmpty()) return GitCredentials::execute("erase",scope);
    scope.chop(1);
    for (const auto &line:backup.split('\n')) if (line.startsWith("password=")) scope+=line+'\n';
    return GitCredentials::execute("store",scope+'\n');
}
constexpr qint64 textPreviewLimit=2*1024*1024;
constexpr qint64 notePreviewLimit=16*1024*1024;
bool relativeFile(const QString &path) {
    if(path.isEmpty() || QDir::isAbsolutePath(path) || path.contains(QChar::Null))return false;
    for(const auto &part:path.split('/'))if(part.isEmpty() || part=="." || part==".." || part==".git")return false;
    return true;
}
QStringList containedFolders(const QString &repository,const QStringList &folders) {
    if(repository.isEmpty())return {};
    const auto root=QDir::cleanPath(repository);QStringList result;
    for(const auto &folder:folders) {
        const auto path=QDir::cleanPath(folder);
        if(path==root || root.startsWith(path+'/'))result.append(QString{});
        else {const auto prefix=root.endsWith('/')?root:root+'/';if(path.startsWith(prefix))result.append(path.mid(prefix.size()));}
    }
    result.removeDuplicates();return result;
}
QStringList privateFolders(const QString &repository) {
    return containedFolders(repository,{PilonWorkspace::appDataDirectory()});
}
QStringList locallyExcludedFolders(const QString &repository) {
    auto folders=containedFolders(repository,{PilonWorkspace::appDataDirectory(),
        PilonWorkspace::modelsDirectory(),PilonWorkspace::addonModelsDirectory("notes"),
        PilonWorkspace::configuredRoot()+"/Models"});
    folders.removeAll(QString{});return folders;
}
bool inFolders(const QString &path,const QStringList &folders) {
    return std::any_of(folders.cbegin(),folders.cend(),[&path](const auto &folder){return folder.isEmpty() || path==folder || path.startsWith(folder+'/');});
}
bool privateFile(const QString &root,const QString &path) {return inFolders(path,privateFolders(root));}
QVariantMap previewFailure(QVariantMap result,const QString &error,const std::shared_ptr<std::atomic_bool> &cancel) {
    result["kind"]="error";result["error"]=error;
    if(cancel && cancel->load()){result["cancelled"]=true;result["error"]=QObject::tr("Preview cancelled.");}
    return result;
}
QVariantMap previewBytes(const QByteArray &data,const QString &path) {
    QVariantMap result{{"bytes",data.size()},{"diff",false}};
    const auto json=QJsonDocument::fromJson(data);
    if(path.endsWith(".pin",Qt::CaseInsensitive) && json.isObject()) {
        const auto object=json.object();const auto stored=object.value("pages").toArray();QVariantList pages;
        qint64 decodedBytes=0,pixels=0;bool valid=!stored.isEmpty() && stored.size()<=32;
        for(const auto &value:stored) {
            if(!valid)break;
            const auto page=value.toObject();
            const auto decoded=QByteArray::fromBase64Encoding(page.value("data").toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
            const auto &png=decoded.decoded;
            if(page.value("mime")!="image/png" || !decoded || png.size()<24 || png.size()>8*1024*1024
                || png.first(8)!=QByteArray::fromHex("89504e470d0a1a0a") || png.mid(12,4)!="IHDR"){valid=false;break;}
            const auto width=qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(png.constData()+16));
            const auto height=qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(png.constData()+20));
            if(!width || !height || width>4096 || height>4096){valid=false;break;}
            decodedBytes+=png.size();pixels+=qint64(width)*height;
            if(pixels>32*1024*1024 || decodedBytes>12*1024*1024){valid=false;break;}
            pages.append(QVariantMap{{"source","data:image/png;base64,"+QString::fromLatin1(png.toBase64())},{"width",width},{"height",height}});
        }
        if(valid){result["kind"]="note";result["pages"]=pages;result["title"]=object.value("title").toString().left(512);return result;}
        if(object.contains("pages")){
            result["kind"]=stored.size()>32?"large":"binary";
            result["error"]=QObject::tr("This note has no safe stored-page preview. Save the version to inspect the exact file.");return result;
        }
    }
    const bool plain=std::none_of(data.cbegin(),data.cend(),[](char ch){const auto value=static_cast<unsigned char>(ch);return value<32 && ch!='\n' && ch!='\r' && ch!='\t';})
        && QString::fromUtf8(data).toUtf8()==data;
    if(plain && data.size()<=textPreviewLimit) {
        const auto pretty=json.isNull()?QByteArray{}:json.toJson(QJsonDocument::Indented);
        result["kind"]="text";result["text"]=QString::fromUtf8(!pretty.isEmpty() && pretty.size()<=textPreviewLimit?pretty:data);return result;
    }
    result["kind"]=data.size()>textPreviewLimit?"large":"binary";
    result["error"]=QObject::tr("This version has no bounded text or page preview. Save the version to inspect its exact contents.");return result;
}
QString exportTarget(const QString &destination,const QVariantMap &context,QString &error) {
    const QFileInfo selected(destination);const auto parent=QFileInfo(selected.absolutePath()).canonicalFilePath();
    if(!QDir::isAbsolutePath(destination) || destination.contains(QChar::Null) || parent.isEmpty() || !QFileInfo(parent).isDir()
        || selected.fileName().isEmpty() || selected.exists() || selected.isSymLink()){error=QObject::tr("Choose a new absolute filename in an existing folder. Existing files are kept.");return {};}
    QStringList blocked{context.value("root").toString(),context.value("gitDirectory").toString()};
    const auto gitDirectory=context.value("gitDirectory").toString();
    const auto common=GitFolderGuard::textFile(gitDirectory+"/commondir");
    if(!common.isEmpty())blocked.append(QFileInfo(QDir(gitDirectory).absoluteFilePath(common)).canonicalFilePath());
    for(const auto &root:blocked)if(!root.isEmpty() && (parent==root || parent.startsWith(root+'/'))){error=QObject::tr("Save conflict versions outside the checkout and its Git metadata.");return {};}
    return QDir(parent).filePath(selected.fileName());
}
QVariantMap inspectVersion(QVariantMap context,const QString &destination,const std::shared_ptr<std::atomic_bool> &cancel) {
    QString error;const auto root=context.value("root").toString(),head=context.value("head").toString(),incoming=context.value("incoming").toString();
    const auto verify=[&]{return GitManagedFiles::verifyMergeRefs(root,head,incoming,&error,cancel);};
    if(!verify())return previewFailure(context,error,cancel);
    if(!context.value("exists").toBool()){context["kind"]="deleted";context["bytes"]=0;return context;}
    auto oid=context.value("blob").toString();qint64 size=0;
    if(oid.isEmpty() && (!GitManagedFiles::blobAt(root,context.value("side")=="current"?head:incoming,context.value("path").toString(),&oid,&size,&error,cancel) || oid.isEmpty()))
        return previewFailure(context,error.isEmpty()?QObject::tr("The retained file version is unavailable."):error,cancel);
    if(destination.isEmpty()) {
        QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);
        const auto limit=context.value("path").toString().endsWith(".pin",Qt::CaseInsensitive)?notePreviewLimit:textPreviewLimit;
        if(!GitManagedFiles::copyBlob(root,oid,&buffer,limit,&size,&error,cancel)) {
            if(size>limit && !(cancel && cancel->load())){if(!verify())return previewFailure(context,error,cancel);context["kind"]="large";context["bytes"]=size;context["error"]=QObject::tr("This version is too large to preview. Save it to inspect the exact file.");return context;}
            return previewFailure(context,error,cancel);
        }
        if(!verify())return previewFailure(context,error,cancel);
        const auto preview=previewBytes(bytes,context.value("path").toString());for(auto it=preview.cbegin();it!=preview.cend();++it)context[it.key()]=it.value();return context;
    }
    const auto target=exportTarget(destination,context,error);if(target.isEmpty())return previewFailure(context,error,cancel);
    QTemporaryFile file(QDir(QFileInfo(target).absolutePath()).filePath(".pilon-version-XXXXXX"));
    if(!file.open()){return previewFailure(context,QObject::tr("The saved version could not be created."),cancel);}
    if(!GitManagedFiles::copyBlob(root,oid,&file,-1,&size,&error,cancel) || !verify())return previewFailure(context,error,cancel);
    if(cancel && cancel->load())return previewFailure(context,{},cancel);
    if(!file.flush())return previewFailure(context,QObject::tr("The saved version could not be written."),cancel);
    const auto temporary=file.fileName();file.close();
    if(!QFile::rename(temporary,target))return previewFailure(context,QObject::tr("The destination exists or the version could not be saved. Choose a new filename."),cancel);
    file.setAutoRemove(false);context["exported"]=true;context["destination"]=target;context["bytes"]=size;return context;
}
QVariantMap publicPreview(QVariantMap result) {
    for(const auto *key:{"head","incoming","blob","exists","gitDirectory","status","previousPath"})result.remove(key);
    return result;
}
QVariantMap inspectChange(QVariantMap context,const std::shared_ptr<std::atomic_bool> &cancel) {
    const auto root=context.value("root").toString(),path=context.value("path").toString(),head=context.value("head").toString();
    const auto status=context.value("status").toString();const bool cached=status.startsWith('D');
    const QFileInfo file(QDir(root).filePath(path));QString error;
    if(!file.exists() && !file.isSymLink() && !status.contains('D'))return previewFailure(context,QObject::tr("The selected file moved or disappeared. Refresh Changes."),cancel);
    if(!cached && file.exists() && !file.isSymLink()) {
        if(!file.isFile())return previewFailure(context,QObject::tr("This item is not a regular file and has no inline preview."),cancel);
        const auto limit=path.endsWith(".pin",Qt::CaseInsensitive)?notePreviewLimit:textPreviewLimit;
        if(file.size()>limit){context["kind"]="large";context["bytes"]=file.size();context["error"]=QObject::tr("This file is too large for an inline preview.");return context;}
        QFile current(file.absoluteFilePath());if(!current.open(QIODevice::ReadOnly))return previewFailure(context,QObject::tr("The selected file could not be read."),cancel);
        const auto bytes=current.read(limit+1);
        if(current.error()!=QFile::NoError)return previewFailure(context,QObject::tr("The selected file could not be read."),cancel);
        if(bytes.size()>limit){context["kind"]="large";context["bytes"]=bytes.size();context["error"]=QObject::tr("This file is too large for an inline preview.");return context;}
        const auto parsed=previewBytes(bytes,path);for(auto it=parsed.cbegin();it!=parsed.cend();++it)context[it.key()]=it.value();
        if(context.value("kind")=="note" || head.isEmpty() || status=="??" || context.value("kind")!="text") {
            if(context.value("kind")=="binary" || context.value("kind")=="large")context["error"]=QObject::tr("This file has no bounded inline preview.");
            return cancel && cancel->load()?previewFailure(context,{},cancel):context;
        }
    } else if(head.isEmpty())return previewFailure(context,QObject::tr("This item has no inline preview."),cancel);
    QStringList paths{path};const auto previous=context.value("previousPath").toString();if(!previous.isEmpty())paths.append(previous);
    for(const auto &name:paths) {
        QString oid;qint64 size=0;
        if(!GitManagedFiles::blobAt(root,head,name,&oid,&size,&error,cancel))return previewFailure(context,error,cancel);
        if(size>textPreviewLimit){context["kind"]="large";context["bytes"]=size;context["error"]=QObject::tr("The committed file is too large for an inline diff.");return context;}
    }
    QByteArray diff;QBuffer buffer(&diff);buffer.open(QIODevice::WriteOnly);
    if(!GitManagedFiles::diffFile(root,head,paths,cached,&buffer,textPreviewLimit,&error,cancel))return previewFailure(context,error,cancel);
    if(diff.isEmpty())return previewFailure(context,QObject::tr("The selected file has no remaining differences. Refresh Changes."),cancel);
    context["kind"]="text";context["text"]=QString::fromUtf8(diff);context["diff"]=true;return context;
}
}

GitHubManager::GitHubManager(QObject *parent) : QObject(parent),
#ifdef Q_OS_ANDROID
    m_git(PilonWorkspace::packagedExecutable(QStringLiteral("git"))),
    m_gh(PilonWorkspace::packagedExecutable(QStringLiteral("gh"))),
    m_glab(PilonWorkspace::packagedExecutable(QStringLiteral("glab"))) {
#else
    m_git(QStandardPaths::findExecutable(QStringLiteral("git"))),
    m_gh(QStandardPaths::findExecutable(QStringLiteral("gh"))),
    m_glab(QStandardPaths::findExecutable(QStringLiteral("glab"))) {
#endif
    PilonSettings settings;
#ifdef Q_OS_ANDROID
    const auto defaultCloneDirectory = PilonWorkspace::publicDirectory("Projects");
#else
    const auto defaultCloneDirectory = QDir::homePath() + "/Projects";
#endif
    m_cloneDirectory = settings.value("GitHub/cloneDirectory", defaultCloneDirectory).toString();
    m_commitName = settings.value("GitHub/commitName").toString();
    m_commitEmail = settings.value("GitHub/commitEmail").toString();
    m_localRepositories = settings.value("Git/localRepositories").toMap();
    m_pendingWorktrees=settings.value("Git/pendingWorktrees").toMap();
    const auto legacy=settings.value("GitHub/localRepositories").toMap();
    for (auto it=legacy.cbegin();it!=legacy.cend();++it) {
        const auto path=QFileInfo(it.value().toString()).canonicalFilePath();
        if (!path.isEmpty() && QFileInfo(path).isDir() && !m_localRepositories.contains(path))
            m_localRepositories[path]=QVariantMap{{"localPath",path},{"remote",it.key()}};
    }
    m_provider = settings.value("Git/provider", "GitHub").toString();
    if (!QStringList{"GitHub", "GitLab", "Custom"}.contains(m_provider)) m_provider = "GitHub";
    m_host = settings.value("Git/" + m_provider + "/host", m_provider == "GitHub" ? "github.com" : m_provider == "GitLab" ? "gitlab.com" : "").toString();
    m_sshKey = settings.value("Git/" + m_provider + "/sshKey").toString();
    m_providers = settings.value("Git/providers").toList();
    m_activeProvider = settings.value("Git/activeProvider").toString();
    if (m_providers.isEmpty() && !settings.contains("Git/providers")) {
        for (const auto &type : QStringList{"GitHub", "GitLab", "Custom"}) {
            if (type != "GitHub" && type != m_provider && !settings.contains("Git/" + type + "/host")) continue;
            const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_providers.append(QVariantMap{{"id", id}, {"type", type},
                {"host", settings.value("Git/" + type + "/host", type == "GitHub" ? "github.com" : type == "GitLab" ? "gitlab.com" : "")},
                {"sshKey", settings.value("Git/" + type + "/sshKey")}});
            if (type == m_provider) m_activeProvider = id;
        }
    }
    const auto selected = m_activeProvider;
    m_activeProvider.clear(); m_provider = "Custom"; m_host.clear(); m_sshKey.clear();
    if (!m_providers.isEmpty()) {
        const bool exists = std::any_of(m_providers.cbegin(), m_providers.cend(), [&selected](const QVariant &value) { return value.toMap().value("id") == selected; });
        setActiveProvider(exists ? selected : m_providers.first().toMap().value("id").toString());
    }
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        recordError(tr("Git did not finish in time. Please try again."));
        m_process.kill();
    });
    connect(&m_process,&QProcess::readyReadStandardOutput,this,&GitHubManager::readProcessOutput);
    connect(&m_process,&QProcess::readyReadStandardError,this,&GitHubManager::readProcessOutput);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        m_timeout.stop(); m_done = {};
        fail(tr("Could not start Git support: %1").arg(m_process.errorString()));
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_timeout.stop();
        readProcessOutput();
        auto done = std::move(m_done);
        if(m_outputExceeded){fail(m_error);return;}
        const auto output = m_output, errors = m_errors;
        if (done) done(status == QProcess::NormalExit ? code : -1, output, errors);
    });
    connect(&m_snapshotWatcher,&QFutureWatcher<QString>::finished,this,[this] {
        if (!m_worktreeLeaseActive || m_phase!="materializing") return;
        if (m_cancelled) { endWorktreeUpdate("cancelled"); return; }
        const auto error=m_snapshotWatcher.result();
        if (!error.isEmpty()) { fail(error); return; }
        if (m_updateKind=="merge") {verifyPinnedState([this]{applyIncoming();});return;}
        requestValidation(m_snapshot->path(),[this]{verifyPinnedState([this]{applyIncoming();});});
    });
    connect(&m_credentialsWatcher,&QFutureWatcher<GitCredentials::Result>::finished,this,[this] {
        auto done=std::exchange(m_credentialsDone,{});
        if(done)done(m_credentialsWatcher.result());
    });
    connect(&m_previewWatcher,&QFutureWatcher<QVariantMap>::finished,this,[this]{
        auto result=m_previewWatcher.result();const auto phase=m_previewPhase;
        const auto root=phase=="reading-change"?m_repository.value("localPath").toString():m_worktreeRoot;
        if(result.value("root").toString()!=root || result.value("operationId").toULongLong()!=m_operationId)
            result=previewFailure(result,tr("The repository changed while its preview was being read."),{});
        m_busy=false;m_phase=m_previewReturnPhase;
        // The preview pane and export dialog present their own result.
        m_error=result.value("kind")=="error" && !result.value("cancelled").toBool()?result.value("error").toString():QString{};
        if(phase=="reading-version"){m_conflictPreview=publicPreview(result);emit conflictPreviewChanged();}
        else if(phase=="reading-change"){m_changePreview=publicPreview(result);emit changePreviewChanged();}
        else if(result.value("exported").toBool()){
            m_notice=tr("Version saved to %1.").arg(result.value("destination").toString());
            emit conflictVersionExported(result.value("path").toString(),result.value("side").toString(),result.value("destination").toString());
        }
        emit changed();
    });
}
GitHubManager::~GitHubManager() {
    if (m_cancelSnapshot) m_cancelSnapshot->store(true);
    m_snapshotWatcher.waitForFinished();
    m_credentialsWatcher.waitForFinished();
    if(m_cancelPreview)m_cancelPreview->store(true);
    m_previewWatcher.waitForFinished();
    disconnect(&m_process, nullptr, this, nullptr);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();
        if (!m_process.waitForFinished(1000)) { m_process.kill(); m_process.waitForFinished(1000); }
    }
}
#ifdef Q_OS_ANDROID
bool GitHubManager::prepareCredentialServer() {
    if (m_gitCredentialServer) return !m_gitCredentialServer->address().isEmpty();
    if (!QDir().mkpath(QDir::homePath() + "/.cache/pilon-git")) {
        fail(tr("The private Git runtime folder could not be created.")); return false;
    }
    m_gitCredentialServer = new GitCredentialServer(QDir::homePath()
        + "/.cache/pilon-git/credential-" + QUuid::createUuid().toString(QUuid::Id128),
        [this](QString action, QByteArray request, GitCredentialServer::Completed completed) {
            const auto phase = m_phase;
            credentialTask([action, request] {
                if (action != "keyring") return GitCredentials::execute(action, request);
                const auto object = QJsonDocument::fromJson(request).object();
                auto decoded = QByteArray::fromBase64Encoding(object.value("secret").toString().toLatin1(),
                    QByteArray::AbortOnBase64DecodingErrors);
                if (!decoded) return GitCredentials::Result{false, {}, {}};
                QJsonObject attributes{{"application", "pilon-git"},
                    {"provider", object.value("_provider")}, {"host", object.value("_host")},
                    {"service", object.value("service")}};
                if (!object.value("all").toBool()) attributes.insert("username", object.value("user"));
                auto result = AndroidCredentials::request(object.value("action").toString(), attributes, decoded.decoded);
                decoded.decoded.fill('\0');
                const auto output = QJsonDocument(QJsonObject{{"found", result.found},
                    {"secret", QString::fromLatin1(result.secret.toBase64())}}).toJson(QJsonDocument::Compact);
                result.secret.fill('\0');
                return GitCredentials::Result{result.ok, result.ok ? output : QByteArray{}, {}};
            }, [this, phase, completed](GitCredentials::Result result) {
                if (m_busy) m_phase = phase;
                emit changed();
                completed(std::move(result));
            });
        }, this);
    if (m_gitCredentialServer->address().isEmpty()) {
        fail(tr("The private Git credential service could not be started.")); return false;
    }
    return true;
}
#endif
bool GitHubManager::begin(bool requireGit) {
    if (busy() || repositorySwitchBlocked()) return false;
    if (requireGit && !available()) { fail(tr("Install git to use repository actions.")); return false; }
    m_busy = true; m_cancelled=false;m_phase="running"; m_error.clear(); m_notice.clear(); emit changed(); return true;
}
void GitHubManager::run(const QString &program, const QStringList &args, const QString &directory, Done done) {
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("LC_ALL", "C"); env.insert("NO_COLOR", "1");
    env.insert("GH_NO_UPDATE_NOTIFIER", "1"); env.insert("GH_NO_EXTENSION_UPDATE_NOTIFIER", "1");
    env.insert("GIT_TERMINAL_PROMPT", "0");
    for (const auto *name : {"GIT_DIR","GIT_WORK_TREE","GIT_COMMON_DIR","GIT_INDEX_FILE","GIT_OBJECT_DIRECTORY","GIT_ALTERNATE_OBJECT_DIRECTORIES","GIT_NAMESPACE"})
        env.remove(name);
    if(program==m_git && !m_temporaryIndex.isEmpty())env.insert("GIT_INDEX_FILE",m_temporaryIndex);
#ifdef Q_OS_ANDROID
    if (program == m_git || program == m_gh || program == m_glab) {
        const auto error = AndroidGit::prepareEnvironment(env);
        if (!error.isEmpty()) { done(-1, {}, error.toUtf8()); return; }
        AndroidGit::prepareRepositoryEnvironment(env, args.value(0) == "-C" ? args.value(1) : directory);
    }
    if (program == m_gh || program == m_glab) {
        if (!prepareCredentialServer()) return;
        if (!m_gitCredentialServer->setProviderScope(program == m_gh ? "gh" : "glab", m_host)) {
            fail(tr("The private provider credential service could not be prepared.")); return;
        }
    }
    env.insert("GH_CONFIG_DIR", QDir::homePath() + "/.config/gh");
    env.insert("GLAB_CONFIG_DIR", QDir::homePath() + "/.config/glab");
    if (m_gitCredentialServer)
        env.insert("PILON_GIT_CREDENTIAL_SOCKET", m_gitCredentialServer->address());
#endif
    env.insert("GITLAB_HOST", m_host); env.insert("GITLAB_API_HOST", m_host); env.insert("GITLAB_SSH_HOST", m_host);
    env.insert("GLAB_API_PROTOCOL", "https"); env.insert("GLAB_GIT_PROTOCOL", "https"); env.insert("GLAB_CONTAINER_REGISTRY_DOMAINS", "none");
    if (!m_sshKey.isEmpty() && (m_host.isEmpty() || remoteDetails(m_repository.value("remote").toString()).value("host") == m_host)) {
        QString key = m_sshKey; key.replace('\'', "'\\''");
        auto ssh = QStringLiteral("ssh");
#ifdef Q_OS_ANDROID
        ssh = PilonWorkspace::packagedExecutable("ssh");
        ssh.replace('\'', "'\\''");
        ssh = '\'' + ssh + '\'';
#endif
        env.insert("GIT_SSH_COMMAND", ssh + " -o BatchMode=yes -o IdentitiesOnly=yes -i '" + key + "'");
    }
    if (m_authenticating) env.insert("GH_BROWSER", QStandardPaths::findExecutable("true"));
    m_process.setProcessEnvironment(env);
    m_process.setWorkingDirectory(directory);
    m_output.clear(); m_errors.clear();m_outputExceeded=false;m_done = std::move(done);
    m_timeout.start(m_authenticating ? 15 * 60 * 1000 : 5 * 60 * 1000);
    auto arguments=args;
    if (program==m_git) arguments=QStringList{"--no-pager","-c","core.hooksPath=/dev/null","-c","merge.verifySignatures=false"}+arguments;
    m_process.start(program, arguments);
}
void GitHubManager::git(const QStringList &args, Done done) {
    QStringList options{"--literal-pathspecs"};
    const auto details = remoteDetails(m_repository.value("remote").toString());
    QString executable = m_provider == "GitHub" ? m_gh : m_provider == "GitLab" ? m_glab : QString{};
    if (!executable.isEmpty() && !m_account.isEmpty() && details.value("host").toString() == m_host) {
#ifdef Q_OS_ANDROID
        if (!prepareCredentialServer()) return;
        if (!m_gitCredentialServer->setProviderScope(m_provider == "GitHub" ? "gh" : "glab", m_host)) {
            fail(tr("The private provider credential service could not be prepared.")); return;
        }
#endif
        executable.replace('\'', "'\\''");
        options += QStringList{"-c", "credential.https://" + m_host + ".helper=",
            "-c", "credential.https://" + m_host + ".helper=!'" + executable + "' auth git-credential"};
    }
    if(m_provider=="Custom" && !credentialUrl().isEmpty()) {
        QUrl remote(m_repository.value("remote").toString());const auto username=remote.userName(QUrl::FullyDecoded);remote.setUserInfo({});
        if(remote.scheme()=="https" && remote.toString(QUrl::FullyEncoded)==credentialUrl()
            && (username.isEmpty() || username==credentialUsername())) {
#ifdef Q_OS_ANDROID
            if (!prepareCredentialServer()) return;
            const auto scope = GitCredentials::request(credentialUrl(), credentialUsername(), {}, nullptr);
            auto helper = PilonWorkspace::packagedExecutable("git_credential");
            if (helper.isEmpty() || m_gitCredentialServer->address().isEmpty()
                || !m_gitCredentialServer->setScope(scope)) {
                fail(tr("The private Git credential service could not be started.")); return;
            }
            helper.replace('\'', "'\\''");
            const auto command = "!'" + helper + "'";
#else
            auto helper=QCoreApplication::applicationFilePath();helper.replace('\'',"'\\''");
            const auto command = "!'" + helper + "' --git-credential";
#endif
            options+=QStringList{"-c","credential.useHttpPath=true","-c","credential."+credentialUrl()+".username="+credentialUsername(),
                "-c","credential."+credentialUrl()+".helper="+command};
        }
    }
    options += args;
    run(m_git, options, m_repository.value("localPath").toString(), std::move(done));
}
void GitHubManager::recordError(const QString &message)
{
    m_error = message;
    if (!message.isEmpty()) ++m_errorRevision;
}

void GitHubManager::fail(const QString &message) {
#ifdef Q_OS_ANDROID
    if (m_gitCredentialServer) m_gitCredentialServer->clearScope();
#endif
    m_temporaryIndex.clear();
    m_busy = false; m_authenticating = false; m_loginCode.clear(); m_loginUrl.clear();
    recordError(message.trimmed());
    if (m_worktreeLeaseActive) endWorktreeUpdate(m_cancelled ? "cancelled" : "failed");
    else { m_phase="failed"; emit changed(); }
}
bool GitHubManager::check(int code, const QByteArray &error) {
    if (!code) return true;
    QString text = QString::fromUtf8(error).trimmed();
    text.remove(QRegularExpression("\\x1b\\[[0-9;]*[A-Za-z]"));
    if (text.isEmpty()) text = tr("The Git operation did not complete.");
    fail(m_error.isEmpty() ? text.left(2000) : m_error); return false;
}
void GitHubManager::finish(const QString &message) {
#ifdef Q_OS_ANDROID
    if (m_gitCredentialServer) m_gitCredentialServer->clearScope();
#endif
    saveProviders();
    m_busy = false; m_phase="idle"; m_authenticating = false; m_loginCode.clear(); m_loginUrl.clear();
    if (!message.isEmpty()) m_notice = message;
    emit changed();
}
void GitHubManager::setCloneDirectory(const QString &path) {
    if(m_busy)return;
    const QString clean = QDir::cleanPath(path.trimmed());
    if (!QDir::isAbsolutePath(clean)) { fail(tr("Choose an absolute folder path.")); return; }
    m_cloneDirectory = clean; PilonSettings().setValue("GitHub/cloneDirectory", clean); emit changed();
}
void GitHubManager::setCommitName(const QString &name) {
    if(m_busy)return;
    m_commitName = name.trimmed(); PilonSettings().setValue("GitHub/commitName", m_commitName); emit changed();
}
void GitHubManager::setCommitEmail(const QString &email) {
    if(m_busy)return;
    m_commitEmail = email.trimmed(); PilonSettings().setValue("GitHub/commitEmail", m_commitEmail); emit changed();
}
bool GitHubManager::persistProviders(const QVariantList &providers,const QString &active) {
    PilonSettings settings;const auto previous=settings.value("Git/providers"),previousActive=settings.value("Git/activeProvider");
    settings.setValue("Git/providers",providers);settings.setValue("Git/activeProvider",active);settings.sync();
    if(settings.status()==PilonSettings::NoError)return true;
    if(previous.isValid())settings.setValue("Git/providers",previous);else settings.remove("Git/providers");
    if(previousActive.isValid())settings.setValue("Git/activeProvider",previousActive);else settings.remove("Git/activeProvider");
    settings.sync();recordError(tr("Unable to save Git account settings. Check your settings folder permissions and try again."));return false;
}
bool GitHubManager::saveProviders() {
    for (auto &value : m_providers) {
        auto entry = value.toMap();
        if (entry.value("id") != m_activeProvider) continue;
        entry["host"] = m_host; entry["sshKey"] = m_sshKey; entry["account"] = m_account; value = entry;
    }
    return persistProviders(m_providers,m_activeProvider);
}
QString GitHubManager::credentialUrl() const {
    for(const auto &value:m_providers)if(value.toMap().value("id").toString()==m_activeProvider)return value.toMap().value("credentialUrl").toString();
    return {};
}
QString GitHubManager::credentialUsername() const {
    for(const auto &value:m_providers)if(value.toMap().value("id").toString()==m_activeProvider)return value.toMap().value("credentialUsername").toString();
    return {};
}
void GitHubManager::credentialTask(std::function<GitCredentials::Result()> task,std::function<void(GitCredentials::Result)> done) {
    m_phase="credentials";m_credentialsDone=std::move(done);emit changed();
    m_credentialsWatcher.setFuture(QtConcurrent::run(std::move(task)));
}
void GitHubManager::changeCredential(const QString &action,const QByteArray &scope,const QByteArray &request,std::function<void(QByteArray)> done) {
    credentialTask([action,scope,request] {
        auto backup=GitCredentials::execute("get",scope);if(!backup.ok)return backup;
        auto result=GitCredentials::execute(action,request);
        if(!result.ok){if(!restoreCredential(scope,backup.output).ok)result.error+=QStringLiteral(" The saved credential could not be restored. Unlock the keyring and retry.");return result;}
        result.output=backup.output;return result;
    },[this,done](GitCredentials::Result result){if(!result.ok){fail(result.error);return;}done(result.output);});
}
void GitHubManager::rollbackCredential(const QByteArray &scope,const QByteArray &backup,const QString &error) {
    credentialTask([scope,backup]{return restoreCredential(scope,backup);},[this,error](GitCredentials::Result result){
        fail(result.ok ? error : error+tr(" The saved credential could not be restored. Unlock the keyring and retry this account."));
    });
}
void GitHubManager::saveCredentials(const QString &url,const QString &username,const QString &token) {
    if(!begin(false))return;
    if(m_provider!="Custom"){fail(tr("Select a Custom account to save an HTTPS username and token."));return;}
    QString error;const auto request=GitCredentials::request(url,username,token,&error);
    if(request.isEmpty() || username.isEmpty() || token.isEmpty()){fail(error.isEmpty() ? tr("Enter a Git username and token.") : error);return;}
    const auto address=QUrl(url.trimmed()).toString(QUrl::FullyEncoded);
    if(!credentialUrl().isEmpty() && (credentialUrl()!=address || credentialUsername()!=username)){fail(tr("Sign out before changing the saved repository URL or username."));return;}
    const auto scope=GitCredentials::request(address,username,{},&error);const auto id=m_activeProvider;
    changeCredential("store",scope,request,[this,scope,address,username,id](QByteArray backup){
        auto providers=m_providers;
        for(auto &value:providers){auto entry=value.toMap();if(entry.value("id").toString()!=id)continue;entry["credentialUrl"]=address;entry["credentialUsername"]=username;value=entry;}
        if(!persistProviders(providers,m_activeProvider)){rollbackCredential(scope,backup,m_error);return;}
        m_providers=providers;finish(tr("Credentials saved."));emit credentialsSaved();
    });
}
QString GitHubManager::proposedManagedAttributes(const QStringList &folders) const {return GitManagedFiles::proposedAttributes(folders);}
QString GitHubManager::proposedManagedIgnores(const QStringList &folders) const {return GitManagedFiles::proposedIgnores(folders);}
void GitHubManager::applyManagedPolicy(const QStringList &folders) {
    if(!localReady())return;
    if(folders.isEmpty() || proposedManagedAttributes(folders).isEmpty() || proposedManagedIgnores(folders).isEmpty()){fail(tr("Choose valid managed folders without line breaks before reviewing their sync rules."));return;}
    inspectFolder(m_repository.value("localPath").toString(),[this,folders]{
        if(!m_repository.value("pendingOperation").toString().isEmpty()){fail(tr("Recover the unfinished repository update before applying sync rules."));return;}
        m_pinnedHead=m_repository.value("head").toString();m_incoming.clear();
        requestWorktreeUpdate(m_repository.value("localPath").toString(),[this,folders]{
            auto requested=folders,approved=m_managedFolders;requested.removeDuplicates();approved.removeDuplicates();requested.sort();approved.sort();
            if(requested!=approved){fail(tr("The managed folders changed. Reopen the sync rules review."));return;}
            verifyPinnedState([this,requested]{
                requestValidation(m_worktreeRoot,[this,requested]{
                    if(!savePendingWorktree("policy")){fail(m_error);return;}
                    QString error;
                    if(!GitManagedFiles::applyAttributes(m_worktreeRoot,requested,&error) || !GitManagedFiles::applyIgnoreRules(m_worktreeRoot,requested,&error)){fail(error);return;}
                    QStringList histories;for(const auto &folder:requested)histories.append(folder.isEmpty()?QString(".library.json"):folder+"/.library.json");
                    QStringList arguments{"ls-files","--cached","-z","--"};arguments+=histories;
                    git(arguments,[this,histories](int code,const QByteArray &out,const QByteArray &err){
                        if(!check(code,err))return;
                        QStringList tracked;
                        for(const auto &entry:out.split('\0')){
                            if(entry.isEmpty())continue;const auto path=QString::fromUtf8(entry);
                            if(path.toUtf8()!=entry || !histories.contains(path)){fail(tr("Git returned an unexpected opened-history path. No files were removed from disk."));return;}
                            tracked.append(path);
                        }
                        tracked.removeDuplicates();
                        const auto completed=[this]{loadStatus([this]{m_policyAwaitingReload=true;m_notice=tr("Sync rules applied. Review and commit the repository changes.");endWorktreeUpdate("applied");});};
                        if(tracked.isEmpty()){completed();return;}
                        QStringList arguments{"rm","--cached","--"};arguments+=tracked;
                        git(arguments,[this,completed](int code,const QByteArray &,const QByteArray &){
                            if(code){fail(tr("Review staged .library.json changes before applying these rules again. Live files have been kept."));return;}completed();
                        });
                    });
                });
            },false);
        });
    });
}
QVariantMap GitHubManager::conflictContext(const QString &path,const QString &side) const {
    if(!m_worktreeLeaseActive || !relativeFile(path) || (side!="current" && side!="incoming"))return {};
    const auto entry=m_conflictVersions.value(path).toMap();if(entry.isEmpty())return {};
    return {{"path",path},{"side",side},{"root",m_worktreeRoot},{"operationId",m_operationId},{"kind","loading"},{"bytes",0},{"diff",false},
        {"head",m_pinnedHead},{"incoming",m_incoming},{"gitDirectory",m_repository.value("gitDirectory")},
        {"exists",entry.value(side=="current"?"currentExists":"incomingExists")},{"blob",entry.value(side=="current"?"currentBlob":"incomingBlob")}};
}
void GitHubManager::previewError(const QString &path,const QString &side,const QString &message,bool change) {
    QVariantMap result{{"path",path},{"side",side},{"root",change?m_repository.value("localPath").toString():m_worktreeRoot},
        {"operationId",m_operationId},{"kind","error"},{"bytes",0},{"diff",false},{"error",message}};
    m_error=message;
    if(change){m_changePreview=result;emit changePreviewChanged();}else{m_conflictPreview=result;emit conflictPreviewChanged();}
    emit changed();
}
void GitHubManager::startPreviewTask(const QString &phase,std::function<QVariantMap(const std::shared_ptr<std::atomic_bool> &)> task) {
    m_previewReturnPhase=m_phase;m_previewPhase=phase;m_phase=phase;m_busy=true;m_error.clear();m_notice.clear();
    m_cancelPreview=std::make_shared<std::atomic_bool>(false);const auto cancel=m_cancelPreview;emit changed();
    m_previewWatcher.setFuture(QtConcurrent::run([task=std::move(task),cancel]{return task(cancel);}));
}
void GitHubManager::readConflictVersion(const QString &path,const QString &side) {
    if(m_busy)return;
    if(privateFile(m_worktreeRoot,path)){previewError(path,side,tr("Pilon private state cannot be previewed or exported through Git."));return;}
    const auto context=conflictContext(path,side);
    if(context.isEmpty()){previewError(path,side,tr("Choose a current or incoming file from the active merge conflicts."));return;}
    m_conflictPreview=publicPreview(context);
    startPreviewTask("reading-version",[context](const auto &cancel){return inspectVersion(context,{},cancel);});emit conflictPreviewChanged();
}
void GitHubManager::exportConflictVersion(const QString &path,const QString &side,const QString &destination) {
    if(m_busy)return;
    if(privateFile(m_worktreeRoot,path)){previewError(path,side,tr("Pilon private state cannot be previewed or exported through Git."));return;}
    const auto context=conflictContext(path,side);
    if(context.isEmpty() || !context.value("exists").toBool()){recordError(tr("Choose an existing current or incoming conflict version to save."));emit changed();return;}
    QString error;const auto target=exportTarget(destination,context,error);
    if(target.isEmpty()){recordError(error);emit changed();return;}
    startPreviewTask("exporting-version",[context,target](const auto &cancel){return inspectVersion(context,target,cancel);});
}
void GitHubManager::readChange(const QString &path) {
    if(m_busy)return;
    if(privateFile(m_repository.value("localPath").toString(),path)){previewError(path,{},tr("Pilon private state cannot be previewed through Git."),true);return;}
    QVariantMap entry;
    if(relativeFile(path))for(const auto &value:m_changes)if(value.toMap().value("path").toString()==path && !entry.value("status").toString().startsWith('D'))entry=value.toMap();
    const auto root=m_repository.value("localPath").toString();
    if(entry.isEmpty() || root.isEmpty() || entry.value("conflict").toBool()){previewError(path,{},tr("The selected file changed. Refresh Changes or review its conflict versions."),true);return;}
    QVariantMap context{{"path",path},{"root",root},{"operationId",m_operationId},{"kind","loading"},{"bytes",0},{"diff",false},
        {"head",m_repository.value("head")},{"status",entry.value("status")},{"previousPath",entry.value("previousPath")}};
    m_changePreview=publicPreview(context);
    startPreviewTask("reading-change",[context](const auto &cancel){return inspectChange(context,cancel);});emit changePreviewChanged();
}
void GitHubManager::setActiveProvider(const QString &id) {
    if (busy() || m_worktreeLeaseActive || id == m_activeProvider) return;
    for (const auto &value : m_providers) {
        const auto entry = value.toMap();
        if (entry.value("id") != id) continue;
        saveProviders();
        m_activeProvider = id; m_provider = entry.value("type").toString();
        m_host = entry.value("host").toString(); m_sshKey = entry.value("sshKey").toString(); m_account = entry.value("account").toString();
        m_repositories = readRepositoryCache(entry); m_repository.clear(); m_changes.clear(); m_error.clear(); m_notice.clear();
        saveProviders(); emit changed(); return;
    }
}
void GitHubManager::addProvider(const QString &type) {
    if (busy() || m_worktreeLeaseActive || !QStringList{"GitHub", "GitLab", "Custom"}.contains(type)) return;
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_providers.append(QVariantMap{{"id", id}, {"type", type},
        {"host", type == "GitHub" ? "github.com" : type == "GitLab" ? "gitlab.com" : ""}});
    setActiveProvider(id);
}
void GitHubManager::removeProvider(const QString &id) {
    if (busy() || m_worktreeLeaseActive) return;
    for (const auto &value:m_providers) {
        const auto entry=value.toMap();if(entry.value("id").toString()!=id)continue;
        if(!begin(false))return;
        const auto url=entry.value("credentialUrl").toString(),username=entry.value("credentialUsername").toString();
        if(url.isEmpty()){if(removeProviderRecord(id))finish();else fail(m_error);return;}
        QString error;const auto scope=GitCredentials::request(url,username,{},&error);
        if(scope.isEmpty()){fail(error);return;}
        changeCredential("erase",scope,scope,[this,id,scope](QByteArray backup){
            if(!removeProviderRecord(id)){rollbackCredential(scope,backup,m_error);return;}finish();
        });return;
    }
}
bool GitHubManager::removeProviderRecord(const QString &id) {
    auto providers=m_providers;
    providers.erase(std::remove_if(providers.begin(),providers.end(),[&id](const QVariant &value){return value.toMap().value("id").toString()==id;}),providers.end());
    const auto selected=id==m_activeProvider ? providers.isEmpty() ? QString{} : providers.first().toMap().value("id").toString() : m_activeProvider;
    if(!persistProviders(providers,selected))return false;
    m_providers=providers;
    QFile::remove(repositoryCachePath(id));
    if(id==m_activeProvider){
        QVariantMap entry;for(const auto &value:providers)if(value.toMap().value("id").toString()==selected)entry=value.toMap();
        m_activeProvider=selected;m_provider=entry.value("type","Custom").toString();m_host=entry.value("host").toString();m_sshKey=entry.value("sshKey").toString();m_account=entry.value("account").toString();
        m_repositories=readRepositoryCache(entry);m_repository.clear();m_changes.clear();
    }
    return true;
}
void GitHubManager::setProvider(const QString &provider) {
    if (busy() || m_worktreeLeaseActive || provider == m_provider) return;
    for (const auto &value : m_providers) {
        const auto entry = value.toMap();
        if (entry.value("type") == provider) { setActiveProvider(entry.value("id").toString()); return; }
    }
    addProvider(provider);
}
void GitHubManager::setHost(const QString &host) {
    if (busy() || m_worktreeLeaseActive) return;
    const auto clean = host.trimmed().toLower(); const QUrl url("https://" + clean);
    if (!clean.isEmpty() && (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || !url.path().isEmpty() || url.hasQuery() || url.hasFragment())) {
        fail(tr("Enter a host name, such as gitlab.example.com.")); return;
    }
    if (clean == m_host) return;
    QFile::remove(repositoryCachePath(m_activeProvider));
    m_host = clean;
    m_account.clear(); m_repositories.clear(); m_error.clear(); m_notice.clear(); saveProviders(); emit changed();
}
void GitHubManager::setSshKey(const QString &path) {
    if (busy() || m_worktreeLeaseActive) return;
    const auto clean = path.trimmed();
    if (!clean.isEmpty() && (!QDir::isAbsolutePath(clean) || !QFileInfo(clean).isFile())) { fail(tr("Choose an existing SSH key file.")); return; }
    m_sshKey = clean; m_error.clear(); saveProviders(); emit changed();
}
void GitHubManager::refreshAccount() {
    if(busy() || m_worktreeLeaseActive)return;
    if (!providerAvailable()) { fail(tr("Install %1 for account browsing, or use a repository URL.").arg(m_provider == "GitLab" ? "glab" : "github-cli")); return; }
    if (!begin()) return;
    m_phase = "refreshing-repositories"; emit changed();
    QStringList args{"api", "user"};
    if (m_provider == "GitLab" || m_host != "github.com") args += QStringList{"--hostname", m_host};
    run(m_provider == "GitLab" ? m_glab : m_gh, args, {}, [this](int code, const QByteArray &out, const QByteArray &err) {
        if (code) {
            if (err.contains("auth login") || err.contains("not logged") || err.contains("HTTP 401") || err.contains("not authenticated")) {
                QFile::remove(repositoryCachePath(m_activeProvider));
                m_account.clear(); m_repositories.clear(); finish();
            } else check(code, err);
            return;
        }
        const auto user = QJsonDocument::fromJson(out).object();
        const auto account = user.value(m_provider == "GitLab" ? "username" : "login").toString();
        if (account.isEmpty()) { fail(tr("The provider returned an invalid account.")); return; }
        if (account != m_account) {
            QFile::remove(repositoryCachePath(m_activeProvider));
            m_repositories.clear(); m_account = account; saveProviders(); emit changed();
        }
        run(m_git, {"config", "--global", "--get", "user.name"}, {}, [this](int, const QByteArray &out, const QByteArray &) {
            if (m_commitName.isEmpty()) m_commitName = QString::fromUtf8(out).trimmed();
            run(m_git, {"config", "--global", "--get", "user.email"}, {}, [this](int, const QByteArray &out, const QByteArray &) {
                if (m_commitEmail.isEmpty()) m_commitEmail = QString::fromUtf8(out).trimmed();
                loadRepositories();
            });
        });
    });
}
void GitHubManager::login() {
    if (!providerAvailable()) { refreshAccount(); return; }
    if (!begin()) return;
    m_authenticating = true; m_loginCode.clear(); m_loginUrl.clear(); emit changed();
    const QStringList args = m_provider == "GitLab"
        ? QStringList{"auth", "login", "--hostname", m_host, "--web", "--git-protocol", "https"}
        : QStringList{"auth", "login", "--hostname", m_host, "--git-protocol", "https", "--web", "--skip-ssh-key"};
    run(m_provider == "GitLab" ? m_glab : m_gh, args, {}, [this](int code, const QByteArray &, const QByteArray &err) {
        if (!check(code, err)) return;
        finish(); refreshAccount();
    });
    if (m_provider == "GitHub") m_process.write("\n");
}
void GitHubManager::readLoginOutput() {
    if (m_provider == "GitLab" && m_authenticating && m_loginUrl.isEmpty()) {
        const auto lines = (m_output + m_errors).split('\n');
        for (qsizetype i = 0; i + 1 < lines.size(); ++i) {
            if (!lines[i].startsWith("PILON_OPEN_URL=")) continue;
            const auto value = QString::fromUtf8(lines[i].sliced(15)).trimmed();
            const QUrl url(value, QUrl::StrictMode), host("https://" + m_host);
            const QUrlQuery query(url);
            if (!url.isValid() || url.scheme() != "https" || !url.userInfo().isEmpty()
                || url.host() != host.host() || url.port(443) != host.port(443)
                || url.path() != "/oauth/authorize" || url.hasFragment()
                || query.hasQueryItem("access_token") || query.hasQueryItem("refresh_token")
                || query.hasQueryItem("client_secret") || query.hasQueryItem("token")) {
                fail(tr("The provider returned an invalid sign-in URL.")); m_process.kill(); return;
            }
            m_loginUrl = value; openLogin(); return;
        }
        return;
    }
    if (m_provider != "GitHub" || !m_authenticating || !m_loginCode.isEmpty()) return;
    const auto match = QRegularExpression("\\b[A-Z0-9]{4}-[A-Z0-9]{4}\\b").match(QString::fromUtf8(m_output + m_errors));
    if (!match.hasMatch()) return;
    m_loginCode = match.captured(); emit changed(); openLogin();
}
void GitHubManager::readProcessOutput() {
    const auto output=m_process.readAllStandardOutput(),errors=m_process.readAllStandardError();
    if(m_outputExceeded)return;
    if(m_output.size()+output.size()>16*1024*1024 || m_errors.size()+errors.size()>65536){
        m_outputExceeded=true;m_output.clear();m_errors.clear();recordError(tr("Git returned too much output. Check the repository or provider response and try again."));
        if(m_process.state()!=QProcess::NotRunning)m_process.kill();return;
    }
    m_output+=output;m_errors+=errors;readLoginOutput();
}
void GitHubManager::openLogin() {
    const auto url = m_provider == "GitLab" ? QUrl(m_loginUrl) : QUrl("https://" + m_host + "/login/device");
    if (url.isValid() && !url.isEmpty()) QDesktopServices::openUrl(url);
}
void GitHubManager::logout() {
    if(m_provider=="Custom" && !credentialUrl().isEmpty()) {
        if(!begin(false))return;
        QString error;const auto scope=GitCredentials::request(credentialUrl(),credentialUsername(),{},&error);const auto id=m_activeProvider;
        if(scope.isEmpty()){fail(error);return;}
        changeCredential("erase",scope,scope,[this,id,scope](QByteArray backup){
            auto providers=m_providers;
            for(auto &value:providers){auto entry=value.toMap();if(entry.value("id").toString()!=id)continue;entry.remove("credentialUrl");entry.remove("credentialUsername");value=entry;}
            if(!persistProviders(providers,m_activeProvider)){rollbackCredential(scope,backup,m_error);return;}
            m_providers=providers;finish(tr("Signed out."));
        });return;
    }
    if (m_account.isEmpty() || !begin()) return;
    run(m_provider == "GitLab" ? m_glab : m_gh, m_provider == "GitLab"
        ? QStringList{"auth", "logout", "--hostname", m_host}
        : QStringList{"auth", "logout", "--hostname", m_host, "--user", m_account}, {},
        [this](int code, const QByteArray &, const QByteArray &err) {
            if (!check(code, err)) return;
            QFile::remove(repositoryCachePath(m_activeProvider));
            m_account.clear(); m_repositories.clear(); m_repository.clear(); m_changes.clear(); finish();
        });
    m_process.closeWriteChannel();
}
void GitHubManager::refreshRepositories() {
    refreshAccount();
}
void GitHubManager::loadRepositories() {
    const bool gitlab = m_provider == "GitLab";
    QStringList args = gitlab
        ? QStringList{"api", "--paginate", "--output", "ndjson", "--hostname", m_host, "projects?membership=true&per_page=100&order_by=last_activity_at"}
        : QStringList{"api", "--paginate", "--slurp", "user/repos?per_page=100&sort=pushed&affiliation=owner,collaborator,organization_member"};
    if (!gitlab && m_host != "github.com") args += QStringList{"--hostname", m_host};
    run(gitlab ? m_glab : m_gh, args, {}, [this, gitlab](int code, const QByteArray &out, const QByteArray &err) {
        if (!check(code, err)) return;
        QJsonArray values;
        if (gitlab) {
            for (const auto &line : out.split('\n')) {
                if (line.trimmed().isEmpty()) continue;
                QJsonParseError parse; const auto item = QJsonDocument::fromJson(line, &parse);
                if (parse.error != QJsonParseError::NoError) { fail(tr("The provider returned an invalid repository list.")); return; }
                if (item.isArray()) for (const auto &value : item.array()) values.append(value);
                else if (item.isObject()) values.append(item.object());
            }
        } else {
            QJsonParseError parse; const auto json = QJsonDocument::fromJson(out, &parse);
            if (parse.error != QJsonParseError::NoError || !json.isArray()) { fail(tr("The provider returned an invalid repository list.")); return; }
            for (const auto &page : json.array()) for (const auto &value : page.toArray()) values.append(value);
        }
        QVariantList repos;
        for (const auto &item : values) {
            const auto value = item.toObject(); const auto name = value.value(gitlab ? "path_with_namespace" : "full_name").toString();
            if (!validName(name)) continue;
            const auto remote = value.value(gitlab ? "http_url_to_repo" : "clone_url").toString("https://" + m_host + "/" + name + ".git");
            auto repository = remoteDetails(remote);
            repository["description"] = value.value("description").toString();
            repository["defaultBranch"] = value.value("default_branch").toString();
            repository["webUrl"] = value.value(gitlab ? "web_url" : "html_url").toString("https://" + m_host + "/" + name);
            repos.append(repository);
        }
        m_repositories = repos;
        NetworkSnapshot::write(repositoryCachePath(m_activeProvider), {{"host", m_host}, {"account", m_account},
            {"repositories", QJsonArray::fromVariantList(repos)}});
        finish();
    });
}
bool GitHubManager::validName(const QString &name) {
    static const QRegularExpression pattern("^[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*$");
    if (!pattern.match(name).hasMatch()) return false;
    for (const auto &part : name.split('/')) if (part == "." || part == "..") return false;
    return true;
}
QVariantMap GitHubManager::remoteDetails(const QString &remote) {
    const auto clean = remote.trimmed();
    if (clean.isEmpty() || clean.startsWith('-') || clean.contains('\n') || clean.contains('\r')) return {};
    QUrl url(clean); QString host, server, path;
    if (QDir::isAbsolutePath(clean)) path = clean;
    else if (!clean.contains("://")) {
        const auto match = QRegularExpression("^(?:[^@/]+@)?([^/:]+):(.+)$").match(clean);
        if (!match.hasMatch()) return {};
        host = match.captured(1); path = match.captured(2);
    } else {
        if (!url.isValid() || !QStringList{"https", "http", "ssh", "git", "file"}.contains(url.scheme()) || !url.password().isEmpty() || url.hasQuery() || url.hasFragment()) return {};
        if (url.scheme() != "file" && url.host().isEmpty()) return {};
        host = url.host(); path = url.path(); if (path.startsWith('/')) path.remove(0, 1);
    }
    server = host;
    if (!host.isEmpty() && url.port() > 0 && ((url.scheme() == "https" && url.port() != 443)
        || (url.scheme() == "http" && url.port() != 80) || (url.scheme() == "ssh" && url.port() != 22))) host += ":" + QString::number(url.port());
    if (path.endsWith(".git")) path.chop(4);
    if (!host.isEmpty() && !validName(path)) return {};
    const auto name = host.isEmpty() ? QFileInfo(path).fileName() : path;
    if (name.isEmpty()) return {};
    const auto webHost = url.scheme() == "ssh" ? server : host;
    return {{"name", name}, {"remote", clean}, {"host", host}, {"server", server}, {"webUrl", host.isEmpty() ? QString{} : "https://" + webHost + "/" + path}};
}
QString GitHubManager::repositoryName(const QString &remote) { return remoteDetails(remote).value("name").toString(); }
QVariantList GitHubManager::localRepositories() const {
    QVariantList result;
    for (const auto &value:m_localRepositories) result.append(value);
    return result;
}
QString GitHubManager::savedFolder(const QString &remote, const QString &preferred) const {
    if (!preferred.isEmpty() && m_localRepositories.value(preferred).toMap().value("remote")==remote) return preferred;
    for (const auto &value:m_localRepositories) {
        const auto entry=value.toMap();
        if (entry.value("remote")==remote) return entry.value("localPath").toString();
    }
    return {};
}
bool GitHubManager::rememberRepository() {
    const auto path=m_repository.value("localPath").toString();
    if (path.isEmpty()) return true;
    auto repositories=m_localRepositories;
    repositories[path]=QVariantMap{{"localPath",path},{"remote",m_repository.value("remote")},{"name",m_repository.value("name")}};
    PilonSettings settings; const auto previous=settings.value("Git/localRepositories");
    settings.setValue("Git/localRepositories",repositories); settings.sync();
    if (settings.status()!=PilonSettings::NoError) {
        if (previous.isValid()) settings.setValue("Git/localRepositories",previous); else settings.remove("Git/localRepositories");
        settings.sync(); fail(tr("Unable to save the repository association. Check your settings folder permissions and try again.")); return false;
    }
    m_localRepositories=repositories;
    return true;
}
void GitHubManager::selectRepository(const QString &name) {
    if (m_busy || repositorySwitchBlocked()) return;
    QVariantMap selected;
    for (const auto &repo : std::as_const(m_repositories)) if (repo.toMap().value("name").toString() == name) selected = repo.toMap();
    if (selected.isEmpty()) return;
    const auto previous=m_repository.value("localPath").toString();
    m_repository = selected; m_changes.clear(); m_error.clear(); m_notice.clear(); emit changed();
    QString path = savedFolder(selected.value("remote").toString(),previous);
    if (path.isEmpty() && m_host == "github.com") path=savedFolder(name);
    if (path.isEmpty()) path = QDir(m_cloneDirectory).filePath(name.section('/', -1));
    if (QFileInfo::exists(QDir(path).filePath(".git")) && begin()) inspectFolder(path);
}
void GitHubManager::useFolder(const QString &path) {
    if (m_repository.isEmpty()) { openFolder(path); return; }
    if (begin()) inspectFolder(path);
}
void GitHubManager::openFolder(const QString &path) {
    if (!begin()) return;
    inspectLocalFolder(path,false);
}
void GitHubManager::attachRepository(const QString &path) {
    if (!begin()) return;
    inspectLocalFolder(path,true);
}
void GitHubManager::inspectLocalFolder(const QString &path, bool requireRoot, std::function<void()> ready) {
    const QFileInfo entry(path);
    const QString directory = entry.isDir() ? entry.canonicalFilePath() : entry.canonicalPath();
    if (!entry.exists() || directory.isEmpty() || (requireRoot && !entry.isDir())) { fail(tr("Choose an existing repository folder.")); return; }
    run(m_git, {"-C", directory, "rev-parse", "--show-toplevel"}, {}, [this,directory,requireRoot,ready](int code, const QByteArray &out, const QByteArray &err) {
        if (code && err.contains("not a git repository") && !requireRoot) {
            m_changes.clear();
            if (m_pendingWorktrees.contains(directory)) {
                m_repository={{"localPath",directory},{"pendingOperation","reload"},{"reloadPending",true}};
                fail(tr("The unfinished update's Git repository is missing at %1. Restore its repository metadata before recovering; its files have been preserved.").arg(directory));return;
            }
            m_repository.clear();finish();return;
        }
        if (!check(code, err)) return;
        const QString root = QFileInfo(gitLine(out)).canonicalFilePath();
        if (root.isEmpty() || (requireRoot && root!=directory)) { fail(tr("Choose the repository's root folder.")); return; }
        run(m_git, {"-C", root, "remote", "get-url", "origin"}, {}, [this, root,ready](int code, const QByteArray &out, const QByteArray &) {
            const auto remote = code ? QString{} : gitLine(out);
            auto repository = remoteDetails(remote);
            if (repository.isEmpty()) repository["name"] = QFileInfo(root).fileName();
            repository["localPath"] = root; repository["remote"] = remote;
            m_repository = repository;
            loadStatus(ready);
        });
    });
}
void GitHubManager::selectRemote(const QString &url) {
    if (m_busy || repositorySwitchBlocked()) return;
    const auto repository = remoteDetails(url);
    if (repository.isEmpty()) { fail(tr("Enter a repository URL using HTTPS, SSH, or a local path.")); return; }
    const auto previous=m_repository.value("localPath").toString();
    m_repository = repository; m_changes.clear(); m_error.clear(); m_notice.clear(); emit changed();
    const auto path = savedFolder(repository.value("remote").toString(),previous);
    if (!path.isEmpty() && QFileInfo::exists(QDir(path).filePath(".git")) && begin()) inspectFolder(path);
}
void GitHubManager::saveRemote(const QString &url) {
    if(busy() || m_worktreeLeaseActive)return;
    if (m_repository.value("localPath").toString().isEmpty()) { selectRemote(url); return; }
    const auto details = remoteDetails(url);
    if (details.isEmpty()) { fail(tr("Enter a valid repository URL.")); return; }
    if (!localReady()) return;
    const auto path = m_repository.value("localPath").toString();
    git({"remote", "get-url", "origin"}, [this, details, path](int code, const QByteArray &, const QByteArray &) {
        git({"remote", code ? "add" : "set-url", "origin", details.value("remote").toString()}, [this, details, path](int code, const QByteArray &, const QByteArray &err) {
            if (!check(code, err)) return;
            m_repository = details; m_repository["localPath"] = path;
            m_notice = tr("Remote saved."); loadStatus();
        });
    });
}
void GitHubManager::inspectFolder(const QString &path, std::function<void()> ready) {
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty() || !QFileInfo(canonical).isDir()) { fail(tr("The repository folder does not exist.")); return; }
    run(m_git, {"-C", canonical, "rev-parse", "--show-toplevel"}, {}, [this, canonical, ready](int code, const QByteArray &out, const QByteArray &err) {
        if (!check(code, err)) return;
        if (QFileInfo(gitLine(out)).canonicalFilePath() != canonical) { fail(tr("Choose the repository's root folder.")); return; }
        run(m_git, {"-C", canonical, "remote", "get-url", "origin"}, {}, [this, canonical, ready](int code, const QByteArray &out, const QByteArray &err) {
            const auto expected = remoteDetails(m_repository.value("remote").toString());
            const auto actual = remoteDetails(code ? QString{} : gitLine(out));
            if (expected.value("server") != actual.value("server") || expected.value("name") != actual.value("name")
                || (QUrl(expected.value("remote").toString()).scheme() == QUrl(actual.value("remote").toString()).scheme() && expected.value("host") != actual.value("host"))
                || (expected.value("host").toString().isEmpty() && expected.value("remote") != actual.value("remote"))) {
                fail(tr("That folder belongs to a different repository remote.")); return;
            }
            m_repository["localPath"] = canonical;
            loadStatus(ready);
        });
    });
}
void GitHubManager::initializeRepository(const QString &path, const QString &branch) {
    if (!begin()) return;
    const QFileInfo folder(path);
    const auto canonical=folder.canonicalFilePath();
    if (!QDir::isAbsolutePath(path) || !folder.isDir() || canonical.isEmpty()) { fail(tr("Choose an existing absolute folder path.")); return; }
    if(privateFolders(canonical).contains(QString{})){fail(tr("Choose a repository outside Pilon's private state folder."));return;}
    if (branch.isEmpty() || branch.startsWith('-') || branch.startsWith("@{-")) { fail(tr("Enter a valid initial branch name.")); return; }
    run(m_git,{"check-ref-format","--branch",branch},{},[this,canonical,branch](int code,const QByteArray &,const QByteArray &err) {
        if (!check(code,err)) return;
        run(m_git,{"-C",canonical,"rev-parse","--git-dir"},{},[this,canonical,branch](int code,const QByteArray &,const QByteArray &err) {
            if (!code) { fail(tr("This folder is already inside a Git repository. Attach its root folder instead.")); return; }
            if (!err.contains("not a git repository")) { check(code,err); return; }
            m_pinnedHead.clear(); m_incoming.clear();
            requestWorktreeUpdate(canonical,[this,canonical,branch] {
                if (!savePendingWorktree("init")) { fail(m_error); return; }
                run(m_git,{"init","--initial-branch",branch,"--",canonical},{},[this,canonical](int code,const QByteArray &,const QByteArray &err) {
                    if (!check(code,err)) return;
                    m_notice=tr("Repository initialized. Review its files before committing.");
                    inspectLocalFolder(canonical,true,[this]{endWorktreeUpdate("applied");});
                });
            });
        });
    });
}
void GitHubManager::cloneRepository() {
    cloneRepositoryTo(QDir(m_cloneDirectory).filePath(m_repository.value("name").toString().section('/',-1)));
}
void GitHubManager::cloneRepositoryTo(const QString &destination) {
    if (m_repository.isEmpty() || !begin()) return;
    if (remoteDetails(m_repository.value("remote").toString()).isEmpty()) { fail(tr("Choose a valid repository URL before cloning.")); return; }
    const auto path=QDir::cleanPath(destination);
    const QFileInfo folder(path);
    if (!QDir::isAbsolutePath(path) || folder.isSymLink()
        || (folder.exists() && (!folder.isDir() || !QDir(path).entryList(QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot).isEmpty()))) {
        fail(tr("Choose an empty or new absolute folder for the clone.")); return;
    }
    if (!QDir().mkpath(path)) { fail(tr("Could not create the clone folder.")); return; }
    const auto canonical=QFileInfo(path).canonicalFilePath();
    if(privateFolders(canonical).contains(QString{})){fail(tr("Choose a repository outside Pilon's private state folder."));return;}
    m_pinnedHead.clear(); m_incoming.clear();
    requestWorktreeUpdate(canonical,[this,canonical] {
        if (!QDir(canonical).entryList(QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot).isEmpty()) { fail(tr("The clone folder is no longer empty.")); return; }
        if (!savePendingWorktree("clone")) { fail(m_error); return; }
        m_repository["localPath"]=canonical;
        m_fileUpdatesStarted=true; m_phase="cloning"; emit changed();
        git({"clone","--",m_repository.value("remote").toString(),canonical},[this,canonical](int code,const QByteArray &,const QByteArray &err) {
            if (!check(code,err)) return;
            m_notice=tr("Repository cloned.");
            inspectFolder(canonical,[this]{requestValidation(m_worktreeRoot,[this]{endWorktreeUpdate("applied");});});
        });
    });
}
bool GitHubManager::localReady() {
    if (m_busy) return false;
    if (m_repository.value("localPath").toString().isEmpty()) { fail(tr("Clone this repository or choose its existing folder first.")); return false; }
    return begin();
}
void GitHubManager::refreshStatus() {
    if (m_worktreeLeaseActive) {
        if (m_busy) return;
        m_busy=true; loadStatus([this]{m_busy=false;emit changed();}); return;
    }
    if (localReady()) inspectFolder(m_repository.value("localPath").toString());
}
bool GitHubManager::protectPrivateFolders(const QString &root) {
    if(privateFolders(root).contains(QString{})){fail(tr("Choose a repository outside Pilon's private state folder."));return false;}
    QString error;
    if(!GitManagedFiles::applyLocalExcludes(root,locallyExcludedFolders(root),&error)){fail(error);return false;}
    return true;
}
bool GitHubManager::verifyPrivateFiles(const QStringList &additionalRevisions) {
    const auto root=m_repository.value("localPath").toString();
    QString error;auto revisions=additionalRevisions;revisions.append(m_repository.value("head").toString());revisions.removeDuplicates();
    if(!GitManagedFiles::verifyUntrackedFolders(root,privateFolders(root),revisions,&error)){fail(error);return false;}
    return true;
}
void GitHubManager::loadStatus(std::function<void()> ready) {
    const auto root=m_repository.value("localPath").toString();
    if(!protectPrivateFolders(root))return;
    const auto privatePaths=privateFolders(root),excludedPaths=locallyExcludedFolders(root);
    git({"status","--porcelain=v2","--branch","-z","--untracked-files=all"},[this,ready,privatePaths,excludedPaths](int code,const QByteArray &out,const QByteArray &err) {
        if (!check(code, err)) return;
        m_repository["branch"]=QString{}; m_repository["head"]=QString{}; m_repository["upstream"]=QString{};
        m_repository["unborn"]=false; m_repository["detached"]=false;
        m_repository["ahead"]=0; m_repository["behind"]=0;
        bool indexDirty=false,worktreeDirty=false,conflicts=false;
        QVariantList changes;
        const auto entries=out.split('\0');
        for (qsizetype i=0;i<entries.size();++i) {
            const auto &entry=entries[i];
            if (entry.startsWith("# branch.oid ")) {
                const auto head=entry.mid(13);
                m_repository["unborn"]=head=="(initial)";
                m_repository["head"]=head=="(initial)" ? QString{} : QString::fromLatin1(head);
            } else if (entry.startsWith("# branch.head ")) {
                const auto branch=entry.mid(14);
                m_repository["detached"]=branch=="(detached)";
                m_repository["branch"]=branch=="(detached)" ? QString{} : QString::fromUtf8(branch);
            } else if (entry.startsWith("# branch.upstream ")) m_repository["upstream"]=QString::fromUtf8(entry.mid(18));
            else if (entry.startsWith("# branch.ab ")) {
                const auto counts=entry.split(' ');
                if (counts.size()==4) { m_repository["ahead"]=counts[2].mid(1).toInt(); m_repository["behind"]=counts[3].mid(1).toInt(); }
            } else if (entry.startsWith("? ")) {
                const auto path=QString::fromUtf8(entry.mid(2));
                if(inFolders(path,excludedPaths))continue;
                changes.append(QVariantMap{{"status","??"},{"path",path}});
                worktreeDirty=true;
            } else if (entry.startsWith("1 ") || entry.startsWith("2 ") || entry.startsWith("u ")) {
                const int fields=entry[0]=='1' ? 8 : entry[0]=='2' ? 9 : 10;
                qsizetype offset=0;
                for (int field=0;field<fields && offset>=0;++field) { offset=entry.indexOf(' ',offset); if (offset>=0) ++offset; }
                if (offset<0) { fail(tr("Git returned an invalid file status.")); return; }
                auto status=entry.mid(2,2); status.replace('.',' ');
                QVariantMap change{{"status",QString::fromLatin1(status)},{"path",QString::fromUtf8(entry.mid(offset))},{"conflict",entry[0]=='u'}};
                if (entry[0]=='u') {
                    const auto fields=entry.left(offset-1).split(' ');
                    change["currentExists"]=fields.value(4)!="000000";
                    change["incomingExists"]=fields.value(5)!="000000";
                    change["currentBlob"]=QString::fromLatin1(fields.value(8));change["incomingBlob"]=QString::fromLatin1(fields.value(9));
                }
                if (entry[0]=='2') {
                    if (++i>=entries.size()) { fail(tr("Git returned an incomplete rename status.")); return; }
                    change["previousPath"]=QString::fromUtf8(entries[i]);
                }
                indexDirty=indexDirty || status[0]!=' '; worktreeDirty=worktreeDirty || status[1]!=' ';
                conflicts=conflicts || entry[0]=='u';
                if(!inFolders(change.value("path").toString(),privatePaths) && !inFolders(change.value("previousPath").toString(),privatePaths))changes.append(change);
            }
        }
        m_changes=changes; m_repository["dirty"]=!changes.isEmpty();
        m_repository["indexDirty"]=indexDirty; m_repository["worktreeDirty"]=worktreeDirty; m_repository["hasConflicts"]=conflicts;
        if (m_worktreeLeaseActive) for (const auto &value:changes) {
            const auto entry=value.toMap();const auto path=entry.value("path").toString();
            if (entry.value("conflict").toBool()) {
                if(!m_conflictVersions.contains(path))m_conflictVersions[path]=entry;
                else {auto retained=m_conflictVersions.value(path).toMap();for(const auto *key:{"currentBlob","incomingBlob"})if(!retained.contains(key))retained[key]=entry.value(key);m_conflictVersions[path]=retained;}
            }
        }
        git({"rev-parse","--absolute-git-dir"},[this,ready](int code,const QByteArray &out,const QByteArray &err) {
            if (!check(code,err)) return;
            const auto directory=QFileInfo(gitLine(out)).canonicalFilePath();
            if (directory.isEmpty()) { fail(tr("The Git repository moved or disappeared. Open its folder again.")); return; }
            m_repository["gitDirectory"]=directory;
            const auto exists=[&directory](const QString &name) { return QFileInfo::exists(QDir(directory).filePath(name)); };
            m_repository["mergeInProgress"]=exists("MERGE_HEAD");
            QString operation;
            if (exists("rebase-merge")) operation="rebase";
            else if (exists("rebase-apply")) operation=exists("rebase-apply/rebasing") ? "rebase" : "am";
            else if (exists("MERGE_HEAD")) operation="merge";
            else if (exists("CHERRY_PICK_HEAD")) operation="cherry-pick";
            else if (exists("REVERT_HEAD")) operation="revert";
            const bool reloadPending=m_pendingWorktrees.contains(m_repository.value("localPath").toString());
            m_repository["reloadPending"]=reloadPending;
            if (operation.isEmpty() && reloadPending) operation="reload";
            m_repository["pendingOperation"]=operation;
            if (!rememberRepository()) return;
            if (ready) ready(); else finish();
        });
    });
}
QString GitHubManager::canonicalPath(const QString &path) const { return QFileInfo(path).canonicalFilePath(); }
QString GitHubManager::folderRecoveryError(const QString &path) const {return GitFolderGuard::blockedReason(path,path);}
QVariantList GitHubManager::conflicts() const {
    QVariantList result;
    if (m_worktreeLeaseActive && !m_conflictVersions.isEmpty()) {
        for (auto it=m_conflictVersions.cbegin();it!=m_conflictVersions.cend();++it) {
            auto entry=it.value().toMap();
            const bool unresolved=std::any_of(m_changes.cbegin(),m_changes.cend(),[&it](const QVariant &value){const auto change=value.toMap();return change.value("conflict").toBool() && change.value("path").toString()==it.key();});
            entry["resolved"]=!unresolved;result.append(entry);
        }
        return result;
    }
    for (const auto &entry:m_changes) if (entry.toMap().value("conflict").toBool()) result.append(entry);
    return result;
}
QVariantList GitHubManager::pendingWorktrees() const {
    QVariantList result;
    for (auto it=m_pendingWorktrees.cbegin();it!=m_pendingWorktrees.cend();++it) {
        auto entry=it.value().toMap(); entry["localPath"]=it.key(); result.append(entry);
    }
    return result;
}
bool GitHubManager::repositorySwitchBlocked() {
    if (!m_worktreeLeaseActive) return false;
    recordError(tr("Finish or recover the current repository update before switching repositories.")); emit changed(); return true;
}
bool GitHubManager::storePendingWorktrees(const QVariantMap &records) {
    PilonSettings settings; const auto previous=settings.value("Git/pendingWorktrees");
    if (records.isEmpty()) settings.remove("Git/pendingWorktrees"); else settings.setValue("Git/pendingWorktrees",records);
    settings.sync();
    if (settings.status()!=PilonSettings::NoError) {
        if (previous.isValid()) settings.setValue("Git/pendingWorktrees",previous); else settings.remove("Git/pendingWorktrees");
        settings.sync(); recordError(tr("Unable to save Git recovery state. Check your settings folder permissions and try again.")); return false;
    }
    m_pendingWorktrees=records; return true;
}
bool GitHubManager::savePendingWorktree(const QString &action,const QString &outcome,const QVariantMap &details) {
    auto records=m_pendingWorktrees, entry=records.value(m_worktreeRoot).toMap();
    for(auto it=details.cbegin();it!=details.cend();++it)entry[it.key()]=it.value();
    if (!entry.contains("headBefore")) entry["headBefore"]=m_pinnedHead;
    if (!action.isEmpty()) entry["action"]=action;
    if (!m_incoming.isEmpty()) entry["incoming"]=m_incoming;
    if (!m_conflictVersions.isEmpty()) entry["conflicts"]=m_conflictVersions;
    if (outcome=="applied" || outcome=="aborted") entry["headAfter"]=m_repository.value("head").toString();
    entry["outcome"]=outcome;
    records[m_worktreeRoot]=entry;
    return storePendingWorktrees(records);
}
void GitHubManager::requestWorktreeUpdate(const QString &root,std::function<void()> ready) {
    m_worktreeRoot=root; ++m_operationId;
    m_worktreeLeaseActive=true; m_awaitingReload=false; m_fileUpdatesStarted=false; m_cancelled=false;
    m_policyAwaitingReload=false;m_pushAfterReload=false;
    m_conflictVersions=m_pendingWorktrees.value(root).toMap().value("conflicts").toMap();
    m_managedFolders.clear(); m_afterPreparation=std::move(ready); m_phase="preparing";
    if (!isSignalConnected(QMetaMethod::fromSignal(&GitHubManager::prepareWorktreeUpdate))) {
        m_worktreeLeaseActive=false; m_afterPreparation={};
        fail(tr("The workspace update guard is unavailable. Reopen Pilon before changing this repository.")); return;
    }
    emit changed(); emit prepareWorktreeUpdate(m_operationId,m_worktreeRoot);
}
void GitHubManager::resumeWorktreeUpdate(quint64 id,bool allowed,const QStringList &folders,const QString &reason) {
    if (id!=m_operationId || !m_worktreeLeaseActive || m_phase!="preparing") return;
    if (!allowed) {
        m_afterPreparation={}; m_worktreeLeaseActive=false; m_busy=false; m_phase="failed";
        recordError(reason.isEmpty() ? tr("Save or finish the current work before updating this folder.") : reason);
        emit changed(); return;
    }
    m_managedFolders=folders;
    auto next=std::exchange(m_afterPreparation,{}); if (next) next();
}
void GitHubManager::requestValidation(const QString &snapshotRoot,std::function<void()> ready) {
    m_afterValidation=std::move(ready); m_phase="validating";
    if (!isSignalConnected(QMetaMethod::fromSignal(&GitHubManager::validateWorktreeUpdate))) {
        fail(tr("The workspace validator is unavailable. The folder remains paused.")); return;
    }
    emit changed(); emit validateWorktreeUpdate(m_operationId,m_worktreeRoot,snapshotRoot);
}
void GitHubManager::validateWorktreeUpdateResult(quint64 id,bool allowed,const QString &reason) {
    if (id!=m_operationId || !m_worktreeLeaseActive || m_phase!="validating") return;
    if (!allowed) { fail(reason.isEmpty() ? tr("The incoming folder data is invalid. Review it before continuing.") : reason); return; }
    auto next=std::exchange(m_afterValidation,{}); if (next) next();
}
void GitHubManager::endWorktreeUpdate(const QString &outcome) {
    m_afterPreparation={}; m_afterValidation={};
    if (m_pendingWorktrees.contains(m_worktreeRoot)) savePendingWorktree({},outcome);
    const bool failedNewClone=m_managedFolders.isEmpty() && m_pendingWorktrees.value(m_worktreeRoot).toMap().value("action").toString()=="clone" && (outcome=="failed" || outcome=="cancelled");
    if(failedNewClone)m_error+=tr(" Any downloaded files in %1 have been kept. Retry in a new empty folder.").arg(m_worktreeRoot);
    m_awaitingReload=outcome!="conflicted" && (failedNewClone || !m_fileUpdatesStarted || outcome=="applied" || outcome=="aborted");
    m_phase=m_awaitingReload ? "awaiting-reload" : outcome=="conflicted" ? "conflicted" : "recovery";
    m_busy=m_awaitingReload;
    emit changed(); emit worktreeUpdateFinished(m_operationId,m_worktreeRoot,outcome);
}
bool GitHubManager::finishWorktreeUpdate(quint64 id,bool succeeded,const QString &reason) {
    if (id!=m_operationId || !m_worktreeLeaseActive || !m_awaitingReload) return false;
    if (!succeeded) {
        m_pushAfterReload=false;
        recordError(reason.isEmpty() ? tr("The updated folder could not be reloaded. It remains paused.") : reason);
        m_phase="reload-failed"; m_busy=false; emit changed(); return false;
    }
    const auto pending=m_pendingWorktrees.value(m_worktreeRoot).toMap();
    const auto outcome=pending.value("outcome").toString();
    const bool failedNewClone=m_managedFolders.isEmpty() && pending.value("action").toString()=="clone"
        && (outcome=="failed" || outcome=="cancelled" || (outcome.isEmpty() && m_repository.value("gitDirectory").toString().isEmpty() && !m_error.isEmpty()));
    if (m_pendingWorktrees.contains(m_worktreeRoot)) {
        auto records=m_pendingWorktrees; records.remove(m_worktreeRoot);
        if (!storePendingWorktrees(records)) { m_pushAfterReload=false;m_phase="reload-failed"; m_busy=false; emit changed(); return false; }
    }
    m_worktreeLeaseActive=false; m_awaitingReload=false; m_busy=false; m_fileUpdatesStarted=false;
    if(failedNewClone){
        for(const auto *key:{"localPath","gitDirectory","head","branch","upstream","dirty","indexDirty","worktreeDirty","hasConflicts","mergeInProgress","pendingOperation"})m_repository.remove(key);
        m_changes.clear();
    }
    m_repository["reloadPending"]=false;
    if (m_repository.value("pendingOperation").toString()=="reload") m_repository["pendingOperation"]=QString{};
    m_temporaryIndex.clear();m_snapshot.reset(); m_managedFolders.clear(); m_conflictVersions.clear();m_phase="idle";
    const bool publish=std::exchange(m_pushAfterReload,false);
    const bool policy=std::exchange(m_policyAwaitingReload,false);
    if (publish) {m_busy=true;m_phase="pushing";QTimer::singleShot(0,this,[this]{if(m_busy && !m_cancelled && m_phase=="pushing")pushAfterUpdate();});}
    emit changed();if(policy)emit managedPolicyApplied();return true;
}
void GitHubManager::verifyPinnedState(std::function<void()> ready,bool clean) {
    loadStatus([this,ready,clean] {
        if (m_repository.value("head").toString()!=m_pinnedHead) { fail(tr("The repository changed while the update was being prepared. Review it and try again.")); return; }
        if(!verifyPrivateFiles())return;
        if (clean && m_repository.value("dirty").toBool()) { fail(tr("Commit or move your local changes before synchronizing.")); return; }
        if (ready) ready();
    });
}
void GitHubManager::verifyMergeState(std::function<void()> ready) {
    loadStatus([this,ready]{
        if(!m_repository.value("mergeInProgress").toBool() || m_repository.value("head").toString()!=m_pinnedHead){rejectRepositoryDrift(tr("The repository HEAD or merge changed outside Pilon. Review it before continuing."));return;}
        git({"rev-parse","--verify","MERGE_HEAD"},[this,ready](int code,const QByteArray &out,const QByteArray &err){
            if(!check(code,err))return;
            if(gitLine(out)!=m_incoming){rejectRepositoryDrift(tr("The incoming merge changed outside Pilon. Review it before continuing."));return;}
            ready();
        });
    });
}
void GitHubManager::rejectRepositoryDrift(const QString &message) {
    m_busy=false;m_awaitingReload=false;m_phase="recovery";recordError(message);m_afterPreparation={};m_afterValidation={};emit changed();
}
void GitHubManager::fetchCurrent(std::function<void()> ready) {
    if (m_repository.value("remote").toString().isEmpty()) { fail(tr("Set a repository remote before fetching.")); return; }
    m_phase="fetching"; emit changed();
    git({"fetch","--no-recurse-submodules","--no-tags","origin"},[this,ready](int code,const QByteArray &,const QByteArray &err) {
        if (check(code,err)) loadStatus(ready);
    });
}
void GitHubManager::fetch() {
    if (!localReady()) return;
    inspectFolder(m_repository.value("localPath").toString(),[this]{fetchCurrent([this]{finish(tr("Remote changes fetched."));});});
}
void GitHubManager::sync() {
    if (!localReady()) return;
    inspectFolder(m_repository.value("localPath").toString(),[this] {
        if(!verifyPrivateFiles())return;
        if (!m_repository.value("pendingOperation").toString().isEmpty()) { fail(tr("Recover the unfinished repository operation before synchronizing.")); return; }
        if (m_repository.value("dirty").toBool()) { fail(tr("Commit or move your local changes before synchronizing.")); return; }
        if (m_repository.value("branch").toString().isEmpty()) { fail(tr("Check out a branch before synchronizing.")); return; }
        fetchCurrent([this]{prepareSync();});
    });
}
void GitHubManager::prepareSync() {
    m_pinnedHead=m_repository.value("head").toString();
    const auto upstream=m_repository.value("upstream").toString();
    if (!upstream.isEmpty() && !upstream.startsWith("origin/")) { fail(tr("Choose an upstream on origin before synchronizing this branch.")); return; }
    const auto reference=upstream.isEmpty() ? "refs/remotes/origin/"+m_repository.value("branch").toString() : "refs/remotes/"+upstream;
    git({"rev-parse","--verify",reference+"^{commit}"},[this](int code,const QByteArray &out,const QByteArray &) {
        if (code) {
            if (m_pinnedHead.isEmpty()) finish(tr("There are no commits to synchronize."));
            else {m_incoming.clear();preparePublication();}
            return;
        }
        m_incoming=gitLine(out);
        if(!verifyPrivateFiles({m_incoming}))return;
        const auto prepare=[this] {
            requestWorktreeUpdate(m_repository.value("localPath").toString(),[this] {
                verifyPinnedState([this] {
                    QString error;
                    if (!GitManagedFiles::verifyAttributes(m_worktreeRoot,m_managedFolders,&error)) { fail(error); return; }
                    git({"config","--name-only","--get-regexp","^merge\\..*\\.driver$"},[this](int code,const QByteArray &out,const QByteArray &err) {
                        if (code!=0 && code!=1) { check(code,err); return; }
                        if (!out.trimmed().isEmpty()) { fail(tr("Custom Git merge drivers require review before this repository can be synchronized.")); return; }
                        materializeIncoming();
                    });
                });
            });
        };
        if (m_pinnedHead.isEmpty()) { m_updateKind="ff"; prepare(); return; }
        git({"merge-base","--is-ancestor",m_incoming,m_pinnedHead},[this,prepare](int code,const QByteArray &,const QByteArray &err) {
            if (!code) { preparePublication(); return; }
            if (code!=1) { check(code,err); return; }
            git({"merge-base","--is-ancestor",m_pinnedHead,m_incoming},[this,prepare](int code,const QByteArray &,const QByteArray &err) {
                if (code!=0 && code!=1) { check(code,err); return; }
                m_updateKind=code==0 ? "ff" : "merge";
                if(m_updateKind=="merge" && (m_commitName.isEmpty() || m_commitEmail.isEmpty())){fail(tr("Set your commit name and email in Repository settings before merging divergent changes."));return;}
                prepare();
            });
        });
    });
}
void GitHubManager::preparePublication() {
    requestWorktreeUpdate(m_repository.value("localPath").toString(),[this] {
        verifyPinnedState([this] {
            QString error;
            if (!GitManagedFiles::verifyAttributes(m_worktreeRoot,m_managedFolders,&error)) {fail(error);return;}
            requestValidation(m_worktreeRoot,[this]{pushAfterUpdate();});
        });
    });
}
void GitHubManager::materializeIncoming() {
    m_snapshot=std::make_unique<QTemporaryDir>();
    if (!m_snapshot->isValid()) { fail(tr("Unable to create the Git validation snapshot.")); return; }
    m_cancelSnapshot=std::make_shared<std::atomic_bool>(false);
    m_phase="materializing"; emit changed();
    const auto repository=m_worktreeRoot,revision=m_incoming,destination=m_snapshot->path();
    const auto folders=m_managedFolders; const auto cancel=m_cancelSnapshot;
    m_snapshotWatcher.setFuture(QtConcurrent::run([repository,revision,destination,folders,cancel] {
        QString error; GitManagedFiles::materializeTree(repository,revision,folders,destination,&error,cancel); return error;
    }));
}
void GitHubManager::applyIncoming() {
    if (!savePendingWorktree(m_updateKind)) { fail(m_error); return; }
    m_fileUpdatesStarted=true; m_phase="merging"; emit changed();
    const QStringList arguments=m_updateKind=="ff" ? QStringList{"merge","--ff-only","--no-edit",m_incoming}
        : QStringList{"merge","--no-ff","--no-commit","--no-edit",m_incoming};
    git(arguments,[this](int code,const QByteArray &,const QByteArray &err) {
        const auto error=QString::fromUtf8(err).trimmed();
        loadStatus([this,code,error] {
            if (m_repository.value("hasConflicts").toBool()) { recordError(tr("Resolve the conflicts before completing synchronization.")); endWorktreeUpdate("conflicted"); return; }
            if (code) { fail(error); return; }
            requestValidation(m_worktreeRoot,[this] {
                if (m_updateKind=="merge") commitMerge(); else pushAfterUpdate();
            });
        });
    });
}
void GitHubManager::pushAfterUpdate() {
    if(!verifyPrivateFiles())return;
    if (m_worktreeLeaseActive) {
        m_publicationHead=m_repository.value("head").toString();m_publicationBranch=m_repository.value("branch").toString();
        git({"remote","get-url","--push","origin"},[this](int code,const QByteArray &out,const QByteArray &err){
            if(!check(code,err))return;
            const auto address=gitLine(out);
            const auto expected=remoteDetails(m_repository.value("remote").toString());
            const auto actual=remoteDetails(address);
            if(actual.isEmpty() || expected.value("server")!=actual.value("server") || expected.value("host")!=actual.value("host") || expected.value("name")!=actual.value("name")
                || (expected.value("server").toString().isEmpty() && expected.value("remote")!=actual.value("remote"))){fail(tr("The configured push URL differs from the selected repository. Review origin before publishing."));return;}
            m_publicationRemote=address;m_pushAfterReload=true;endWorktreeUpdate("applied");
        });return;
    }
    m_phase="pushing"; emit changed();
    verifyPublication([this]{
        const auto temporary="pilon-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
        git({"-c","remote."+temporary+".url="+m_publicationRemote,"-c","remote."+temporary+".fetch=+refs/heads/*:refs/remotes/origin/*",
            "push",temporary,m_publicationHead+":refs/heads/"+m_publicationBranch},[this](int code,const QByteArray &,const QByteArray &err){
            if(!check(code,err))return;
            verifyPublication([this]{
                git({"branch","--set-upstream-to=origin/"+m_publicationBranch,m_publicationBranch},[this](int code,const QByteArray &,const QByteArray &err){
                    if(!check(code,err))return;
                    loadStatus([this]{finish(tr("Repository synchronized."));});
                });
            });
        });
    });
}
void GitHubManager::verifyPublication(std::function<void()> ready) {
    loadStatus([this,ready]{
        if(m_publicationHead.isEmpty() || m_repository.value("head").toString()!=m_publicationHead || m_repository.value("branch").toString()!=m_publicationBranch){fail(tr("The repository HEAD or branch changed after validation. Review it before publishing."));return;}
        git({"remote","get-url","--push","origin"},[this,ready](int code,const QByteArray &out,const QByteArray &err){
            if(!check(code,err))return;
            if(gitLine(out)!=m_publicationRemote){fail(tr("The repository push URL changed after validation. Review it before publishing."));return;}
            ready();
        });
    });
}
void GitHubManager::pull() {
    sync();
}
void GitHubManager::ensureMergeLease(std::function<void()> ready) {
    if (m_busy) return;
    if (m_worktreeLeaseActive) { m_busy=true;m_error.clear();m_notice.clear();ready();return; }
    if (!localReady()) return;
    inspectFolder(m_repository.value("localPath").toString(),[this,ready] {
        m_pinnedHead=m_repository.value("head").toString();m_incoming.clear();
        requestWorktreeUpdate(m_repository.value("localPath").toString(),[this,ready] {
            const auto record=m_pendingWorktrees.value(m_worktreeRoot).toMap();
            if (!record.isEmpty()) {m_pinnedHead=record.value("headBefore").toString();m_incoming=record.value("incoming").toString();}
            if(!record.isEmpty() && !QStringList{"commit","init","policy"}.contains(record.value("action").toString()))m_fileUpdatesStarted=true;
            if (!m_repository.value("mergeInProgress").toBool()) {ready();return;}
            git({"rev-parse","--verify","MERGE_HEAD"},[this,ready](int code,const QByteArray &out,const QByteArray &err) {
                if (!check(code,err)) return;
                if(!m_incoming.isEmpty() && gitLine(out)!=m_incoming){rejectRepositoryDrift(tr("The incoming merge changed outside Pilon. Review it before recovering."));return;}
                m_incoming=gitLine(out);m_updateKind="merge";m_fileUpdatesStarted=true;ready();
            });
        });
    });
}
void GitHubManager::recoverMerge() {
    ensureMergeLease([this] {
        loadStatus([this] {
            const auto record=m_pendingWorktrees.value(m_worktreeRoot).toMap();
            if (!record.isEmpty()) {
                m_pinnedHead=record.value("headBefore").toString();m_incoming=record.value("incoming").toString();m_updateKind=record.value("action").toString();
            }
            if (m_repository.value("mergeInProgress").toBool()) {
                if (m_repository.value("head").toString()!=m_pinnedHead) { rejectRepositoryDrift(tr("The repository HEAD changed during the unfinished merge. Review it outside Pilon before recovering."));return; }
                git({"rev-parse","--verify","MERGE_HEAD"},[this](int code,const QByteArray &out,const QByteArray &err) {
                    if (!check(code,err)) return;
                    if(!m_incoming.isEmpty() && gitLine(out)!=m_incoming){rejectRepositoryDrift(tr("The incoming merge changed outside Pilon. Review it before recovering."));return;}
                    m_incoming=gitLine(out);m_updateKind="merge";
                    if (!savePendingWorktree("merge","conflicted")) { fail(m_error);return; }
                    m_fileUpdatesStarted=true;endWorktreeUpdate("conflicted");
                });
                return;
            }
            if (record.isEmpty()) { fail(tr("There is no unfinished Pilon update or Git merge in this folder."));return; }
            const auto action=record.value("action").toString(),outcome=record.value("outcome").toString();
            const auto head=m_repository.value("head").toString();
            if(action=="commit-index") {
                m_fileUpdatesStarted=true;
                const auto target=record.value("targetHead").toString();const auto paths=record.value("selectedPaths").toStringList();
                if(head==m_pinnedHead) {requestValidation(m_worktreeRoot,[this]{endWorktreeUpdate("applied");});return;}
                if(!target.isEmpty() && head==target && !paths.isEmpty()) {
                    if(!record.value("indexRefreshPending").toBool()){requestValidation(m_worktreeRoot,[this]{endWorktreeUpdate("applied");});return;}
                    m_fileUpdatesStarted=true;refreshCommittedIndex();return;
                }
                rejectRepositoryDrift(tr("The repository does not match the interrupted selected commit. Review its history before recovering."));return;
            }
            const bool noCheckout=action=="commit" || action=="init" || action=="policy";
            const bool completed=((outcome=="applied" || outcome=="aborted") && record.value("headAfter").toString()==head)
                || (action=="ff" && head==m_incoming) || action=="clone";
            m_fileUpdatesStarted=!noCheckout;
            if (!noCheckout && m_repository.value("dirty").toBool()) {
                fail(tr("The interrupted update is not in a completed state. Preserve its files and finish or repair it with Git before retrying recovery."));return;
            }
            const auto validate=[this]{requestValidation(m_worktreeRoot,[this]{endWorktreeUpdate("applied");});};
            if (noCheckout || completed) {validate();return;}
            if (action=="merge" && !head.isEmpty()) {
                git({"rev-list","--parents","-1","HEAD"},[this,validate](int code,const QByteArray &out,const QByteArray &err) {
                    if (!check(code,err)) return;
                    const auto parents=gitLine(out).split(' ');
                    if (parents.size()==3 && parents[1]==m_pinnedHead && parents[2]==m_incoming) {validate();return;}
                    fail(tr("The repository does not match the interrupted merge. Review its history before recovering."));
                });return;
            }
            fail(tr("The interrupted update is not in a completed state. Preserve its files and finish or repair it with Git before retrying recovery."));
        });
    });
}
void GitHubManager::continueMerge() {
    ensureMergeLease([this] {
        loadStatus([this] {
            if (!m_repository.value("mergeInProgress").toBool()) { m_busy=false;recoverMerge();return; }
            verifyMergeState([this]{
                if (m_repository.value("hasConflicts").toBool()) { recordError(tr("Resolve all conflicts before completing the merge."));endWorktreeUpdate("conflicted");return; }
                git({"diff","--quiet","--"},[this](int code,const QByteArray &,const QByteArray &err) {
                    if (code==1) { recordError(tr("Stage your resolved changes before completing the merge."));endWorktreeUpdate("conflicted");return; }
                    if (!check(code,err)) return;
                    requestValidation(m_worktreeRoot,[this]{commitMerge();});
                });
            });
        });
    });
}
void GitHubManager::commitMerge() {
    if (m_commitName.isEmpty() || m_commitEmail.isEmpty()) { fail(tr("Set your commit name and email in Repository settings."));return; }
    verifyMergeState([this] {
        if(!verifyPrivateFiles({m_incoming}))return;
        if (m_repository.value("hasConflicts").toBool() || m_repository.value("worktreeDirty").toBool()) { fail(tr("The resolved files changed during validation. Stage and review them before continuing."));return; }
        if (!savePendingWorktree("merge")) { fail(m_error);return; }
        m_fileUpdatesStarted=true;m_phase="committing";emit changed();
        git({"-c","user.name="+m_commitName,"-c","user.email="+m_commitEmail,"commit","--no-edit"},[this](int code,const QByteArray &,const QByteArray &err) {
            if (!check(code,err)) return;
            loadStatus([this]{pushAfterUpdate();});
        });
    });
}
void GitHubManager::abortMerge() {
    ensureMergeLease([this] {
        verifyMergeState([this] {
            if (!savePendingWorktree("merge")) { fail(m_error);return; }
            m_fileUpdatesStarted=true;m_phase="aborting";emit changed();
            git({"merge","--abort"},[this](int code,const QByteArray &,const QByteArray &err) {
                if (!check(code,err)) return;
                loadStatus([this]{requestValidation(m_worktreeRoot,[this]{endWorktreeUpdate("aborted");});});
            });
        });
    });
}
void GitHubManager::resolveConflict(const QString &path,const QString &side) {
    if (side!="current" && side!="incoming") { recordError(tr("Choose the current or incoming version of this file."));emit changed();return; }
    ensureMergeLease([this,path,side] {
        verifyMergeState([this,path,side] {
            QVariantMap selected;
            for (const auto &entry:conflicts()) if (entry.toMap().value("path").toString()==path) selected=entry.toMap();
            if (selected.isEmpty() || !m_repository.value("mergeInProgress").toBool()) { recordError(tr("This file is not part of the current merge conflicts."));endWorktreeUpdate("conflicted");return; }
            if (selected.value("resolved").toBool() && selected.value("selectedSide").toString()==side
                && !selected.value(side=="current" ? "currentExists" : "incomingExists").toBool()
                && !QFileInfo::exists(QDir(m_worktreeRoot).filePath(path))) {endWorktreeUpdate("conflicted");return;}
            if (!savePendingWorktree("merge","conflicted")) { fail(m_error);return; }
            m_fileUpdatesStarted=true;m_phase="resolving";emit changed();
            const auto complete=[this,path,side](int code,const QByteArray &,const QByteArray &err) {
                if (!check(code,err)) return;
                auto entry=m_conflictVersions.value(path).toMap();entry["selectedSide"]=side;m_conflictVersions[path]=entry;
                loadStatus([this]{endWorktreeUpdate("conflicted");});
            };
            if (!selected.value("resolved").toBool() && !selected.value(side=="current" ? "currentExists" : "incomingExists").toBool()) {
                git({"rm","--",path},complete);return;
            }
            const auto source=side=="current" ? m_pinnedHead : m_incoming;
            git({"restore","--source="+source,"--staged","--worktree","--",path},complete);
        });
    });
}
void GitHubManager::cancel() {
    if (!m_busy || m_awaitingReload || m_phase=="credentials" || m_credentialsWatcher.isRunning()) return;
    if(m_phase=="reading-version" || m_phase=="exporting-version" || m_phase=="reading-change"){if(m_cancelPreview)m_cancelPreview->store(true);return;}
    m_cancelled=true;recordError(tr("Git operation cancelled."));
    if(m_phase=="preparing") {
        m_afterPreparation={};m_afterValidation={};m_worktreeLeaseActive=false;m_awaitingReload=false;m_busy=false;m_phase="failed";
        emit changed();emit worktreeUpdateFinished(m_operationId,m_worktreeRoot,"preparation-cancelled");return;
    }
    if (m_snapshotWatcher.isRunning()) { if(m_cancelSnapshot)m_cancelSnapshot->store(true);return; }
    m_temporaryIndex.clear();m_afterPreparation={};m_afterValidation={};m_done={};m_timeout.stop();
    if (m_process.state()!=QProcess::NotRunning) {
        m_process.kill();
        // Complete only after the child exits, so a new operation cannot reuse its process.
        m_done=[this](int,const QByteArray &,const QByteArray &){if(m_worktreeLeaseActive)endWorktreeUpdate("cancelled");else{m_busy=false;m_phase="failed";emit changed();}};
    } else if (m_worktreeLeaseActive) endWorktreeUpdate("cancelled");
    else {m_busy=false;m_phase="failed";emit changed();}
}
void GitHubManager::push() {
    if (!localReady()) return;
    inspectFolder(m_repository.value("localPath").toString(), [this] {
        if(!verifyPrivateFiles())return;
        const QString branch = m_repository.value("branch").toString();
        if (branch.isEmpty()) { fail(tr("Check out a branch before pushing.")); return; }
        if (!m_repository.value("pendingOperation").toString().isEmpty()) {fail(tr("Recover the unfinished repository update before pushing."));return;}
        if (m_repository.value("dirty").toBool()) {fail(tr("Commit or move your local changes before pushing."));return;}
        m_pinnedHead=m_repository.value("head").toString();m_incoming.clear();preparePublication();
    });
}
void GitHubManager::commit(const QString &message, const QStringList &paths) {
    if (!localReady()) return;
    inspectFolder(m_repository.value("localPath").toString(), [this, message, paths] {
        if(std::any_of(paths.cbegin(),paths.cend(),[this](const auto &path){return privateFile(m_repository.value("localPath").toString(),path);})){fail(tr("Pilon private state cannot be committed through Git."));return;}
        if(!verifyPrivateFiles())return;
        if (!m_repository.value("pendingOperation").toString().isEmpty()) { fail(tr("Recover the unfinished repository operation before committing."));return; }
        if (message.trimmed().isEmpty() || paths.isEmpty()) { fail(tr("Select files and enter a commit message.")); return; }
        if (m_commitName.isEmpty() || m_commitEmail.isEmpty()) { fail(tr("Set your commit name and email in Settings.")); return; }
        m_pinnedHead=m_repository.value("head").toString();m_incoming.clear();
        requestWorktreeUpdate(m_repository.value("localPath").toString(),[this,message,paths] {
            verifyPinnedState([this,message,paths] {
                requestValidation(m_worktreeRoot,[this,message,paths]{commitSelected(message,paths);});
            },false);
        });
    });
}
void GitHubManager::commitSelected(const QString &message,const QStringList &paths) {
    if(!verifyPrivateFiles())return;
    QStringList selected, untracked, cachedDeletions;
    for (const auto &path : paths) {
        QVariantMap match;
        for (const auto &entry : std::as_const(m_changes)) if (entry.toMap().value("path").toString() == path) {
            match=entry.toMap();
            const QFileInfo file(QDir(m_worktreeRoot).filePath(path));
            if(match.value("status").toString().startsWith('D') && (file.exists() || file.isSymLink()))cachedDeletions.append(path);
        }
        if (match.isEmpty()) { fail(tr("The selected files have changed. Refresh the repository and try again.")); return; }
        selected.append(path);
        if (match.value("status").toString() == "??") untracked.append(path);
        if (!match.value("previousPath").toString().isEmpty()) selected.append(match.value("previousPath").toString());
    }
    selected.removeDuplicates();
    if(!cachedDeletions.isEmpty()){commitWithCachedDeletions(message,selected,cachedDeletions);return;}
    if (!savePendingWorktree("commit")) { fail(m_error);return; }
    m_phase="committing";emit changed();
    const auto commitSelected = [this, selected, message] {
        QStringList args{"-c", "user.name=" + m_commitName, "-c", "user.email=" + m_commitEmail,
                         "commit", "--only", "--message", message, "--"};
        args += selected;
        git(args, [this](int code, const QByteArray &, const QByteArray &err) {
            if (!check(code, err)) return;
            m_notice = tr("Selected changes committed."); loadStatus([this]{endWorktreeUpdate("applied");});
        });
    };
    if (untracked.isEmpty()) commitSelected();
    else {
        QStringList args{"add", "--intent-to-add", "--"}; args += untracked;
        git(args, [this, commitSelected](int code, const QByteArray &, const QByteArray &err) { if (check(code, err)) commitSelected(); });
    }
}
void GitHubManager::commitWithCachedDeletions(const QString &message,const QStringList &selected,const QStringList &deletions) {
    m_snapshot=std::make_unique<QTemporaryDir>();
    const auto temporary=QFileInfo(m_snapshot->path()).canonicalFilePath();
    if(!m_snapshot->isValid() || temporary==m_worktreeRoot || temporary.startsWith(m_worktreeRoot+'/')){fail(tr("Unable to create a selected-commit index outside the repository."));return;}
    if(!savePendingWorktree("commit-index",{},{{"selectedPaths",selected},{"indexRefreshPending",true}})){fail(m_error);return;}
    m_phase="committing";emit changed();
    git({"write-tree"},[this,message,selected,deletions](int code,const QByteArray &out,const QByteArray &err){
        if(!check(code,err))return;
        if(!savePendingWorktree({}, {},{{"indexBeforeTree",gitLine(out)}})){fail(m_error);return;}
        m_temporaryIndex=m_snapshot->filePath("index");stageSelectedCommit(message,selected,deletions);
    });
}
void GitHubManager::stageSelectedCommit(const QString &message,const QStringList &selected,const QStringList &deletions) {
    git(m_pinnedHead.isEmpty()?QStringList{"read-tree","--empty"}:QStringList{"read-tree",m_pinnedHead},[this,message,selected,deletions](int code,const QByteArray &,const QByteArray &err){
        if(!check(code,err))return;
        auto working=selected;for(const auto &path:deletions)working.removeAll(path);
        const auto removeCached=[this,message,selected,deletions]{
            QStringList arguments{"rm","--cached","--ignore-unmatch","--"};arguments+=deletions;
            git(arguments,[this,message,selected](int code,const QByteArray &,const QByteArray &err){
                if(!check(code,err))return;
                writeSelectedCommit(message,selected);
            });
        };
        if(working.isEmpty()){removeCached();return;}
        QStringList arguments{"add","--all","--"};arguments+=working;
        git(arguments,[this,removeCached](int code,const QByteArray &,const QByteArray &err){if(check(code,err))removeCached();});
    });
}
void GitHubManager::writeSelectedCommit(const QString &message,const QStringList &selected) {
    git({"write-tree"},[this,message,selected](int code,const QByteArray &out,const QByteArray &err){
        if(!check(code,err))return;
        m_temporaryIndex.clear();
        const auto tree=gitLine(out);
        git({"config","--bool","--get","commit.gpgSign"},[this,message,selected,tree](int code,const QByteArray &out,const QByteArray &err){
            if(code!=0 && code!=1){check(code,err);return;}
            QStringList arguments{"-c","user.name="+m_commitName,"-c","user.email="+m_commitEmail,"commit-tree",tree};
            if(code==0 && out.trimmed()=="true")arguments.append("-S");
            if(!m_pinnedHead.isEmpty())arguments+=QStringList{"-p",m_pinnedHead};
            arguments+=QStringList{"-m",message};
            git(arguments,[this,selected](int code,const QByteArray &out,const QByteArray &err){
                if(!check(code,err))return;
                prepareIndexRefresh(gitLine(out),selected);
            });
        });
    });
}
void GitHubManager::prepareIndexRefresh(const QString &target,const QStringList &selected) {
    m_temporaryIndex=m_snapshot->filePath("refresh-index");
    const auto before=m_pendingWorktrees.value(m_worktreeRoot).toMap().value("indexBeforeTree").toString();
    git({"read-tree",before},[this,target,selected](int code,const QByteArray &,const QByteArray &err){
        if(!check(code,err))return;
        QStringList arguments{"reset","--quiet",target,"--"};arguments+=selected;
        git(arguments,[this,target,selected](int code,const QByteArray &,const QByteArray &err){
            if(!check(code,err))return;
            git({"write-tree"},[this,target](int code,const QByteArray &out,const QByteArray &err){
                if(!check(code,err))return;
                m_temporaryIndex.clear();
                if(!savePendingWorktree({}, {},{{"targetHead",target},{"indexAfterTree",gitLine(out)}})){fail(m_error);return;}
                m_fileUpdatesStarted=true;
                git({"update-ref","-m","commit: Pilon selected files","HEAD",target,m_pinnedHead},[this](int code,const QByteArray &,const QByteArray &err){
                    if(check(code,err))refreshCommittedIndex();
                });
            });
        });
    });
}
void GitHubManager::refreshCommittedIndex() {
    const auto record=m_pendingWorktrees.value(m_worktreeRoot).toMap();
    const auto target=record.value("targetHead").toString(),before=record.value("indexBeforeTree").toString(),after=record.value("indexAfterTree").toString();
    if(target.isEmpty() || before.isEmpty() || after.isEmpty()){rejectRepositoryDrift(tr("The selected-commit index snapshot is unavailable. Preserve your staged changes and review the repository before recovering."));return;}
    git({"rev-parse","--verify","HEAD"},[this,target,before,after](int code,const QByteArray &out,const QByteArray &err){
        if(!check(code,err))return;
        if(gitLine(out)!=target){rejectRepositoryDrift(tr("The repository HEAD changed before its selected index could be refreshed."));return;}
        git({"read-tree","-m","-i",before,after},[this,target](int code,const QByteArray &,const QByteArray &err){
            if(code){rejectRepositoryDrift(tr("The selected index could not be refreshed. Preserve your staged changes and retry recovery after reviewing them.\n%1").arg(QString::fromUtf8(err).trimmed().left(2000)));return;}
            loadStatus([this,target]{
                if(m_repository.value("head").toString()!=target){rejectRepositoryDrift(tr("The repository HEAD changed while its selected index was being refreshed."));return;}
                m_fileUpdatesStarted=false;
                if(!savePendingWorktree({}, {},{{"indexRefreshPending",false}})){fail(m_error);return;}
                requestValidation(m_worktreeRoot,[this]{m_notice=tr("Selected changes committed.");endWorktreeUpdate("applied");});
            });
        });
    });
}
void GitHubManager::openOnGitHub() {
    const QUrl url(m_repository.value("webUrl").toString());
    if (url.isValid() && !url.host().isEmpty() && url.scheme() == "https") QDesktopServices::openUrl(url);
}
