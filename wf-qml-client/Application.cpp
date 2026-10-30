#include "Application.h"
Application::Application(int & argc, char **argv) : QGuiApplication(argc, argv)
{
    setDesktopFileName("org.wf.sample-decorator");

    m_engine    = new QQmlEngine(this);
    s_component = new QQmlComponent(m_engine, QUrl::fromLocalFile(QML_PATH_1), this);

    if (!s_component->isError())
    {
        qDebug() << "Loaded " << QML_PATH_1;
    } else
    {
        qDebug() << "QML ERROR";
    }
}

void Application::set_theme(std::string theme)
{
    if (theme == "border1")
    {
        delete s_component;
        s_component = new QQmlComponent(m_engine, QUrl::fromLocalFile(QML_PATH_1), this);
    } else if (theme == "border2")
    {
        delete s_component;
        s_component = new QQmlComponent(m_engine, QUrl::fromLocalFile(QML_PATH_2), this);
    } else
    {
        delete s_component;
        s_component = new QQmlComponent(m_engine, QUrl::fromLocalFile(QML_PATH_1), this);
    }

    if (!s_component->isError())
    {
        qDebug() << "Loaded " << theme;
    } else
    {
        qDebug() << "QML ERROR";
    }
}
