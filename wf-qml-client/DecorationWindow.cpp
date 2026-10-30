#include "Application.h"
#include "DecorationWindow.h"
#include "protocol.hpp"
#include <iostream>
/** Constructor */
DecorationWindow::DecorationWindow(uint32_t id, QWindow *parent) : QQuickWindow(parent), wf_id(id),
    isGroupParent(false), groupId(0)
{
    setColor(Qt::transparent);
    setFlags(Qt::FramelessWindowHint | Qt::BypassWindowManagerHint);
    setup_items();
}

void DecorationWindow::setup_items()
{
    QVariantMap props;
    props.insert("titleBarHeight", 40);
    props.insert("borderWidth", 8);
    if (m_item)
    {
        m_item->deleteLater();
    }

    m_item = qobject_cast<QQuickItem*>(app->s_component->createWithInitialProperties(props));
    connect(m_item, SIGNAL(minimize()), this, SLOT(showMinimized()));
    connect(m_item, SIGNAL(maximize()), this, SLOT(showMaximized()));
    connect(m_item, SIGNAL(close()), this, SLOT(close()));
    connect(m_item, SIGNAL(move()), this, SLOT(startSystemMove()));
    connect(m_item, SIGNAL(border_1()), this, SLOT(set_theme_1()));
    connect(m_item, SIGNAL(border_2()), this, SLOT(set_theme_2()));
    m_item->setParentItem(contentItem());
    m_item->setSize(size());
}

/** Set border theme 1 */
void DecorationWindow::set_theme_1()
{
    update_theme("border1");
}

/** Set border theme 2 */
void DecorationWindow::set_theme_2()
{
    update_theme("border2");
}

/** Set app ID */
void DecorationWindow::setAppId(const QString& appId)
{
    if (QIcon::hasThemeIcon(appId))
    {
        QIcon icon = QIcon::fromTheme(appId);
        // send icon to qml
    } else
    {
        QFileInfo desktop("/usr/share/applications/" + appId + ".desktop");
        if (desktop.exists())
        {
            QSettings settings(desktop.absoluteFilePath(), QSettings::NativeFormat);
            if (settings.childGroups().contains("Desktop Entry"))
            {
                settings.beginGroup("Desktop Entry");
                if (settings.childKeys().contains("Icon"))
                {
                    m_item->setProperty("windowIcon", settings.value("Icon"));
                }
            }
        }
    }

    m_item->setProperty("app_id", appId);
}

/** Set window title */
void DecorationWindow::setWindowTitle(const QString& title)
{
    setTitle(title);
    m_item->setProperty("title", title);
}

/** Get app ID */
std::string DecorationWindow::getAppId()
{
    return m_item->property("app_id").toString().toStdString();
}

/** Get window title */
std::string DecorationWindow::getWindowTitle()
{
    return m_item->property("title").toString().toStdString();
}

/** Close event */
void DecorationWindow::closeEvent(QCloseEvent *event)
{
    close_request(wf_id);
    event->accept();
}

/** Mouse press event */
void DecorationWindow::mousePressEvent(QMouseEvent *event)
{
    if ((event->button() == Qt::LeftButton))
    {
        Qt::Edges edges = getEdgesAt(event->pos());
        if (edges)
        {
            startSystemResize(edges);
        }
    }

    QWindow::mousePressEvent(event);
}

/** Resize event */
void DecorationWindow::resizeEvent(QResizeEvent *event)
{
    m_item->setSize(size());
    QWindow::resizeEvent(event);
}

/** Expose event */
void DecorationWindow::exposeEvent(QExposeEvent *event)
{
    QQuickWindow::exposeEvent(event);
    update_borders(wf_id, 40, 8, 8, 8, 8);
}

/** Get edges at */
Qt::Edges DecorationWindow::getEdgesAt(const QPoint & pos)
{
    int padding     = 3;
    Qt::Edges edges = {};
    if (pos.x() <= defaultBorderSize + padding)
    {
        edges |= Qt::LeftEdge;
    } else if (pos.x() >= width() - defaultBorderSize - padding)
    {
        edges |= Qt::RightEdge;
    }

    if (pos.y() <= defaultBorderSize + padding)
    {
        edges |= Qt::TopEdge;
    } else if (pos.y() >= height() - defaultBorderSize - padding)
    {
        edges |= Qt::BottomEdge;
    }

    return edges;
}

/** Destructor */
DecorationWindow::~DecorationWindow()
{
    close_request(wf_id);
}
