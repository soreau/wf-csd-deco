#include "protocol.hpp"
#include "decorator.hpp"

#include <QDir>
#include <QTimer>
#include <QApplication>
#include <QWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QScrollArea>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QToolTip>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWindow>
#include <QScreen>
#include <QMap>
#include <QSharedPointer>
#include <QDebug>
#include <QPainter>
#include <QPainterPath>
#include <QMenu>
#include <QCommandLineParser>
#include <QGraphicsDropShadowEffect>

#include <qpa/qplatformnativeinterface.h>

QString qt6DecoCfgPath;

// Global data
QMap<uint32_t, QWidget*> view_to_decor;

static QPainterPath getBorderPath(QRectF rect, qreal radius, qreal penSize, qreal shadowSize,
    uint32_t tiledEdges)
{
    QPainterPath path;

    path.setFillRule(Qt::WindingFill);

    qreal halfBorderSize = penSize / 2.0;
    qreal offset = halfBorderSize;

    // No rounded corners when tiled
    if (tiledEdges != 0)
    {
        path.addRect(rect.adjusted(offset, offset, -offset, -offset));
    } else
    {
        if (penSize >= 2.0)
        {
            QRectF borderRect = QRectF(
                shadowSize - halfBorderSize,
                shadowSize - halfBorderSize,
                rect.width() + penSize,
                rect.height() + penSize).adjusted(penSize, penSize, -penSize, -penSize);
            path.addRoundedRect(borderRect, radius, radius);
        } else
        {
            QRectF topRect = QRectF(
                shadowSize - halfBorderSize,
                shadowSize - halfBorderSize,
                rect.width() + penSize,
                rect.height() + penSize).adjusted(penSize, penSize, -penSize, -penSize);

            QRectF bottomRect = QRectF(
                shadowSize,
                shadowSize + radius,
                rect.width(),
                rect.height() - radius).adjusted(offset, offset, -offset, -offset);

            path.addRoundedRect(topRect, radius, radius);
            path.addRect(bottomRect);
        }
    }

    return path.simplified();
}

// ===== DecorationWindow Implementation =====
DecorationWindow::DecorationWindow(uint32_t id, QWidget *parent) :
    QWidget(parent), wf_id(id), isGroupParent(false), groupId(0)
{
    setWindowFlags(
        Qt::Window | Qt::CustomizeWindowHint | Qt::FramelessWindowHint);

    settings = new Settings(this);

    connect(
        settings.get(), &Settings::settingsChanged, this, [this] ()
    {
        iconLbl->setFixedSize(QSize(settings->uiSize, settings->uiSize));
        titleLbl->setStyleSheet(QString("QLabel { color: %1; }").arg(settings->textColor.name()));
        minBtn->setFixedSize(QSize(settings->uiSize, settings->uiSize));
        maxBtn->setFixedSize(QSize(settings->uiSize, settings->uiSize));
        closeBtn->setFixedSize(QSize(settings->uiSize, settings->uiSize));
        groupBtn->setFixedHeight(settings->uiSize);

        mainLyt->setContentsMargins(QMargins(settings->shadowSize, settings->shadowSize, settings->shadowSize,
            settings->shadowSize));
        baseLyt->setContentsMargins(QMargins(settings->borderSize, settings->borderSize, settings->borderSize,
            settings->borderSize));

        QPoint relative_position = clientArea->mapTo(base, QPoint(0, 0));
        update_borders(wf_id, relative_position.y() + settings->shadowSize, 0,
            settings->shadowSize + settings->borderSize, settings->shadowSize + settings->borderSize,
            settings->shadowSize + settings->borderSize);

        /** Show show only if it has finite borders */
        if (settings->shadowSize)
        {
            drawShadow();
        }
        /** Disable shadows when shadow radius is zero */
        else
        {
            setGraphicsEffect(nullptr);
        }

        repaint();
    });

    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    resize(300, 300);

    setupUI();

    view_to_decor[id] = this;
}

DecorationWindow::~DecorationWindow()
{
    close_request(wf_id);
}

