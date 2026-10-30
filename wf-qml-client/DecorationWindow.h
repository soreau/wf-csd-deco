#ifndef DECORATIONWINDOW_H
#define DECORATIONWINDOW_H
#include <wayland-client.h>
#include <QFileInfo>
#include <QQuickWindow>
#include <QQuickItem>
#include <QQmlComponent>
#include <QIcon>
#include <QPoint>
#include <QSettings>
class DecorationWindow:public QQuickWindow
{
    Q_OBJECT
public slots:
    void set_theme_1();
    void set_theme_2();
public:
    DecorationWindow(uint32_t id,QWindow* parent=nullptr);
    ~DecorationWindow();
    void setWindowTitle(const QString& title);
    void setAppId(const QString& appId);
    std::string getWindowTitle();
    std::string getAppId();
    Qt::Edges getEdgesAt(const QPoint& pos);
    inline uint32_t getWfId()const{return wf_id;}
    void setup_items();
protected:
    void closeEvent(QCloseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void exposeEvent(QExposeEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
private:
    bool isGroupParent;
    int groupId;
    uint32_t wf_id;
    int defaultBorderSize=8;
    QQuickItem* m_item=nullptr;
};
#endif
