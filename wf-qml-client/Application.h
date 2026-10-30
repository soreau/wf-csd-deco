#ifndef APPLICATION_H
#define APPLICATION_H
#include <QGuiApplication>
#include <QQmlEngine>
#include "DecorationWindow.h"
class Application:public QGuiApplication
{
    Q_OBJECT
public:
    explicit Application(int &argc,char **argv);
    void set_theme(std::string theme);
    QQmlComponent* s_component=nullptr;
private:
    QQmlEngine* m_engine=nullptr;
};
extern Application *app;
#endif