void DecorationWindow::setupUI()
{
    baseLyt = new QVBoxLayout();
    baseLyt->setContentsMargins(QMargins(settings->borderSize, settings->borderSize, settings->borderSize,
        settings->borderSize));
    baseLyt->setSpacing(0);

    iconLbl = new TabDragSource(wf_id, this);
    iconLbl->setCursor(Qt::ArrowCursor);
    iconLbl->setFixedSize(QSize(settings->uiSize, settings->uiSize));
    iconLbl->setPixmap(QIcon::fromTheme("wayfire").pixmap(settings->uiSize));

    titleLbl = new QLabel();
    titleLbl->setCursor(Qt::ArrowCursor);
    titleLbl->setStyleSheet(QString("QLabel { color: %1; }").arg(settings->textColor.name()));
    titleLbl->setFont(settings->titleFont);

    minBtn = new DecorationButton(DecorationButton::Type::Minimize, this);
    minBtn->setFixedSize(QSize(settings->uiSize, settings->uiSize));
    minBtn->setMouseTracking(true);
    connect(minBtn, &DecorationButton::clicked, this, &QWidget::showMinimized);

    maxBtn = new DecorationButton(DecorationButton::Type::Maximize, this);
    maxBtn->setFixedSize(QSize(settings->uiSize, settings->uiSize));
    maxBtn->setMouseTracking(true);
    connect(maxBtn, &DecorationButton::clicked, [this] ()
    {
        showMaximized();
    });

    closeBtn = new DecorationButton(DecorationButton::Type::Close, this);
    closeBtn->setFixedSize(QSize(settings->uiSize, settings->uiSize));
    closeBtn->setMouseTracking(true);
    connect(closeBtn, &DecorationButton::clicked, this, &QWidget::close);

    groupBtn = new TabDropTarget(this);
    groupBtn->setFixedHeight(settings->uiSize);
    groupBtn->setCursor(Qt::ArrowCursor);
    groupBtn->setStyleSheet(QString("TabDropTarget { color: %1; background-color: %2; }").arg(settings->
        textColor.name(), settings->baseColor.name()));

    clientArea = new QWidget();
    clientArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    QHBoxLayout *titleLyt = new QHBoxLayout();
    titleLyt->setContentsMargins(QMargins(5, 5, 5, 5));
    titleLyt->setSpacing(5);

    titleLyt->addWidget(iconLbl);
    titleLyt->addWidget(groupBtn);
    titleLyt->addWidget(titleLbl);
    titleLyt->addStretch();
    titleLyt->addWidget(minBtn);
    titleLyt->addWidget(maxBtn);
    titleLyt->addWidget(closeBtn);

    baseLyt->addLayout(titleLyt);
    baseLyt->addWidget(clientArea);

    base = new QWidget();
    base->setLayout(baseLyt);
    base->setCursor(Qt::ArrowCursor);

    QGraphicsDropShadowEffect *shadow = new QGraphicsDropShadowEffect(base);
    shadow->setBlurRadius(settings->shadowSize);
    shadow->setColor(settings->shadowColor);
    shadow->setOffset(0);
    setGraphicsEffect(shadow);

    mainLyt = new QVBoxLayout();
    mainLyt->setContentsMargins(QMargins(settings->shadowSize, settings->shadowSize, settings->shadowSize,
        settings->shadowSize));

    mainLyt->addWidget(base);

    setLayout(mainLyt);
}

// ===== Group Management - Like GTK version =====

void DecorationWindow::addTabForWindow(uint32_t cdata_wf_id)
{
    // In Qt version, we add to the TabDropTarget menu instead of a tab box
    // This is called by refreshGroup to populate the menu
    if (this->groupBtn)
    {
        QWidget *decor = view_to_decor[cdata_wf_id];
        if (!decor)
        {
            return;
        }

        DecorationWindow *win = qobject_cast<DecorationWindow*>(decor);
        if (!win)
        {
            return;
        }

        this->groupBtn->addWindow(
            cdata_wf_id,
            win->appId,
            win->windowTitle());
    }
}

void DecorationWindow::clearGroupTabs(uint32_t group_id)
{
    if (!group_id)
    {
        return;
    }

    // Clear all TabDropTarget menus for windows in this group
    for (auto it = view_to_decor.begin(); it != view_to_decor.end(); ++it)
    {
        if (auto *dec = qobject_cast<DecorationWindow*>(it.value()))
        {
            if (dec->groupId == group_id)
            {
                if (dec->groupBtn)
                {
                    dec->groupBtn->clearAll();
                }
            }
        }
    }
}

