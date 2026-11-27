#pragma once
#include <QVariantMap>
#include <QVariantList>
#include <atomic>
#include <functional>
#include <memory>

QVariantMap assistantAudioSummary(const QString &wavPath);
QVariantMap transcribeAssistantAudio(const QString &wavPath, const QString &whisperExecutable,
                                    const QString &whisperModel, const std::shared_ptr<std::atomic_bool> &cancel);
void saveAssistantAudioTranscript(const QString &directory, const QString &attachmentId, const QString &text,
                                  const std::shared_ptr<std::atomic_bool> &cancel);
bool loadAssistantAttachmentTranscripts(QVariantList &attachments, const QString &directory);
void loadAssistantAudioTranscripts(QVariantList &messages, const QString &directory);
bool applyAssistantAttachmentTranscripts(QVariantList &attachments, const QVariantMap &transcripts);
bool applyAssistantAudioTranscripts(QVariantList &messages, const QVariantMap &transcripts,
                                    const QString &messageId = {});

QVariantMap prepareAssistantMedia(const QVariantList &attachments, const QString &text,
                                 bool nativeAudio, const QString &directory,
                                 const QString &whisperExecutable, const QString &whisperModel,
                                 const std::shared_ptr<std::atomic_bool> &cancel, bool collectTranscriptions = false,
                                 const std::function<void(const QString &, const QString &)> &transcriptReady = {});
