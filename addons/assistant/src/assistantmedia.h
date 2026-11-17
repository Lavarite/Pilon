#pragma once
#include <QVariantMap>
#include <QVariantList>
#include <atomic>
#include <memory>

QVariantMap assistantAudioSummary(const QString &wavPath);

QVariantMap prepareAssistantMedia(const QVariantList &attachments, const QString &text,
                                 bool nativeAudio, const QString &directory,
                                 const QString &whisperExecutable, const QString &whisperModel,
                                 const std::shared_ptr<std::atomic_bool> &cancel);