void DecorationWindow::refreshGroup(uint32_t group_id)
{
    if (!group_id)
    {
        return;
    }

    // Get the order from the parent
    QList<uint32_t> button_order;
    bool hasParent = false;

    for (auto it = view_to_decor.begin(); it != view_to_decor.end(); ++it)
    {
        if (auto *dec = qobject_cast<DecorationWindow*>(it.value()))
        {
            if ((dec->groupId == group_id) && dec->isGroupParent)
            {
                button_order = dec->groupOrder;
                hasParent    = true;
                break;
            }
        }
    }

    // If there's no parent or the order is empty, the group is effectively empty
    if (!hasParent || button_order.isEmpty())
    {
        // Reset all windows that still think they're in this group
        for (auto it = view_to_decor.begin(); it != view_to_decor.end(); ++it)
        {
            if (auto *dec = qobject_cast<DecorationWindow*>(it.value()))
            {
                if (dec->groupId == group_id)
                {
                    dec->groupId = 0;
                    dec->isGroupParent = false;
                    dec->groupOrder.clear();

                    // Reset their menu to show only themselves
                    if (dec->groupBtn)
                    {
                        dec->groupBtn->clearAll();
                        dec->groupBtn->addWindow(
                            dec->wf_id,
                            dec->appId,
                            dec->windowTitle());
                    }
                }
            }
        }

        return;
    }

    // Clear all tabs in the group first
    clearGroupTabs(group_id);

    // For each window in the group, add all tabs
    for (auto it = view_to_decor.begin(); it != view_to_decor.end(); ++it)
    {
        if (auto *dec = qobject_cast<DecorationWindow*>(it.value()))
        {
            if (dec->groupId == group_id)
            {
                // Add each window in order to this window's menu
                for (auto id : button_order)
                {
                    dec->addTabForWindow(id);
                }
            }
        }
    }

    // If only one window in group, convert it to a standalone window
    if (button_order.size() == 1)
    {
        uint32_t solo_id = button_order.first();
        auto *dec = qobject_cast<DecorationWindow*>(view_to_decor[solo_id]);
        if (dec)
        {
            dec->groupId = 0;
            dec->isGroupParent = false;
            dec->groupOrder.clear();

            if (dec->groupBtn)
            {
                dec->groupBtn->clearAll();
                dec->groupBtn->addWindow(
                    solo_id,
                    dec->appId,
                    dec->windowTitle());
            }
        }

        // Remove the now-empty group from all other windows
        for (auto it = view_to_decor.begin(); it != view_to_decor.end(); ++it)
        {
            if (auto *other = qobject_cast<DecorationWindow*>(it.value()))
            {
                if ((other->groupId == group_id) && (other->wf_id != solo_id))
                {
                    other->groupId = 0;
                    other->isGroupParent = false;
                    other->groupOrder.clear();
                }
            }
        }
    }
}

void DecorationWindow::group(uint32_t drop_target_id, uint32_t wf_id)
{
    auto *drop_target_win = qobject_cast<DecorationWindow*>(view_to_decor[drop_target_id]);
    auto *drag_source_win = qobject_cast<DecorationWindow*>(view_to_decor[wf_id]);

    if (!drop_target_win || !drag_source_win)
    {
        return;
    }

    uint32_t group_id = 1;

    if (drag_source_win->groupId && (drag_source_win->groupId == drop_target_win->groupId))
    {
        qDebug() << "Cannot add tab to the same group.";
        return;
    }

    // Ungroup the dragged window first
    ungroup(wf_id, false);

    // Tell the compositor about the grouping
    group_windows(drop_target_id, wf_id);

    // Find or create group ID
    if (drop_target_win->groupId)
    {
        group_id = drop_target_win->groupId;
    } else
    {
        for (auto it = view_to_decor.begin(); it != view_to_decor.end(); ++it)
        {
            if (auto *dec = qobject_cast<DecorationWindow*>(it.value()))
            {
                if (dec->groupId >= group_id)
                {
                    group_id = dec->groupId + 1;
                }
            }
        }

        drop_target_win->isGroupParent = true;
        drop_target_win->groupId = group_id;
        drop_target_win->groupOrder.append(drop_target_win->wf_id);
    }

    // Set the dragged window's group
    drag_source_win->groupId = group_id;

    // Add to parent's order
    for (auto it = view_to_decor.begin(); it != view_to_decor.end(); ++it)
    {
        if (auto *dec = qobject_cast<DecorationWindow*>(it.value()))
        {
            if ((dec->groupId == group_id) && dec->isGroupParent)
            {
                if (!dec->groupOrder.contains(drag_source_win->wf_id))
                {
                    dec->groupOrder.append(drag_source_win->wf_id);
                }

                break;
            }
        }
    }

    // Refresh all windows in the group
    refreshGroup(group_id);
}

