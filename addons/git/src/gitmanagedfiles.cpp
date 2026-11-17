#include "gitmanagedfiles.h"
#include "../platform/android/androidgit.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace {
constexpr qsizetype maximumMetadata = 16 * 1024 * 1024;
constexpr qsizetype maximumErrors = 65536;
constexpr int validationTimeout = 30000;
bool fail(QString *error, const QString &text) { if (error) *error = text; return false; }
bool safeRelative(const QString &path, bool allowEmpty = false) {
    if (path.isEmpty()) return allowEmpty;
    if (QDir::isAbsolutePath(path) || path.contains(QChar::Null)) return false;
    for (const auto &part : path.split('/')) if (part.isEmpty() || part == "." || part == ".." || part == ".git") return false;
    return true;
}
bool validFolders(const QStringList &folders, QString *error) {
    for (const auto &folder : folders)
        if (!safeRelative(folder, true)) return fail(error, QStringLiteral("Managed folders must be inside the repository."));
    return true;
}
bool git(const QString &directory, const QStringList &arguments, QByteArray *output, QString *error,
         const QByteArray &input = {}, const std::shared_ptr<std::atomic_bool> &cancel = {},
         QIODevice *sink = nullptr, int timeout = validationTimeout, qint64 maximumBytes = -1,
         qint64 *streamed = nullptr) {
    if (output) output->clear();
    if (streamed) *streamed = 0;
    if (cancel && cancel->load()) return fail(error, QStringLiteral("Git validation was cancelled."));
    if (timeout <= 0) return fail(error, QStringLiteral("Git validation timed out."));
#ifdef Q_OS_ANDROID
    const auto executable = PilonWorkspace::packagedExecutable("git");
#else
    const auto executable = QStandardPaths::findExecutable("git");
#endif
    if (executable.isEmpty()) return fail(error, QStringLiteral("Install Git to synchronize this folder."));
    QProcess process; process.setWorkingDirectory(directory);
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto &key : {"GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
                            "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_NAMESPACE"}) environment.remove(key);
    environment.insert("GIT_TERMINAL_PROMPT", "0"); environment.insert("GIT_OPTIONAL_LOCKS", "0");
    environment.insert("GIT_NO_LAZY_FETCH", "1");
#ifdef Q_OS_ANDROID
    const auto preparationError = AndroidGit::prepareEnvironment(environment, false);
    if (!preparationError.isEmpty()) return fail(error, preparationError);
    AndroidGit::prepareRepositoryEnvironment(environment, directory);
