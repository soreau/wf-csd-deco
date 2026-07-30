#pragma once

#include "protocol.hpp"
#include <QApplication>
#include <QWidget>
#include <QPointer>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QToolButton>
#include <QLabel>
#include <QScrollArea>
#include <QDrag>
#include <QMenu>
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
#include <QDebug>
#include <QAbstractAnimation>
#include <QPropertyAnimation>
#include <QWidgetAction>
#include <QSettings>
#include <QFileSystemWatcher>

class DecorationButton;
class TabDragSource;
class TabDropTarget;
class Settings;

class DecorationWindow : public QWidget
{
    Q_OBJECT

  public:
    DecorationWindow(uint32_t wf_id, QWidget *parent = nullptr);
    ~DecorationWindow();

    void setWindowTitle(const QString & title);
    void setAppId(const QString & appId);
    void markAsActive(bool active);
    void notifyTiledEdges(uint32_t edges);
    uint32_t getWfId() const
    {
        return wf_id;
    }

    // Group management - like GTK version
    void addTabForWindow(uint32_t cdata_wf_id);
    void refreshGroup(uint32_t group_id);
    void clearGroupTabs(uint32_t group_id);
    void group(uint32_t drop_target_id, uint32_t wf_id);
    void ungroup(uint32_t wf_id, bool notify_server);

  protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *pEvent) override;

  private:
    void setupUI();
    bool isOverButtons();

    uint32_t wf_id;
    QString appId;
    bool isGroupParent;
    uint32_t groupId;
    QList<uint32_t> groupOrder;

    QPointer<QVBoxLayout> mainLyt;
    QPointer<QVBoxLayout> baseLyt;
    QPointer<QWidget> base;
    QPointer<QWidget> clientArea;

    QPointer<TabDragSource> iconLbl;
    QPointer<QLabel> titleLbl;
    QPointer<TabDropTarget> groupBtn;

    QPointer<DecorationButton> minBtn;
    QPointer<DecorationButton> maxBtn;
    QPointer<DecorationButton> closeBtn;

    Qt::Edges getEdgesAt(const QPoint & pos);
    void updateCursorShape(const QPoint & pos);
    void drawShadow();

    QPointer<Settings> settings;

    bool isActive = false;
    int savedShadowSize = 0;
    uint32_t tiledEdges = 0;
};

class DecorationButton : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal opacity READ opacity WRITE setOpacity)

  public:
    enum class Type
    {
        Minimize,
        Maximize,
        Pin,
        Close,
    };

    DecorationButton(Type btnType, QWidget *parent = nullptr);
    qreal opacity() const
    {
        return mOpacity;
    }

    void setOpacity(qreal opacity);
    bool isUnderMouse = false;

  signals:
    void clicked();

  protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

  private:
    void animateOpacity(qreal targetOpacity);
    Type buttonType;
    qreal mOpacity;
    bool isPressed = false;
    QPropertyAnimation *opacityAnimation;
};

class TabDragSource : public QLabel
{
    Q_OBJECT

  public:
    TabDragSource(uint32_t id, QWidget *parent = nullptr);

  protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

  private:
    uint32_t wfId = 0;
    QPoint dragStartPos;
};

class TabDropTarget : public QPushButton
{
    Q_OBJECT

  public:
    TabDropTarget(QWidget *parent = nullptr);

    // Like add_tab_button in GTK version
    void addWindow(uint32_t wf_id, const QString & appId, const QString & title);
    void removeWindow(uint32_t wf_id);
    void clearAll();

  protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

  private:
    void updateButtonState();
};

class GroupEntry : public QWidget
{
    Q_OBJECT

  public:
    GroupEntry(uint32_t wf_id, const QString & appId, const QString & title, QWidget *parent);

    void setTitle(QString& title);
    void setAppId(QString& appId);

    Q_SIGNAL void ungroup();

  private:
    QPointer<TabDragSource> iconLbl;
    QPointer<QLabel> titleLbl;
    QPointer<QToolButton> ungroupBtn;

  protected:
    // void mousePressEvent(QMouseEvent *event) override;
    // void mouseReleaseEvent(QMouseEvent *event) override;
};

class Settings : public QObject
{
    Q_OBJECT

  public:
    Settings(QObject *parent);

    /* Default border size, can be zero */
    int borderSize = 2;

    /* Default border size, can be zero */
    int uiSize = 24;

    /* Base titlebar color, can be transparent */
    QColor baseColor = QColor(29, 29, 29);

    /* Titlebar text color, contrasts with baseColor */
    QColor textColor = QColor(Qt::white);

    /* Titlebar font */
    QFont titleFont = QFont("sans", 10);

    /* Active border color, can be transparent */
    QColor activeBorderColor = QColor("#008080");

    /* Inactive border color, can be transparent */
    QColor inactiveBorderColor = QColor(29, 29, 29);

    /* Shadow size */
    int shadowSize = 10;

    /* Shadow color */
    QColor shadowColor = QColor(0, 0, 0);

    Q_SIGNAL void settingsChanged();

  private:
    QPointer<QSettings> sett;
    QPointer<QFileSystemWatcher> fsw;

    void loadSettings();
};