void DecorationWindow::ungroup(uint32_t wf_id, bool notify_server)
{
    auto *win = qobject_cast<DecorationWindow*>(view_to_decor[wf_id]);
    if (!win)
    {
        return;
    }

    auto group_id = win->groupId;

    if (group_id)
    {
        // Remove from parent's order
        for (auto it = view_to_decor.begin(); it != view_to_decor.end(); ++it)
        {
            if (auto *dec = qobject_cast<DecorationWindow*>(it.value()))
            {
                if ((group_id == dec->groupId) && dec->isGroupParent)
                {
                    dec->groupOrder.removeAll(wf_id);
                    break;
                }
            }
        }
    }

    // Reset this window's group data
    win->isGroupParent = false;
    win->groupOrder.clear();
    win->groupId = 0;

    // Reset this window's menu to show only itself
    if (win->groupBtn)
    {
        win->groupBtn->clearAll();
        win->groupBtn->addWindow(
            wf_id,
            win->appId,
            win->windowTitle());
    }

    // If there was a group, refresh it
    if (group_id)
    {
        refreshGroup(group_id);
    }

    if (notify_server)
    {
        ungroup_window(wf_id);
    }
}

// ===== Event handlers =====

bool DecorationWindow::isOverButtons()
{
    return minBtn->isUnderMouse || maxBtn->isUnderMouse || closeBtn->isUnderMouse;
}

void DecorationWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat("application/x-wf-window-id"))
    {
        event->acceptProposedAction();
    }
}

void DecorationWindow::dropEvent(QDropEvent *event)
{
    if (event->mimeData()->hasFormat("application/x-wf-window-id"))
    {
        bool ok;
        uint32_t id = event->mimeData()->data("application/x-wf-window-id").toUInt(&ok);
        if (ok && (id != wf_id))
        {
            group(wf_id, id);
            event->acceptProposedAction();
        }
    }
}

void DecorationWindow::closeEvent(QCloseEvent *event)
{
    close_request(wf_id);
    event->accept();
}

void DecorationWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);

    emit settings->settingsChanged();
}

Qt::Edges DecorationWindow::getEdgesAt(const QPoint & pos)
{
    int padding     = settings->shadowSize + 3;
    Qt::Edges edges = {};
    if (pos.x() <= settings->borderSize + padding)
    {
        edges |= Qt::LeftEdge;
    } else if (pos.x() >= width() - settings->borderSize - padding)
    {
        edges |= Qt::RightEdge;
    }

    if (pos.y() <= settings->borderSize + padding)
    {
        edges |= Qt::TopEdge;
    } else if (pos.y() >= height() - settings->borderSize - padding)
    {
        edges |= Qt::BottomEdge;
    }

    return edges;
}

void DecorationWindow::updateCursorShape(const QPoint & pos)
{
    Qt::Edges edges = getEdgesAt(pos);
    if (edges.testFlag(Qt::LeftEdge) && edges.testFlag(Qt::TopEdge))
    {
        setCursor(Qt::SizeFDiagCursor);
    } else if (edges.testFlag(Qt::RightEdge) && edges.testFlag(Qt::BottomEdge))
    {
        setCursor(Qt::SizeFDiagCursor);
    } else if (edges.testFlag(Qt::LeftEdge) && edges.testFlag(Qt::BottomEdge))
    {
        setCursor(Qt::SizeBDiagCursor);
    } else if (edges.testFlag(Qt::RightEdge) && edges.testFlag(Qt::TopEdge))
    {
        setCursor(Qt::SizeBDiagCursor);
    } else if (edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge))
    {
        setCursor(Qt::SizeHorCursor);
    } else if (edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge))
    {
        setCursor(Qt::SizeVerCursor);
    } else
    {
        setCursor(Qt::ArrowCursor);
    }
}