#endif
    process.setProcessEnvironment(environment);
    QString stopped;
    qsizetype errorBytes = 0;
    qint64 written = 0;
    QObject::connect(&process, &QProcess::readyReadStandardOutput, &process, [&] {
        const auto bytes = process.readAllStandardOutput();
        if (!stopped.isEmpty()) return;
        if (cancel && cancel->load()) stopped = QStringLiteral("Git validation was cancelled.");
        else if (sink) {
            if (maximumBytes >= 0 && bytes.size() > maximumBytes - written)
                stopped = QStringLiteral("This file is too large to preview.");
            else if (sink->write(bytes) != bytes.size()) stopped = QStringLiteral("The Git file could not be written.");
            else written += bytes.size();
        } else if (!output || output->size() + bytes.size() > maximumMetadata)
            stopped = QStringLiteral("Git returned too much metadata. Reduce the managed folders and try again.");
        else *output += bytes;
        if (!stopped.isEmpty()) process.kill();
    });
    QObject::connect(&process, &QProcess::readyReadStandardError, &process, [&] {
        errorBytes += process.readAllStandardError().size();
        if (errorBytes > maximumErrors) {
            stopped = QStringLiteral("Git returned too much error output. Check the repository and try again.");
            process.kill();
        }
    });
    QElapsedTimer timer; timer.start();
    process.start(executable, QStringList{"-c", "core.hooksPath=/dev/null", "--literal-pathspecs"} + arguments);
    if (!process.waitForStarted(qMin(3000, timeout))) return fail(error, QStringLiteral("Git could not be started."));
    process.write(input); process.closeWriteChannel();
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(100);
        if ((cancel && cancel->load()) || timer.elapsed() >= timeout) {
            process.kill(); process.waitForFinished(1000);
            return fail(error, cancel && cancel->load() ? QStringLiteral("Git validation was cancelled.") : QStringLiteral("Git validation timed out."));
        }
    }
    if (!stopped.isEmpty()) return fail(error, stopped);
    if (cancel && cancel->load()) return fail(error, QStringLiteral("Git validation was cancelled."));
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode())
        return fail(error, QStringLiteral("Git could not read the repository snapshot."));
    if (streamed) *streamed = written;
    return true;
}
bool validOid(const QString &oid) {
    return QRegularExpression("^(?:[a-f0-9]{40}|[a-f0-9]{64})$").match(oid).hasMatch();
}
QString quoted(const QString &value) {
    QByteArray result = "\"";
    for (const auto ch : value.toUtf8()) {
        const auto byte = static_cast<unsigned char>(ch);
        if (ch == '\\' || ch == '"') { result += '\\'; result += ch; }
        else if (byte < 32 || byte == 127) result += '\\' + QByteArray::number(byte, 8).rightJustified(3, '0');
        else result += ch;
    }
    return QString::fromUtf8(result + '"');
}
QStringList patterns(const QStringList &folders) {
    QStringList result;
    for (auto folder : folders) {
        QString literal;
        for (const auto ch : folder) {
            if (QStringView(u"\\*?[]").contains(ch)) literal += '\\';
            literal += ch;
        }
        folder = literal.isEmpty() ? "/" : "/" + literal + "/";
        for (const auto &suffix : {"**/*.pin", "**/*.pical", "**/*.pilon-event", "**/.pilon-calendar-deletions.json",
                                   "**/.folders.json", "**/.library.json", "**/.settings.json"})
            result << quoted(folder + suffix) + " -merge";
    }
    result.removeDuplicates(); return result;
}
bool validIgnoreFolders(const QStringList &folders,QString *error) {
    if(!validFolders(folders,error))return false;
    for(const auto &folder:folders)
        if(folder.contains('\n') || folder.contains('\r') || QString::fromUtf8(folder.toUtf8())!=folder)
            return fail(error,QStringLiteral("Git ignore rules cannot represent managed folder names with line breaks or invalid text."));
    return true;
}
QStringList ignorePatterns(const QStringList &folders) {
    QStringList result;
    for(const auto &folder:folders) {
        QString literal;
        for(const auto ch:folder) {if(QStringView(u"\\*?[] !#").contains(ch))literal+='\\';literal+=ch;}
        result << '/' + (literal.isEmpty() ? QString() : literal+'/') + ".library.json";
    }
    result.removeDuplicates();return result;
}
bool appendRules(const QString &path,const QStringList &rules,const QString &description,QString *error) {
    if(QFileInfo(path).isSymLink())return fail(error,description+QStringLiteral(" cannot be a symbolic link."));
    QByteArray data;QFile existing(path);
    if(existing.exists()) {
        if(!existing.open(QIODevice::ReadOnly))return fail(error,description+QStringLiteral(" could not be read."));
        if(existing.size()>maximumMetadata)return fail(error,description+QStringLiteral(" are too large to update safely."));
        data=existing.readAll();
    }
    const auto original=data;
    for(const auto &line:rules) {
        if(data.split('\n').contains(line.toUtf8()))continue;
        if(!data.isEmpty() && !data.endsWith('\n'))data+='\n';
        data+=line.toUtf8()+'\n';
    }
    if(original==data)return true;
    QSaveFile file(path);
    if(!file.open(QIODevice::WriteOnly) || file.write(data)!=data.size() || !file.commit())
        return fail(error,description+QStringLiteral(" could not be saved."));
    return true;
}
}

