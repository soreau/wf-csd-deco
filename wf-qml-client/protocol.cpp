#include "protocol.hpp"
#include "wf-decorator-client-protocol.h"
#include "Application.h"
#include "DecorationWindow.h"
#include <wayland-client.h>
#include <cstring>
// #include <QDebug>
#include <QGuiApplication>
static wf_decorator_manager *decorator_manager;
static uint32_t decorator_global_id;
// Forward declarations
// extern QMap<uint32_t, QWidget*> view_to_decor;
extern QMap<uint32_t, QWindow*> view_to_decor;
static void create_new_decoration(void*, wf_decorator_manager*, uint32_t view_id)
{
    auto window = create_deco_window(view_id);
    view_to_decor[view_id] = window;
}

static void destroy_decoration(void*, wf_decorator_manager*, uint32_t view_id)
{
    destroy_deco_window(view_id);
}

static void title_changed(void*, wf_decorator_manager*, uint32_t view, const char *new_title)
{
    set_title(view_to_decor[view], new_title);
}

static void app_id_changed(void*, wf_decorator_manager*, uint32_t view, const char *new_app_id)
{
    set_app_id(view_to_decor[view], new_app_id);
}

static void notify_focus(void*, wf_decorator_manager*, uint32_t view)
{
    // for (uint32_t view_id : view_to_decor.keys())
    // {
    // if (view_id == view)
    // {
    // qobject_cast<DecorationWindow*>(view_to_decor[view_id])->markAsActive(true);
    // } else
    // {
    // qobject_cast<DecorationWindow*>(view_to_decor[view_id])->markAsActive(false);
    // }
    // }
}

static void notify_tiled(void*, wf_decorator_manager*, uint32_t view, uint32_t edges)
{
    // qobject_cast<DecorationWindow*>(view_to_decor[view])->notifyTiledEdges(edges);
}

const wf_decorator_manager_listener decorator_listener =
{
    create_new_decoration,
    destroy_decoration,
    title_changed,
    app_id_changed,
    notify_focus,
    notify_tiled
};
void registry_add_object(void*, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t)
{
    // qDebug() << "new registry:" << interface;
    if ((strcmp(interface, wf_decorator_manager_interface.name) == 0) &&
        (decorator_global_id != id))
    {
        // qDebug() << "bind it";
        decorator_manager = (wf_decorator_manager*)wl_registry_bind(registry, id,
            &wf_decorator_manager_interface, 1u);
        wf_decorator_manager_add_listener(decorator_manager, &decorator_listener, NULL);
        decorator_global_id = id;
    }
}

void registry_remove_object(void*, struct wl_registry*, uint32_t id)
{
    if (id == decorator_global_id)
    {
        wf_decorator_manager_destroy(decorator_manager);
        decorator_manager = NULL;
    }
}

static struct wl_registry_listener registry_listener = {
    &registry_add_object,
    &registry_remove_object
};
void update_theme(std::string theme)
{
    app->set_theme(theme);

    for (auto [id, v] : view_to_decor.asKeyValueRange())
    {
        if (auto *dec = qobject_cast<DecorationWindow*>(v))
        {
            dec->setup_items();
        }
    }
}

void close_request(uint32_t id)
{
    if (!decorator_manager)
    {
        return;
    }

    wf_decorator_manager_close_request(decorator_manager, id);
}

void update_borders(uint32_t id, uint32_t top, uint32_t bottom, uint32_t left, uint32_t right,
    uint32_t border)
{
    if (!decorator_manager)
    {
        return;
    }

    wf_decorator_manager_update_borders(decorator_manager, id, top, bottom, left, right, border);
}

void group_windows(uint32_t parent_id, uint32_t child_id)
{
    if (!decorator_manager)
    {
        return;
    }

    wf_decorator_manager_group_windows(decorator_manager, parent_id, child_id);
}

void select_window(uint32_t id)
{
    if (!decorator_manager)
    {
        return;
    }

    wf_decorator_manager_select_window(decorator_manager, id);
}

void ungroup_window(uint32_t id)
{
    if (!decorator_manager)
    {
        return;
    }

    wf_decorator_manager_ungroup_window(decorator_manager, id);
}

void setup_protocol(void *display)
{
    // In Qt6, you would get the Wayland display differently
    // This is a placeholder - you'll need to adapt this for Qt's Wayland integration
    auto display_wl = (wl_display*)display;
    auto registry   = wl_display_get_registry(display_wl);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display_wl);
}

/** Create Deco Window */
DecorationWindow *create_deco_window(uint32_t wf_id)
{
    DecorationWindow *window = new DecorationWindow(wf_id);
    window->setWindowTitle(QString("__wf_decorator:%1").arg(wf_id));
    window->show();
    return window;
}

void destroy_deco_window(uint32_t wf_id)
{
    auto window = view_to_decor[wf_id];
    if (window)
    {
        window->close();
        delete window;
        view_to_decor.remove(wf_id);
    }
}

void set_title(QWindow *window, const char *title)
{
    if (auto *dec = qobject_cast<DecorationWindow*>(window))
    {
        dec->setWindowTitle(QString::fromUtf8(title));
    }
}

void set_app_id(QWindow *window, const char *app_id)
{
    if (auto *dec = qobject_cast<DecorationWindow*>(window))
    {
        dec->setAppId(QString::fromUtf8(app_id));
    }
}
