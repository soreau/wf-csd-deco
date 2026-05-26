#include <algorithm>
#include "protocol.hpp"

GtkApplication *app;

struct custom_data
{
    GtkWidget *window;
    GtkWidget *area;
    gulong size_allocate_signal;
};

static void activate(GtkApplication* app, gpointer)
{
    GdkDisplay* display = gdk_display_get_default();
    setup_protocol(display);

    g_application_hold(G_APPLICATION(app));
}

static gboolean on_close_request(GtkWindow *window, gpointer data)
{
    GtkWidget *win = (GtkWidget *) data;
    auto it = std::find_if(view_to_decor.begin(), view_to_decor.end(),
    [&win](const std::pair<uint32_t, GtkWidget *>& element)
    {
        return element.second == win;
    });

    if (it != view_to_decor.end())
    {
        uint32_t id = it->first;
        view_to_decor.erase(id);
    }

    return false;
}

static void size_allocate(GObject *, GParamSpec *, gpointer data)
{
	printf("size_allocate\n");
	auto cdata = (custom_data *) data;
	auto win = cdata->window;
	auto area = cdata->area;
    GtkNative *native = gtk_widget_get_native(area);

    double surface_x, surface_y;
    gtk_native_get_surface_transform(native, &surface_x, &surface_y);

    graphene_rect_t bounds;
    gtk_widget_compute_bounds(area, GTK_WIDGET(native), &bounds);

    double final_x = surface_x + bounds.origin.x;
    double final_y = surface_y + bounds.origin.y;
    double width = bounds.size.width;
    double height = bounds.size.height;

    auto it = std::find_if(view_to_decor.begin(), view_to_decor.end(),
    [&win](const std::pair<uint32_t, GtkWidget *>& element)
    {
        return element.second == win;
    });

    if (it != view_to_decor.end())
    {
        uint32_t id = it->first;
        update_borders(id, final_y, final_x, final_x, final_x);
    }

    g_signal_handler_disconnect(win, cdata->size_allocate_signal);
    free(cdata);
}

static void on_menu_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    g_print("Menu item clicked\n");
}

GtkWidget *create_deco_window(std::string title)
{
    auto window = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(window), 300, 300);
    auto area = gtk_drawing_area_new();
    gtk_window_set_child(GTK_WINDOW(window), area);
    gtk_window_set_title(GTK_WINDOW(window), title.c_str());
    auto data = (custom_data *) malloc(sizeof(custom_data));
    data->window = window;
    data->area = area;
    data->size_allocate_signal = g_signal_connect(window, "notify::default-width", G_CALLBACK(size_allocate), data);
    g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), window);

    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(window), header);

    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Preferences", "app.prefs");
    g_menu_append(menu, "About", "app.about");

    GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    GtkWidget *button = gtk_menu_button_new();
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "open-menu-symbolic");

    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), button);

    GSimpleAction *prefs_action = g_simple_action_new("prefs", NULL);
    g_signal_connect(prefs_action, "activate", G_CALLBACK(on_menu_action), NULL);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(prefs_action));

    GSimpleAction *about_action = g_simple_action_new("about", NULL);
    g_signal_connect(about_action, "activate", G_CALLBACK(on_menu_action), NULL);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(about_action));

    gtk_window_present(GTK_WINDOW(window));

    return window;
}

void set_title(GtkWidget *window, const char *title)
{
    gtk_window_set_title(GTK_WINDOW(window), title);
}

int main(int argc, char **argv)
{
    int status;

    app = gtk_application_new("org.wf.sample-decorator", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    return status;
}