bool GitManagedFiles::blobAt(const QString &repository, const QString &revision, const QString &relativePath,
                             QString *oid, qint64 *size, QString *error, const std::shared_ptr<std::atomic_bool> &cancel) {
    if (error) error->clear();
    if (oid) oid->clear();
    if (size) *size = 0;
    if (!validOid(revision) || !safeRelative(relativePath) || QString::fromUtf8(relativePath.toUtf8()) != relativePath)
        return fail(error, QStringLiteral("A pinned Git revision and valid file path are required."));
    QByteArray output;
    if (!git(repository, {"ls-tree", "-lz", "--full-tree", revision, "--", relativePath}, &output, error, {}, cancel)) return false;
    if (output.isEmpty()) return true;
    const auto entries = output.split('\0');
    if (entries.size() != 2 || !entries.last().isEmpty()) return fail(error, QStringLiteral("Git returned an invalid file entry."));
    const auto tab = entries.first().indexOf('\t');
    const auto header = entries.first().first(qMax<qsizetype>(0, tab)).simplified().split(' ');
    if (tab < 0 || entries.first().sliced(tab + 1) != relativePath.toUtf8() || header.size() != 4
        || (header[0] != "100644" && header[0] != "100755") || header[1] != "blob")
        return fail(error, QStringLiteral("Only regular Git files can be previewed or saved."));
    const auto identity = QString::fromLatin1(header[2]);bool validSize = false;
    const auto bytes = header[3].toLongLong(&validSize);
    if (!validOid(identity) || !validSize || bytes < 0) return fail(error, QStringLiteral("Git returned invalid file metadata."));
    if (oid) *oid = identity;
    if (size) *size = bytes;
    return true;
}

bool GitManagedFiles::copyBlob(const QString &repository, const QString &oid, QIODevice *sink, qint64 maximumBytes,
                               qint64 *size, QString *error, const std::shared_ptr<std::atomic_bool> &cancel) {
    if (error) error->clear();
    if (size) *size = 0;
    if (!validOid(oid) || !sink || !sink->isWritable() || maximumBytes < -1)
        return fail(error, QStringLiteral("A valid Git file and writable destination are required."));
    QByteArray type, metadata;
    if (!git(repository, {"cat-file", "-t", oid}, &type, error, {}, cancel)) return false;
    if (type.trimmed() != "blob") return fail(error, QStringLiteral("Only regular Git files can be previewed or saved."));
    if (!git(repository, {"cat-file", "-s", oid}, &metadata, error, {}, cancel)) return false;
    bool validSize = false; const auto bytes = metadata.trimmed().toLongLong(&validSize);
    if (!validSize || bytes < 0) return fail(error, QStringLiteral("Git returned an invalid file size."));
    if (size) *size = bytes;
    if (maximumBytes >= 0 && bytes > maximumBytes) return fail(error, QStringLiteral("This file is too large to preview. Save the version to inspect it."));
    qint64 written = 0;
    if (!git(repository, {"cat-file", "blob", oid}, nullptr, error, {}, cancel, sink, validationTimeout, bytes, &written)) return false;
    if (written != bytes) return fail(error, QStringLiteral("Git returned an incomplete file."));
    if (size) *size = bytes;
    return true;
}