void DecorationWindow::drawShadow()
{
    QGraphicsDropShadowEffect *shadow = new QGraphicsDropShadowEffect(base);
    shadow->setBlurRadius(settings->shadowSize);
    shadow->setColor(settings->shadowColor);
    shadow->setOffset(0);

    setGraphicsEffect(shadow);
}

void DecorationWindow::mousePressEvent(QMouseEvent *event)
{
    if ((event->button() == Qt::LeftButton) && !isOverButtons() && !iconLbl->underMouse() &&
        !groupBtn->underMouse())
    {
        Qt::Edges edges = getEdgesAt(event->pos());
        if (edges)
        {
            windowHandle()->startSystemResize(edges);
        } else
        {
            windowHandle()->startSystemMove();
        }
    }

    QWidget::mousePressEvent(event);
}

void DecorationWindow::mouseMoveEvent(QMouseEvent *event)
{
    updateCursorShape(event->pos());
    QWidget::mouseMoveEvent(event);
}

void DecorationWindow::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setRenderHints(QPainter::Antialiasing);

    qreal radius = 5.0;

    if (isActive)
    {
        painter.setPen(QPen(settings->activeBorderColor, settings->borderSize));
    } else
    {
        painter.setPen(QPen(settings->inactiveBorderColor, settings->borderSize));
    }

    painter.setBrush(settings->baseColor);

    // painter.drawPath(getBorderPath(QRectF(0, 0, width(), height()), radius, settings->borderSize));
    painter.drawPath(getBorderPath(base->geometry(), radius, settings->borderSize, settings->shadowSize,
        tiledEdges));

    painter.end();
}

void DecorationWindow::setWindowTitle(const QString & title)
{
    QWidget::setWindowTitle(title);
    titleLbl->setText(title);
}

void DecorationWindow::setAppId(const QString & appId)
{
    this->appId = appId;

    QPixmap pixmap;
    if (QIcon::hasThemeIcon(appId))
    {
        pixmap = QIcon::fromTheme(appId).pixmap(settings->uiSize);
    } else
    {
        pixmap = QIcon::fromTheme("wayfire").pixmap(settings->uiSize);
    }

    if (iconLbl)
    {
        iconLbl->setPixmap(pixmap);
    }

    // Add initial tab
    if (groupBtn)
    {
        groupBtn->addWindow(wf_id, appId, windowTitle());
    }
}

void DecorationWindow::markAsActive(bool active)
{
    isActive = active;
    repaint();
}

void DecorationWindow::notifyTiledEdges(uint32_t edges)
{
    if ((!tiledEdges && !edges) || (tiledEdges && edges))
    {
        return;
    }

    qDebug() << "Tiling state changed:" << edges;

    if (edges)
    {
        savedShadowSize = settings->shadowSize;
        settings->shadowSize = 0;
    } else
    {
        settings->shadowSize = savedShadowSize;
        savedShadowSize = 0;
    }

    emit settings->settingsChanged();

    tiledEdges = edges;
}

// ===== DecorationButton Implementation =====
DecorationButton::DecorationButton(Type btnType, QWidget *parent) :
    QWidget(parent), buttonType(btnType), mOpacity(0.25), isUnderMouse(false)
{
    setFixedSize(16, 16);
    setMouseTracking(true);

    opacityAnimation = new QPropertyAnimation(this, "opacity", this);
    opacityAnimation->setDuration(200);
    opacityAnimation->setEasingCurve(QEasingCurve::OutCubic);
}

void DecorationButton::setOpacity(qreal opacity)
{
    mOpacity = opacity;
    update();
}

void DecorationButton::enterEvent(QEnterEvent *event)
{
    isUnderMouse = true;
    animateOpacity(0.75);
    qobject_cast<QWidget*>(parent())->repaint();
    QWidget::enterEvent(event);
}

void DecorationButton::leaveEvent(QEvent *event)
{
    isUnderMouse = false;
    animateOpacity(0.25);
    qobject_cast<QWidget*>(parent())->repaint();
    QWidget::leaveEvent(event);
}

void DecorationButton::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        isPressed = true;
        animateOpacity(1.0);
        update();
    }

    QWidget::mousePressEvent(event);
}

