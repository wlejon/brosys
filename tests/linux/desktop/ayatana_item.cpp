// A third-party StatusNotifierItem: libayatana-appindicator3 (GTK 3,
// libdbusmenu-gtk for the menu), as real tray apps use it. Needs an X
// display (Xvfb in the test). Prints what happens to it:
//
//   READY   CONNECTED <0|1>   CLICKED <label>   SCROLL <delta> <direction>
//   CHANGED   MENU-ADDED
//
// SIGUSR1 changes icon / title / status, SIGUSR2 adds a menu item,
// SIGTERM quits.
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>

#include <csignal>
#include <cstdio>
#include <string>

namespace {

AppIndicator* g_indicator = nullptr;
GtkWidget* g_menu = nullptr;

void say(const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

void on_item_activate(GtkMenuItem* item, gpointer) { say(std::string("CLICKED ") + gtk_menu_item_get_label(item)); }

void on_connection_changed(AppIndicator*, gboolean connected, gpointer) {
    say(std::string("CONNECTED ") + (connected ? "1" : "0"));
}

void on_scroll(AppIndicator*, gint delta, guint direction, gpointer) {
    say("SCROLL " + std::to_string(delta) + " " + std::to_string(direction));
}

gboolean on_usr1(gpointer) {
    app_indicator_set_icon_full(g_indicator, "dialog-warning", "warning");
    app_indicator_set_title(g_indicator, "Ayatana Changed");
    app_indicator_set_status(g_indicator, APP_INDICATOR_STATUS_ATTENTION);
    say("CHANGED");
    return G_SOURCE_CONTINUE;
}

gboolean on_usr2(gpointer) {
    GtkWidget* item = gtk_menu_item_new_with_label("Added Later");
    g_signal_connect(item, "activate", G_CALLBACK(on_item_activate), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), item);
    gtk_widget_show(item);
    say("MENU-ADDED");
    return G_SOURCE_CONTINUE;
}

gboolean on_term(gpointer) {
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

}  // namespace

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    g_indicator = app_indicator_new("brosys-ayatana", "dialog-information", APP_INDICATOR_CATEGORY_COMMUNICATIONS);
    app_indicator_set_attention_icon_full(g_indicator, "dialog-error", "error");
    app_indicator_set_title(g_indicator, "Ayatana Title");
    g_signal_connect(g_indicator, "connection-changed", G_CALLBACK(on_connection_changed), nullptr);
    g_signal_connect(g_indicator, "scroll-event", G_CALLBACK(on_scroll), nullptr);

    g_menu = gtk_menu_new();
    GtkWidget* hello = gtk_menu_item_new_with_label("Hello");
    g_signal_connect(hello, "activate", G_CALLBACK(on_item_activate), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), hello);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), gtk_separator_menu_item_new());
    GtkWidget* toggle = gtk_check_menu_item_new_with_label("Toggle");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(toggle), TRUE);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), toggle);
    GtkWidget* off = gtk_menu_item_new_with_label("Disabled");
    gtk_widget_set_sensitive(off, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), off);
    gtk_widget_show_all(g_menu);
    app_indicator_set_menu(g_indicator, GTK_MENU(g_menu));
    app_indicator_set_secondary_activate_target(g_indicator, hello);
    app_indicator_set_status(g_indicator, APP_INDICATOR_STATUS_ACTIVE);

    g_unix_signal_add(SIGUSR1, on_usr1, nullptr);
    g_unix_signal_add(SIGUSR2, on_usr2, nullptr);
    g_unix_signal_add(SIGTERM, on_term, nullptr);
    say("READY");
    gtk_main();
    g_object_unref(g_indicator);
    return 0;
}