bool GitManagedFiles::verifyMergeRefs(const QString &repository, const QString &expectedHead, const QString &expectedMerge,
                                      QString *error, const std::shared_ptr<std::atomic_bool> &cancel) {
    if (error) error->clear();
    if (!validOid(expectedHead) || !validOid(expectedMerge)) return fail(error, QStringLiteral("Pinned merge versions are required."));
    QByteArray head, mergePath;
    if (!git(repository, {"rev-parse", "--verify", "HEAD"}, &head, error, {}, cancel)
        || !git(repository, {"rev-parse", "--git-path", "MERGE_HEAD"}, &mergePath, error, {}, cancel)) return false;
    const auto path = QString::fromUtf8(mergePath.trimmed());
    if (path.isEmpty()) return fail(error, QStringLiteral("The merge is no longer available."));
    QFile merge(QDir(repository).absoluteFilePath(path));
    if (!merge.open(QIODevice::ReadOnly) || merge.size() > 256)
        return fail(error, QStringLiteral("The merge is no longer available."));
    if (head.trimmed() != expectedHead.toLatin1() || merge.readAll().trimmed() != expectedMerge.toLatin1())
        return fail(error, QStringLiteral("The merge changed outside Pilon. Review the repository before continuing."));
    return true;
}

bool GitManagedFiles::diffFile(const QString &repository, const QString &head, const QStringList &paths, bool cached,
                              QIODevice *sink, qint64 maximumBytes, QString *error, const std::shared_ptr<std::atomic_bool> &cancel) {
    if (error) error->clear();
    if ((!head.isEmpty() && !validOid(head)) || (head.isEmpty() && !cached) || paths.isEmpty()
        || !sink || !sink->isWritable() || maximumBytes < 0)
        return fail(error, QStringLiteral("A valid Git change and preview destination are required."));
    for (const auto &path : paths)
        if (!safeRelative(path) || QString::fromUtf8(path.toUtf8()) != path)
            return fail(error, QStringLiteral("The selected Git path is unsupported."));
    QStringList arguments{"diff", "--no-ext-diff", "--no-textconv", "--no-color"};
    if (cached) arguments << "--cached";
    if (!head.isEmpty()) arguments << head;
    arguments << "--"; arguments << paths;
    return git(repository, arguments, nullptr, error, {}, cancel, sink, validationTimeout, maximumBytes);
}

QString GitManagedFiles::proposedAttributes(const QStringList &relativeFolders) {
    if (!validFolders(relativeFolders, nullptr)) return {};
    const auto lines = patterns(relativeFolders);
    return lines.isEmpty() ? QString() : lines.join('\n') + '\n';
}
QString GitManagedFiles::proposedIgnores(const QStringList &relativeFolders) {
    if(!validIgnoreFolders(relativeFolders,nullptr))return {};
    const auto lines=ignorePatterns(relativeFolders);return lines.isEmpty() ? QString() : lines.join('\n')+'\n';
}
bool GitManagedFiles::applyIgnoreRules(const QString &repository,const QStringList &relativeFolders,QString *error) {
    if(error)error->clear();
    if(!validIgnoreFolders(relativeFolders,error))return false;
    return appendRules(QDir(repository).filePath(".gitignore"),ignorePatterns(relativeFolders),QStringLiteral("Repository ignore rules"),error);
}
bool GitManagedFiles::applyLocalExcludes(const QString &repository,const QStringList &relativeFolders,QString *error) {
    if(error)error->clear();
    if(relativeFolders.isEmpty())return true;
    if(!validIgnoreFolders(relativeFolders,error) || relativeFolders.contains(QString{}))
        return fail(error,QStringLiteral("Private folders must be below the repository root."));
    QByteArray output;
    if(!git(repository,{"rev-parse","--git-path","info/exclude"},&output,error))return false;
    if(output.endsWith('\n'))output.chop(1);
    const auto name=QString::fromUtf8(output);const auto path=QDir(repository).absoluteFilePath(name);
    if(name.isEmpty() || name.toUtf8()!=output || QFileInfo(QFileInfo(path).absolutePath()).isSymLink())
        return fail(error,QStringLiteral("Local Git exclusions must use a regular repository metadata directory."));
    if(!QDir().mkpath(QFileInfo(path).absolutePath()))return fail(error,QStringLiteral("Local Git exclusions could not be created."));
    QStringList rules;
    for(const auto &folder:relativeFolders) {
        QString literal;
        for(const auto ch:folder){if(QStringView(u"\\*?[] !#").contains(ch))literal+='\\';literal+=ch;}
        rules << '/'+literal+'/';
    }
    rules.removeDuplicates();
    return appendRules(path,rules,QStringLiteral("Local Git exclusions"),error);
}
bool GitManagedFiles::verifyUntrackedFolders(const QString &repository,const QStringList &relativeFolders,
                                            const QStringList &revisions,QString *error) {
    if(error)error->clear();
    if(relativeFolders.isEmpty())return true;
    if(!validFolders(relativeFolders,error) || relativeFolders.contains(QString{}))
        return fail(error,QStringLiteral("Private folders must be below the repository root."));
    QByteArray output;QStringList arguments{"ls-files","--cached","-z","--"};arguments+=relativeFolders;
    if(!git(repository,arguments,&output,error))return false;
    const auto tracked=[error]{return fail(error,QStringLiteral("This repository tracks Pilon private state. Use a new repository or remove its private paths from the index and reachable history with Git before committing or synchronizing. Pilon has kept its files unchanged."));};
    if(!output.isEmpty())return tracked();
    for(const auto &revision:revisions) {
        if(revision.isEmpty())continue;
        if(!validOid(revision))return fail(error,QStringLiteral("A pinned Git revision is required to check private paths."));
        arguments={"--no-replace-objects","rev-list","--full-history","--max-count=1","--objects","--no-object-names",revision,"--"};arguments+=relativeFolders;
        if(!git(repository,arguments,&output,error))return false;
        if(!output.isEmpty())return tracked();
    }
    return true;
}

