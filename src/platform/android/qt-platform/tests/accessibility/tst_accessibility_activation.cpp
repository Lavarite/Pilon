#include <QAccessible>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QThread>
#include <QtTest>
#include <atomic>
#include <memory>
#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif

class AccessibilityActivationTests : public QObject {
    Q_OBJECT
private slots:
    void activationWithLiveTextEditRunsOnQtThread() {
#ifndef Q_OS_ANDROID
        QSKIP("Exercises Android's real JNI accessibility callback.");
#else
        // qExec pumps events without entering QGuiApplication::exec(), which normally sets this root.
        QAccessible::setRootObject(qApp);
        QQuickWindow window;
        window.resize(360, 240);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        struct Invocation {
            std::atomic<bool> androidThread{false}, jniOk{true};
        };
        const auto invocation = std::make_shared<Invocation>();
        const auto activateFromAndroid = [invocation](bool active) {
            return QNativeInterface::QAndroidApplication::runOnAndroidMainThread([invocation, active] {
                invocation->androidThread.store(QThread::currentThread() != qApp->thread());
                QJniObject::callStaticMethod<void>("org/qtproject/qt/android/QtNativeAccessibility", "setActive", "(Z)V", jboolean(active));
                QJniEnvironment env;
                if (env.checkAndClearExceptions()) invocation->jniOk.store(false);
            });
        };
        auto disabling = activateFromAndroid(false);
        QVERIFY(QTest::qWaitFor([&] { return disabling.isFinished(); }, 5000));
        QTRY_VERIFY_WITH_TIMEOUT(!QAccessible::isActive(), 5000);
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl("qrc:/qt/qml/PilonAccessibilityProbe/Editor.qml"));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *editor = qobject_cast<QQuickItem *>(object.data());
        QVERIFY(editor);
        editor->setParentItem(window.contentItem());

        const auto attached = [editor] {
            for (auto *child : editor->children())
                if (QByteArray(child->metaObject()->className()) == "QQuickAccessibleAttached") return child;
            return static_cast<QObject *>(nullptr);
        };
        QVERIFY2(!attached(), "The editor must not have an existing Accessible child before activation.");
        struct Observer final : QAccessible::ActivationObserver {
            std::atomic<int> activated{0};
            std::atomic<bool> correctThread{true};
            void accessibilityActiveChanged(bool active) override {
                if (!active) return;
                if (QThread::currentThread() != qApp->thread()) correctThread.store(false);
                activated.fetch_add(1);
            }
        } observer;
        QAccessible::installActivationObserver(&observer);
        const auto removeObserver = qScopeGuard([&] { QAccessible::removeActivationObserver(&observer); });
        auto future = activateFromAndroid(true);
        QVERIFY(QTest::qWaitFor([&] { return future.isFinished(); }, 5000));
        QVERIFY(invocation->androidThread.load());
        QVERIFY(invocation->jniOk.load());
        QTRY_VERIFY_WITH_TIMEOUT(observer.activated.load() > 0, 5000);
        QVERIFY2(observer.correctThread.load(), "Activation observer ran outside Qt's application thread.");
        QTRY_VERIFY_WITH_TIMEOUT(attached(), 5000);
        QCOMPARE(attached()->parent(), editor);
        QCOMPARE(attached()->thread(), editor->thread());
        QCOMPARE(editor->thread(), qApp->thread());
        QVERIFY(editor->setProperty("text", "Edited after activation"));
        QCOMPARE(editor->property("text").toString(), QString("Edited after activation"));
        QAccessible::setActive(false);
#endif
    }
};

QTEST_MAIN(AccessibilityActivationTests)
#include "tst_accessibility_activation.moc"
