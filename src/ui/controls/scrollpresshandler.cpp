#include "scrollpresshandler.h"

#include <QPointerEvent>

void ScrollPressHandler::setView(QQuickItem *view)
{
    if (m_view == view) return;
    if (m_view)
        disconnect(m_view, &QQuickItem::windowChanged, this, &ScrollPressHandler::setWindow);
    m_view = view;
    if (view)
        connect(view, &QQuickItem::windowChanged, this, &ScrollPressHandler::setWindow);
    setWindow(view ? view->window() : nullptr);
    emit viewChanged();
}

void ScrollPressHandler::setWindow(QQuickWindow *window)
{
    if (m_window == window) return;
    if (m_window) m_window->removeEventFilter(this);
    m_window = window;
    if (window) window->installEventFilter(this);
}

bool ScrollPressHandler::eventFilter(QObject *, QEvent *event)
{
    // Flickable's child filter consumes a press before descendant pointer handlers can react.
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::TouchBegin:
    case QEvent::TabletPress:
        break;
    default:
        return false;
    }
    if (!m_view || !m_view->isVisible() || !m_view->isEnabled()) return false;
    const auto *pointer = static_cast<QPointerEvent *>(event);
    if (!pointer->pointCount()) return false;
    const auto position = pointer->points().constFirst().scenePosition();
    for (auto *item = m_view.data(); item; item = item->parentItem()) {
        if ((item == m_view || item->clip()) && !item->contains(item->mapFromScene(position)))
            return false;
    }
    QMetaObject::invokeMethod(m_view, "cancelFlick");
    return false;
}