bool GitManagedFiles::applyAttributes(const QString &repository, const QStringList &relativeFolders, QString *error) {
    if (error) error->clear();
    if (!validFolders(relativeFolders, error)) return false;
    return appendRules(QDir(repository).filePath(".gitattributes"),patterns(relativeFolders),QStringLiteral("Repository attributes"),error);
}

bool GitManagedFiles::verifyAttributes(const QString &repository, const QStringList &relativeFolders, QString *error) {
    if (error) error->clear();
    if (!validFolders(relativeFolders, error)) return false;
    if (relativeFolders.isEmpty()) return true;
    QStringList arguments{"ls-files", "-co", "--exclude-standard", "-z", "--"};
    bool all = relativeFolders.contains("");
    if (!all) arguments += relativeFolders;
    QByteArray files;
    if (!git(repository, arguments, &files, error)) return false;
    QStringList paths;
    for (const auto &name : files.split('\0')) {
        if (name.isEmpty()) continue;
        const auto path = QString::fromUtf8(name);
        if (path.toUtf8() != name || !safeRelative(path)) return fail(error, QStringLiteral("A repository filename is unsupported."));
        const auto base = QFileInfo(path).fileName();
        if (path.endsWith(".pin") || path.endsWith(".pical") || path.endsWith(".pilon-event")
            || base == ".pilon-calendar-deletions.json" || base == ".folders.json" || base == ".library.json" || base == ".settings.json") paths << path;
    }
    for (const auto &folder : relativeFolders) {
        const auto prefix = folder.isEmpty() ? QString() : folder + '/';
        for (const auto &name : {"pilon-validation.pin", "pilon-validation.pical", "pilon-validation.pilon-event",
                                 ".pilon-calendar-deletions.json", ".folders.json", ".library.json", ".settings.json"}) paths << prefix + name;
    }
    paths.removeDuplicates(); QByteArray input;
    for (const auto &path : paths) input += path.toUtf8() + '\0';
    QByteArray attributes;
    if (!git(repository, {"check-attr", "-z", "--stdin", "merge"}, &attributes, error, input)) return false;
    const auto fields = attributes.split('\0');
    if (fields.size() != paths.size() * 3 + 1) return fail(error, QStringLiteral("Git returned invalid file attributes."));
    for (qsizetype i = 0; i < fields.size() - 1; i += 3) {
        if (fields[i+1] != "merge" || fields[i+2] != "unset")
            return fail(error, QStringLiteral("Review and add Pilon's managed-file rules in Repository settings before synchronizing."));
    }
    return true;
}