void DecorationButton::mouseReleaseEvent(QMouseEvent *event)
{
    if ((event->button() == Qt::LeftButton) && isPressed)
    {
        isPressed = false;
        animateOpacity(isUnderMouse ? 0.75 : 0.25);
        update();
        emit clicked();
    }

    QWidget::mouseReleaseEvent(event);
}

void DecorationButton::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QColor color;
    switch (buttonType)
    {
      case Type::Minimize:
        color = Qt::darkYellow;
        break;

      case Type::Maximize:
        color = Qt::darkCyan;
        break;

      case Type::Pin:
        color = Qt::darkGreen;
        break;

      case Type::Close:
        color = Qt::darkRed;
        break;
    }

    painter.setPen(QPen(color, 2.0, Qt::SolidLine));
    color.setAlphaF(mOpacity);
    QRect circleRect((width() - 16) / 2, (height() - 16) / 2, 16, 16);
    painter.setBrush(color);
    painter.drawEllipse(circleRect.adjusted(1.0, 1.0, -1.0, -1.0));
    painter.end();
}

void DecorationButton::animateOpacity(qreal targetOpacity)
{
    if (opacityAnimation->state() == QAbstractAnimation::Running)
    {
        opacityAnimation->stop();
    }

    opacityAnimation->setEndValue(targetOpacity);
    opacityAnimation->start();
}

// ===== TabDragSource Implementation =====
TabDragSource::TabDragSource(uint32_t id, QWidget *parent) :
    QLabel(parent), wfId(id)
{
    setFixedSize(24, 24);
    setScaledContents(true);
    setStyleSheet("QLabel { border: none; background: transparent; }");
}

void TabDragSource::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        dragStartPos = event->pos();
    }

    QLabel::mousePressEvent(event);
}

void TabDragSource::mouseMoveEvent(QMouseEvent *event)
{
    if (!(event->buttons() & Qt::LeftButton))
    {
        return;
    }

    if ((event->pos() - dragStartPos).manhattanLength() < QApplication::startDragDistance())
    {
        return;
    }

    QDrag *drag = new QDrag(this);
    QMimeData *mimeData = new QMimeData;
    mimeData->setData("application/x-wf-window-id", QByteArray::number(wfId));
    drag->setMimeData(mimeData);
    drag->setPixmap(pixmap(Qt::ReturnByValue).scaled(32, 32));
    drag->setHotSpot(QPoint(16, 16));
    drag->exec(Qt::CopyAction | Qt::MoveAction);
}

void TabDragSource::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        select_window(wfId);
    }

    QLabel::mouseReleaseEvent(event);
}

// ===== TabDropTarget Implementation =====
TabDropTarget::TabDropTarget(QWidget *parent) :
    QPushButton(parent)
{
    setAcceptDrops(true);
    setFixedHeight(24);
    setText("+");
    setToolTip("Drop here to group windows");
    setStyleSheet(
        "QPushButton {"
        "  border: 1px solid #888;"
        "  border-radius: 3px;"
        "  padding: 2px 8px;"
        "  background: transparent;"
        "}"
        "QPushButton:hover {"
        "  background: #d8d8d8;"
        "}");
    setMenu(new QMenu(this));
}

void TabDropTarget::addWindow(uint32_t wf_id, const QString & appId, const QString & title)
{
    QWidgetAction *action = new QWidgetAction(menu());
    GroupEntry *entry     = new GroupEntry(wf_id, appId, title, menu());
    action->setDefaultWidget(entry);
    action->setData(wf_id);
    action->setText(title);

    // Click on the menu item → select the window
    connect(action, &QAction::triggered, this, [wf_id] ()
    {
        select_window(wf_id);
    });

    // Ungroup button clicked → perform full ungroup
    connect(entry, &GroupEntry::ungroup, this, [this, wf_id] ()
    {
        DecorationWindow *parentWin = qobject_cast<DecorationWindow*>(parent()->parent());
        if (!parentWin)
        {
            return;
        }

        parentWin->ungroup(wf_id, true);
    });

    menu()->addAction(action);
    updateButtonState();
}

void TabDropTarget::removeWindow(uint32_t wf_id)
{
    for (QAction *action : menu()->actions())
    {
        if (action->data().toUInt() == wf_id)
        {
            menu()->removeAction(action);
            delete action;
            break;
        }
    }

    updateButtonState();
}

