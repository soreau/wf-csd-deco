#include "protocol.hpp"
GtkApplication *app;
GtkWidget *window;
GtkWidget *area;
gulong size_allocate_signal;

static void activate (GtkApplication* app, gpointer)
{
    GdkDisplay* display = gdk_display_get_default();
    setup_protocol(display);

    g_application_hold(G_APPLICATION(app));
}

static void size_allocate(GObject *, GParamSpec *, gpointer)
{
	printf("size_allocate\n");
    GtkNative *native = gtk_widget_get_native(area);

    double surface_x, surface_y;
    gtk_native_get_surface_transform(native, &surface_x, &surface_y);

    graphene_rect_t bounds;
    gtk_widget_compute_bounds(area, GTK_WIDGET(native), &bounds);

    double final_x = surface_x + bounds.origin.x;
    double final_y = surface_y + bounds.origin.y;
    double width = bounds.size.width;
    double height = bounds.size.height;

    update_borders(final_y, final_x, final_x, final_x);

    g_signal_handler_disconnect(window, size_allocate_signal);
}

GtkWidget *create_deco_window(std::string title)
{
    window = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(window), 300, 300);
    area = gtk_drawing_area_new();
    gtk_window_set_child(GTK_WINDOW(window), area);
    gtk_window_set_title(GTK_WINDOW(window), title.c_str());
    size_allocate_signal = g_signal_connect(window, "notify::default-width", G_CALLBACK (size_allocate), NULL);

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
    g_signal_connect(app, "activate", G_CALLBACK (activate), NULL);
    status = g_application_run(G_APPLICATION (app), argc, argv);
    g_object_unref (app);

    return status;
}
