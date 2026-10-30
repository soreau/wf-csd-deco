#include "protocol.hpp"
#include <QMap>
#include <QSharedPointer>
#include <qpa/qplatformnativeinterface.h>
#include "Application.h"

Application *app;
QMap<uint32_t, QWindow*> view_to_decor;
int main(int argc, char *argv[])
{
    Application a(argc, argv);
    app = &a;
    QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
    if (native)
    {
        struct wl_display *display =
            reinterpret_cast<wl_display*>(native->nativeResourceForIntegration("display"));
        setup_protocol(display);
        a.setQuitOnLastWindowClosed(false);
        return a.exec();
    } else
    {
        qFatal() << "Unable to get wayland display!";
    }
}

#include "main.moc"
