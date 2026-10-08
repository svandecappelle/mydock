#include <gtk/gtk.h>

#include "config.h"
#include "dock.h"
#include "platform.h"

static Dock *dock;
static gboolean open_preferences; /* --preferences given to the primary instance */

static void on_activate(GtkApplication *app, gpointer data)
{
    if (!dock) {
        dock = dock_new(app, dock_config_load());
        gtk_widget_show_all(dock_get_window(dock));
    }
    if (open_preferences) {
        open_preferences = FALSE;
        dock_show_preferences(dock, GDK_CURRENT_TIME);
    }
}

static void on_preferences_action(GSimpleAction *action, GVariant *param, gpointer app)
{
    if (dock)
        dock_show_preferences(dock, GDK_CURRENT_TIME);
}

/* `macdock --preferences`: open the settings of the running dock, or start
 * the dock with its settings open. */
static int on_local_options(GApplication *app, GVariantDict *options, gpointer data)
{
    if (g_variant_dict_contains(options, "version")) {
        g_print("macdock %s\n", MACDOCK_VERSION);
        return 0;
    }
    if (!g_variant_dict_contains(options, "preferences"))
        return -1;
    g_autoptr(GError) err = NULL;
    if (!g_application_register(app, NULL, &err)) {
        g_printerr("macdock: %s\n", err->message);
        return 1;
    }
    if (g_application_get_is_remote(app)) {
        g_action_group_activate_action(G_ACTION_GROUP(app), "preferences", NULL);
        return 0;
    }
    open_preferences = TRUE;
    return -1;
}

static gboolean on_quit_signal(gpointer app)
{
    g_application_quit(G_APPLICATION(app));
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("io.github.macdock", G_APPLICATION_DEFAULT_FLAGS);

    static const GOptionEntry options[] = {
        { "preferences", 'p', 0, G_OPTION_ARG_NONE, NULL, "Open the dock settings", NULL },
        { "version", 0, 0, G_OPTION_ARG_NONE, NULL, "Print the version and exit", NULL },
        { NULL },
    };
    g_application_add_main_option_entries(G_APPLICATION(app), options);
    static const GActionEntry actions[] = {
        { .name = "preferences", .activate = on_preferences_action },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), actions, G_N_ELEMENTS(actions), app);

    g_signal_connect(app, "handle-local-options", G_CALLBACK(on_local_options), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    platform_on_quit_request(on_quit_signal, app);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