void TabDropTarget::clearAll()
{
    menu()->clear();
    updateButtonState();
}

void TabDropTarget::updateButtonState()
{
    if (menu()->actions().isEmpty())
    {
        setIcon(QIcon());
        setText("+");
        setToolTip("Drop here to group windows");
    } else
    {
        DecorationWindow *parentWin = qobject_cast<DecorationWindow*>(parent()->parent());
        uint32_t activeWfId = parentWin ? parentWin->getWfId() : 0;

        QAction *displayAction = nullptr;

        // Try to find the action for the parent window (current client)
        if (activeWfId != 0)
        {
            for (QAction *action : menu()->actions())
            {
                if (action->data().toUInt() == activeWfId)
                {
                    displayAction = action;
                    break;
                }
            }
        }

        // Fallback to first action if parent window not found in menu
        if (!displayAction)
        {
            displayAction = menu()->actions().first();
        }

        setIcon(displayAction->icon());
        setText(displayAction->text());
        setToolTip(QString("Group of %1 windows - Current: %2")
            .arg(menu()->actions().size())
            .arg(displayAction->text()));
    }
}

void TabDropTarget::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat("application/x-wf-window-id"))
    {
        event->acceptProposedAction();
        setStyleSheet(
            "QPushButton {"
            "  border: 2px solid #4CAF50;"
            "  border-radius: 3px;"
            "  padding: 2px 8px;"
            "  background: #c8e6c9;"
            "}");
    }
}

void TabDropTarget::dragLeaveEvent(QDragLeaveEvent *event)
{
    setStyleSheet(
        "QPushButton {"
        "  border: 1px solid #888;"
        "  border-radius: 3px;"
        "  padding: 2px 8px;"
        "  background: transparent;"
        "}");
    QPushButton::dragLeaveEvent(event);
}

void TabDropTarget::dropEvent(QDropEvent *event)
{
    setStyleSheet(
        "QPushButton {"
        "  border: 1px solid #888;"
        "  border-radius: 3px;"
        "  padding: 2px 8px;"
        "  background: transparent;"
        "}");

    if (event->mimeData()->hasFormat("application/x-wf-window-id"))
    {
        bool ok;
        uint32_t dropped_wf_id = event->mimeData()->data("application/x-wf-window-id").toUInt(&ok);
        if (ok)
        {
            DecorationWindow *targetWindow = qobject_cast<DecorationWindow*>(parent()->parent());
            if (targetWindow)
            {
                targetWindow->group(targetWindow->getWfId(), dropped_wf_id);
                event->acceptProposedAction();
            }
        }
    }
}

// ===== GroupEntry =====

GroupEntry::GroupEntry(uint32_t wf_id, const QString & appId, const QString & title,
    QWidget *parent) : QWidget(parent)
{
    QHBoxLayout *lyt = new QHBoxLayout();

    iconLbl = new TabDragSource(wf_id, this);
    iconLbl->setFixedSize(QSize(24, 24));

    QPixmap pixmap;
    if (QIcon::hasThemeIcon(appId))
    {
        pixmap = QIcon::fromTheme(appId).pixmap(24);
    } else
    {
        pixmap = QIcon::fromTheme("wayfire").pixmap(24);
    }

    if (iconLbl)
    {
        iconLbl->setPixmap(pixmap);
    }

    titleLbl = new QLabel(title, this);
    titleLbl->setFixedHeight(24);

    ungroupBtn = new QToolButton(this);
    ungroupBtn->setFixedSize(QSize(24, 24));
    ungroupBtn->setIcon(QIcon::fromTheme("arrow-up-double"));
    ungroupBtn->setToolTip("Ungroup");
    ungroupBtn->setAutoRaise(true);
    connect(ungroupBtn, &QToolButton::clicked, this, &GroupEntry::ungroup);

    lyt->addWidget(iconLbl);
    lyt->addWidget(titleLbl);
    lyt->addWidget(ungroupBtn);

    setLayout(lyt);
}

// ===== Settings =====