bool GitManagedFiles::materializeTree(const QString &repository, const QString &revision, const QStringList &relativeFolders,
                                     const QString &emptyDestination, QString *error,
                                     const std::shared_ptr<std::atomic_bool> &cancel) {
    if (error) error->clear();
    if (!validFolders(relativeFolders, error) || !QRegularExpression("^(?:[a-f0-9]{40}|[a-f0-9]{64})$").match(revision).hasMatch())
        return fail(error, QStringLiteral("A pinned Git revision and valid managed folders are required."));
    const QFileInfo destination(emptyDestination);
    if (!destination.isAbsolute() || !destination.isDir() || destination.isSymLink()
        || !QDir(emptyDestination).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty())
        return fail(error, QStringLiteral("Git validation requires an empty temporary folder."));
    const auto repositoryRoot = QFileInfo(repository).canonicalFilePath();
    const auto destinationRoot = destination.canonicalFilePath();
    if (repositoryRoot.isEmpty() || destinationRoot == repositoryRoot || destinationRoot.startsWith(repositoryRoot + '/'))
        return fail(error, QStringLiteral("The validation snapshot must be outside the repository."));
    if (relativeFolders.isEmpty()) return true;
    QElapsedTimer timer; timer.start();
    QStringList arguments{"ls-tree", "-rz", "--full-tree", revision, "--"};
    if (!relativeFolders.contains("")) arguments += relativeFolders;
    QByteArray tree;
    if (!git(repository, arguments, &tree, error, {}, cancel)) return false;
    for (const auto &entry : tree.split('\0')) {
        if (entry.isEmpty()) continue;
        if (cancel && cancel->load()) return fail(error, QStringLiteral("Git validation was cancelled."));
        const auto tab = entry.indexOf('\t');
        if (tab < 0) return fail(error, QStringLiteral("Git returned an invalid tree entry."));
        const auto header = entry.first(tab).split(' ');
        const auto rawPath = entry.sliced(tab + 1); const auto path = QString::fromUtf8(rawPath);
        if (header.size() != 3 || !safeRelative(path) || path.toUtf8() != rawPath)
            return fail(error, QStringLiteral("A managed snapshot contains an unsupported filename."));
        if ((header[0] != "100644" && header[0] != "100755") || header[1] != "blob")
            return fail(error, QStringLiteral("Managed folders cannot contain symbolic links or submodules."));
        const auto oid = QString::fromLatin1(header[2]);
        if (!QRegularExpression("^(?:[a-f0-9]{40}|[a-f0-9]{64})$").match(oid).hasMatch())
            return fail(error, QStringLiteral("Git returned an invalid object identity."));
        const auto target = QDir(emptyDestination).filePath(path);
        if (!QDir().mkpath(QFileInfo(target).absolutePath())) return fail(error, QStringLiteral("A validation folder could not be created."));
        QSaveFile file(target);
        if (!file.open(QIODevice::WriteOnly)) return fail(error, QStringLiteral("A validation snapshot could not be written."));
        if (!git(repository, {"cat-file", "blob", oid}, nullptr, error, {}, cancel, &file,
                 qMax(0, validationTimeout - static_cast<int>(timer.elapsed())))) return false;
        if (!file.commit())
            return fail(error, QStringLiteral("A validation snapshot could not be written."));
    }
    for (const auto &folder : relativeFolders)
        if (!QDir().mkpath(QDir(emptyDestination).filePath(folder))) return fail(error, QStringLiteral("A validation folder could not be created."));
    return true;
}
