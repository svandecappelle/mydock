#include <glib-unix.h>
#include <gtk/gtk.h>
#include <signal.h>

#include "config.h"
#include "dock.h"

static void on_activate(GtkApplication *app, gpointer data)
{
    if (gtk_application_get_windows(app)) /* already running: single instance */
        return;
    Dock *dock = dock_new(app, dock_config_load());
    gtk_widget_show_all(dock_get_window(dock));
}

static gboolean on_quit_signal(gpointer app)
{
    g_application_quit(G_APPLICATION(app));
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("io.github.macdock", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_unix_signal_add(SIGINT, on_quit_signal, app);
    g_unix_signal_add(SIGTERM, on_quit_signal, app);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