Settings::Settings(QObject *parent) : QObject(parent)
{
    /** -c was not used. Let's see env var */
    if (qt6DecoCfgPath.isEmpty())
    {
        QString qt6DecoCfgPath = qgetenv("WF_QT6_DECO_CONFIG_PATH");
    }

    /** env var was also not set, let's use the default path */
    if (qt6DecoCfgPath.isEmpty())
    {
        qt6DecoCfgPath = QDir::home().filePath(".config/wayfire/csd-decorator/qt6deco.ini");
    }

    sett = new QSettings(qt6DecoCfgPath, QSettings::IniFormat);
    loadSettings();

    fsw = new QFileSystemWatcher(this);
    fsw->addPath(qt6DecoCfgPath);

    connect(fsw, &QFileSystemWatcher::fileChanged, [this, qt6DecoCfgPath] (QString path)
    {
        if (!fsw->files().contains(qt6DecoCfgPath))
        {
            QTimer::singleShot(250, [this, qt6DecoCfgPath] ()
            {
                loadSettings();
                fsw->addPath(qt6DecoCfgPath);
                emit settingsChanged();
            });
        } else
        {
            loadSettings();
            emit settingsChanged();
        }
    });
}

void Settings::loadSettings()
{
    sett->sync();
    if (sett->contains("borderSize"))
    {
        QVariant borderSizeVar = sett->value("borderSize", activeBorderColor);
        if (borderSizeVar.isValid() && borderSizeVar.canConvert<int>())
        {
            borderSize = borderSizeVar.toInt();
        }
    }

    if (sett->contains("uiSize"))
    {
        QVariant uiSizeVar = sett->value("uiSize", activeBorderColor);
        if (uiSizeVar.isValid() && uiSizeVar.canConvert<int>())
        {
            uiSize = uiSizeVar.toInt();
        }
    }

    if (sett->contains("baseColor"))
    {
        QVariant baseColorVar = sett->value("baseColor", activeBorderColor);
        if (baseColorVar.isValid() && baseColorVar.canConvert<QColor>())
        {
            baseColor = baseColorVar.value<QColor>();
        }
    }

    if (sett->contains("textColor"))
    {
        QVariant textColorVar = sett->value("textColor", activeBorderColor);
        if (textColorVar.isValid() && textColorVar.canConvert<QColor>())
        {
            textColor = textColorVar.value<QColor>();
        }
    }

    if (sett->contains("titleFont"))
    {
        QVariant titleFontVar = sett->value("titleFont", activeBorderColor);
        if (titleFontVar.isValid() && titleFontVar.canConvert<QFont>())
        {
            titleFont = titleFontVar.value<QFont>();
        }
    }

    if (sett->contains("activeBorderColor"))
    {
        QVariant activeBorderColorVar = sett->value("activeBorderColor", activeBorderColor);
        if (activeBorderColorVar.isValid() && activeBorderColorVar.canConvert<QColor>())
        {
            activeBorderColor = activeBorderColorVar.value<QColor>();
        }
    }

    if (sett->contains("inactiveBorderColor"))
    {
        QVariant inactiveBorderColorVar = sett->value("inactiveBorderColor", activeBorderColor);
        if (inactiveBorderColorVar.isValid() && inactiveBorderColorVar.canConvert<QColor>())
        {
            inactiveBorderColor = inactiveBorderColorVar.value<QColor>();
        }
    }

    if (sett->contains("shadowSize"))
    {
        QVariant shadowSizeVar = sett->value("shadowSize", activeBorderColor);
        if (shadowSizeVar.isValid() && shadowSizeVar.canConvert<int>())
        {
            shadowSize = shadowSizeVar.toInt();
        }
    }

    if (sett->contains("shadowColor"))
    {
        QVariant shadowColorVar = sett->value("shadowColor", activeBorderColor);
        if (shadowColorVar.isValid() && shadowColorVar.canConvert<QColor>())
        {
            shadowColor = shadowColorVar.value<QColor>();
        }
    }
}

// ===== Main =====

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setDesktopFileName("org.wf.sample-decorator");

    QCommandLineParser parser;
    parser.addOption({{"c", "config"}, "Configuration file path", "config"});

    parser.process(app);

    if (parser.isSet("config"))
    {
        qt6DecoCfgPath = parser.value("config");
    }

    QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();

    if (native)
    {
        struct wl_display *display =
            reinterpret_cast<wl_display*>(native->nativeResourceForIntegration("display"));

        setup_protocol(display);
        return app.exec();
    } else
    {
        qFatal() << "Unable to get wayland display!";
    }
}
