// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#include "../src/ui/desktop_entry.h"
#include <gio/gio.h>
#include <glib.h>
#include <stdio.h>

/* ============================================================================
 * EXEC FIELD CODES
 * ============================================================================ */

static void test_strip_field_codes(void) {
  char *exec = lambdock_exec_strip_field_codes("firefox %u");
  g_assert_cmpstr(exec, ==, "firefox");
  g_free(exec);

  exec = lambdock_exec_strip_field_codes("nautilus --new-window %U");
  g_assert_cmpstr(exec, ==, "nautilus --new-window");
  g_free(exec);

  /* Everything the spec says expands to nothing disappears, whatever the code's
   * case, and the leftover gaps are collapsed. */
  exec = lambdock_exec_strip_field_codes("app %f %F %i %c %k %d %D %n %N %v");
  g_assert_cmpstr(exec, ==, "app");
  g_free(exec);

  /* A literal percent stays, written as %%. */
  exec = lambdock_exec_strip_field_codes("printf 100%%");
  g_assert_cmpstr(exec, ==, "printf 100%");
  g_free(exec);

  /* Unknown codes are kept verbatim instead of silently vanishing. */
  exec = lambdock_exec_strip_field_codes("app %z");
  g_assert_cmpstr(exec, ==, "app %z");
  g_free(exec);

  /* Leading/trailing whitespace and a lone trailing '%' are handled. */
  exec = lambdock_exec_strip_field_codes("  app %u  ");
  g_assert_cmpstr(exec, ==, "app");
  g_free(exec);

  exec = lambdock_exec_strip_field_codes("app %");
  g_assert_cmpstr(exec, ==, "app");
  g_free(exec);

  g_assert_null(lambdock_exec_strip_field_codes(NULL));
}

/* ============================================================================
 * PARSING
 * ============================================================================ */

static const char *VALID_ENTRY = "[Desktop Entry]\n"
                                 "Type=Application\n"
                                 "Name=Web Browser\n"
                                 "Exec=firefox %u\n"
                                 "Icon=firefox\n";

static void test_parse_valid(void) {
  LambdockDesktopEntry *entry =
      lambdock_desktop_entry_new_from_data(VALID_ENTRY, "firefox.desktop");

  g_assert_nonnull(entry);
  g_assert_cmpstr(entry->id, ==, "firefox.desktop");
  g_assert_cmpstr(entry->name, ==, "Web Browser");
  g_assert_cmpstr(entry->exec, ==, "firefox");
  g_assert_cmpstr(entry->icon, ==, "firefox");
  g_assert_false(entry->no_display);
  g_assert_false(entry->hidden);
  lambdock_desktop_entry_free(entry);
  lambdock_desktop_entry_free(NULL);
}

static void test_parse_rejects(void) {
  /* Not a key file at all. */
  g_assert_null(lambdock_desktop_entry_new_from_data("this is not a desktop",
                                                     "x.desktop"));
  /* Empty data. */
  g_assert_null(lambdock_desktop_entry_new_from_data("", "x.desktop"));
  g_assert_null(lambdock_desktop_entry_new_from_data(NULL, "x.desktop"));

  /* Wrong group: only [Desktop Entry] describes the application itself. */
  g_assert_null(lambdock_desktop_entry_new_from_data(
      "[Desktop Action new]\nName=Nope\nExec=x\n", "x.desktop"));

  /* Not an application (links, directories...). */
  g_assert_null(lambdock_desktop_entry_new_from_data(
      "[Desktop Entry]\nType=Link\nName=A link\nURL=https://example.org\n",
      "x.desktop"));

  /* No name / no exec. */
  g_assert_null(lambdock_desktop_entry_new_from_data(
      "[Desktop Entry]\nType=Application\nExec=x\n", "x.desktop"));
  g_assert_null(lambdock_desktop_entry_new_from_data(
      "[Desktop Entry]\nType=Application\nName=No exec\n", "x.desktop"));
}

static void test_parse_flags_and_icon_default(void) {
  LambdockDesktopEntry *entry = lambdock_desktop_entry_new_from_data(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=Hidden Thing\n"
      "Exec=hidden\n"
      "NoDisplay=true\n"
      "Hidden=true\n",
      "hidden.desktop");

  g_assert_nonnull(entry);
  /* Parsing keeps the flags: hiding is the scanner's decision, because a
   * dropped .desktop file should still be able to add the app. */
  g_assert_true(entry->no_display);
  g_assert_true(entry->hidden);
  g_assert_cmpstr(entry->icon, ==, "application-x-executable");
  lambdock_desktop_entry_free(entry);

  /* An empty Icon= falls back to the generic executable icon too. */
  entry = lambdock_desktop_entry_new_from_data("[Desktop Entry]\n"
                                               "Type=Application\n"
                                               "Name=No icon\n"
                                               "Exec=x\n"
                                               "Icon=\n",
                                               "noicon.desktop");
  g_assert_nonnull(entry);
  g_assert_cmpstr(entry->icon, ==, "application-x-executable");
  lambdock_desktop_entry_free(entry);
}

static void test_parse_localized_name(void) {
  LambdockDesktopEntry *entry = lambdock_desktop_entry_new_from_data(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=Default Name\n"
      "Name[de]=Deutscher Name\n"
      "Exec=app\n",
      "i18n.desktop");

  g_assert_nonnull(entry);
  /* The default locale is "C", so the untranslated Name wins. */
  g_assert_cmpstr(entry->name, ==, "Default Name");
  lambdock_desktop_entry_free(entry);
}

static void test_parse_file(void) {
  char *dir = g_dir_make_tmp("lambdock-desktop-XXXXXX", NULL);
  g_assert_nonnull(dir);

  char *path = g_build_filename(dir, "sample.desktop", NULL);
  g_assert_true(g_file_set_contents(path, VALID_ENTRY, -1, NULL));

  LambdockDesktopEntry *entry = lambdock_desktop_entry_new_from_file(path);
  g_assert_nonnull(entry);
  g_assert_cmpstr(entry->id, ==, "sample.desktop");
  g_assert_cmpstr(entry->exec, ==, "firefox");
  lambdock_desktop_entry_free(entry);

  /* Missing files are simply not entries. */
  char *missing = g_build_filename(dir, "missing.desktop", NULL);
  g_assert_null(lambdock_desktop_entry_new_from_file(missing));
  g_assert_null(lambdock_desktop_entry_new_from_file(NULL));

  remove(path);
  g_free(missing);
  g_free(path);
  remove(dir);
  g_free(dir);
}

/* ============================================================================
 * SCANNING
 * ============================================================================ */

static void write_entry(const char *dir, const char *filename, const char *name,
                        const char *exec) {
  char *path = g_build_filename(dir, filename, NULL);
  char *data = g_strdup_printf("[Desktop Entry]\n"
                               "Type=Application\n"
                               "Name=%s\n"
                               "Exec=%s\n"
                               "Icon=%s\n",
                               name, exec, exec);
  g_assert_true(g_file_set_contents(path, data, -1, NULL));
  g_free(data);
  g_free(path);
}

static gboolean entries_contain(GPtrArray *entries, const char *name) {
  for (guint i = 0; i < entries->len; i++) {
    LambdockDesktopEntry *entry = g_ptr_array_index(entries, i);
    if (g_strcmp0(entry->name, name) == 0)
      return TRUE;
  }
  return FALSE;
}

static int entries_index_of(GPtrArray *entries, const char *name) {
  for (guint i = 0; i < entries->len; i++) {
    LambdockDesktopEntry *entry = g_ptr_array_index(entries, i);
    if (g_strcmp0(entry->name, name) == 0)
      return (int)i;
  }
  return -1;
}

static int entries_count_named(GPtrArray *entries, const char *name) {
  int count = 0;
  for (guint i = 0; i < entries->len; i++) {
    LambdockDesktopEntry *entry = g_ptr_array_index(entries, i);
    if (g_strcmp0(entry->name, name) == 0)
      count++;
  }
  return count;
}

static LambdockDesktopEntry *entry_named(GPtrArray *entries,
                                         const char *name) {
  for (guint i = 0; i < entries->len; i++) {
    LambdockDesktopEntry *entry = g_ptr_array_index(entries, i);
    if (g_strcmp0(entry->name, name) == 0)
      return entry;
  }
  return NULL;
}

static void test_scan(const char *data_dir) {
  char *apps = g_build_filename(data_dir, "applications", NULL);

  write_entry(apps, "zebra.desktop", "Zebra", "zebra");
  write_entry(apps, "alpha.desktop", "Alpha", "alpha");

  char *path = g_build_filename(apps, "hidden.desktop", NULL);
  char *data = g_strdup_printf("[Desktop Entry]\n"
                               "Type=Application\n"
                               "Name=Hidden\n"
                               "Exec=hidden\n"
                               "Hidden=true\n");
  g_assert_true(g_file_set_contents(path, data, -1, NULL));
  g_free(data);
  g_free(path);

  path = g_build_filename(apps, "nodisplay.desktop", NULL);
  data = g_strdup_printf("[Desktop Entry]\n"
                         "Type=Application\n"
                         "Name=NoDisplay\n"
                         "Exec=nodisplay\n"
                         "NoDisplay=true\n");
  g_assert_true(g_file_set_contents(path, data, -1, NULL));
  g_free(data);
  g_free(path);

  GPtrArray *entries = lambdock_desktop_entries_scan();
  g_assert_nonnull(entries);

  /* The two visible applications are listed, in name order, and the ones the
   * user or the distribution hid are not. */
  g_assert_true(entries_contain(entries, "Alpha"));
  g_assert_true(entries_contain(entries, "Zebra"));
  g_assert_false(entries_contain(entries, "Hidden"));
  g_assert_false(entries_contain(entries, "NoDisplay"));
  g_assert_cmpint(entries_index_of(entries, "Alpha"), <,
                  entries_index_of(entries, "Zebra"));

  lambdock_desktop_entries_free(entries);
  lambdock_desktop_entries_free(NULL);

  g_free(apps);
}

/* ============================================================================
 * STARTUP WM CLASS AND STEMS
 * ============================================================================ */

static void test_parse_wm_class_and_stem(void) {
  LambdockDesktopEntry *entry = lambdock_desktop_entry_new_from_data(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=Files\n"
      "Exec=nautilus --new-window %U\n"
      "Icon=org.gnome.Nautilus\n"
      "StartupWMClass=org.gnome.Nautilus;nautilus;\n",
      "org.gnome.Nautilus.desktop");

  g_assert_nonnull(entry);
  g_assert_cmpstr(entry->stem, ==, "org.gnome.Nautilus");
  /* Only the first class is kept: that is the one compositors report. */
  g_assert_cmpstr(entry->wm_class, ==, "org.gnome.Nautilus");
  lambdock_desktop_entry_free(entry);

  /* No StartupWMClass: no wm_class, and the stem still comes off the id. */
  entry = lambdock_desktop_entry_new_from_data("[Desktop Entry]\n"
                                               "Type=Application\n"
                                               "Name=Plain\n"
                                               "Exec=plain\n",
                                               "plain.desktop");
  g_assert_nonnull(entry);
  g_assert_cmpstr(entry->stem, ==, "plain");
  g_assert_null(entry->wm_class);
  lambdock_desktop_entry_free(entry);
}static void test_parse_categories(void) {
  LambdockDesktopEntry *entry = lambdock_desktop_entry_new_from_data(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=Firefox\n"
      "Exec=firefox %u\n"
      "Categories=Network;WebBrowser;\n",
      "firefox.desktop");

  g_assert_nonnull(entry);
  g_assert_cmpstr(entry->categories, ==, "Network;WebBrowser;");
  g_assert_true(lambdock_desktop_entry_has_category(entry, "WebBrowser"));
  g_assert_true(lambdock_desktop_entry_has_category(entry, "webbrowser"));
  g_assert_true(lambdock_desktop_entry_has_category(entry, "Network"));
  g_assert_false(lambdock_desktop_entry_has_category(entry, "Music"));

  /* An entry with no Categories= reports none, and matching a NULL entry or a
   * NULL category is false rather than a crash. */
  g_assert_false(lambdock_desktop_entry_has_category(entry, NULL));
  g_assert_false(lambdock_desktop_entry_has_category(NULL, "Music"));
  lambdock_desktop_entry_free(entry);

  entry = lambdock_desktop_entry_new_from_data("[Desktop Entry]\n"
                                               "Type=Application\n"
                                               "Name=Plain\n"
                                               "Exec=plain\n",
                                               "plain.desktop");
  g_assert_nonnull(entry);
  g_assert_null(entry->categories);
  g_assert_false(lambdock_desktop_entry_has_category(entry, "WebBrowser"));
  lambdock_desktop_entry_free(entry);

  /* An empty Categories= is the same as none. */
  entry = lambdock_desktop_entry_new_from_data("[Desktop Entry]\n"
                                               "Type=Application\n"
                                               "Name=Empty\n"
                                               "Exec=empty\n"
                                               "Categories=\n",
                                               "empty.desktop");
  g_assert_nonnull(entry);
  g_assert_null(entry->categories);
  lambdock_desktop_entry_free(entry);
}

/* ============================================================================
 * NAMES FROM APP IDS
 * ============================================================================
 */

static void test_app_id_to_name(void) {
  struct {
    const char *app_id;
    const char *expected;
  } cases[] = {
      /* reverse-DNS: the vendor prefix goes, camel case is split */
      {"org.gnome.SystemMonitor", "System Monitor"},
      {"org.gnome.SystemMonitor.desktop", "System Monitor"},
      {"org.wezfurlong.wezterm", "Wezterm"},
      {"com.google.Chrome", "Chrome"},
      {"org.kde.gwenview", "Gwenview"},
      {"io.github.foo.Bar", "Bar"},
      /* single-component and dashed ids */
      {"firefox", "Firefox"},
      {"wezterm", "Wezterm"},
      {"google-chrome", "Google Chrome"},
      {"libreoffice-writer", "Libreoffice Writer"},
      {"org.gnome.Software_Store", "Software Store"},
      /* already-cased ids are left alone */
      {"Alacritty", "Alacritty"},
      {"Freebuff", "Freebuff"},
      /* acronyms survive, and an acronym followed by a word is split */
      {"org.gnome.ZFSManager", "ZFS Manager"},
      {"org.gnome.Nautilus", "Nautilus"},
  };

  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    char *name = lambdock_app_id_to_name(cases[i].app_id);
    g_assert_cmpstr(name, ==, cases[i].expected);
    g_free(name);
  }

  g_assert_null(lambdock_app_id_to_name(NULL));
  g_assert_null(lambdock_app_id_to_name(""));
}

/* ============================================================================
 * THE CACHE
 * ============================================================================ */

static void write_app(const char *dir, const char *filename, const char *body) {
  char *path = g_build_filename(dir, filename, NULL);
  g_assert_true(g_file_set_contents(path, body, -1, NULL));
  g_free(path);
}

/* The same application installed twice -- a distro package and a Flatpak export
 * of it -- has two differently named desktop files, and the grid would show a
 * tile for each. Two *different* applications that happen to share a name are
 * two applications, and both stay. */
static void test_scan_collapses_duplicate_applications(const char *data_dir) {
  char *apps = g_build_filename(data_dir, "applications", NULL);

  /* Distro LibreOffice Writer and its Flatpak export: same name, same window
   * class, different icon. */
  write_app(apps, "libreoffice-writer.desktop",
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=LibreOffice Writer\n"
            "Exec=libreoffice --writer %U\n"
            "Icon=libreoffice-writer\n"
            "StartupWMClass=libreoffice-writer\n");
  write_app(apps, "org.libreoffice.LibreOffice.writer.desktop",
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=LibreOffice Writer\n"
            "Exec=/usr/bin/flatpak run --branch=stable org.libreoffice.LibreOffice "
            "--writer %U\n"
            "Icon=org.libreoffice.LibreOffice.writer\n"
            "StartupWMClass=libreoffice-writer\n");

  /* Two applications called "Displays": no window class on either, and
   * different icons, so they are not the same thing and both are kept. */
  write_app(apps, "network.cycles.wdisplays.desktop",
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Displays\n"
            "Exec=wdisplays\n"
            "Icon=wdisplays\n");
  write_app(apps, "gnome-display-panel.desktop",
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Displays\n"
            "Exec=gnome-control-center display\n"
            "Icon=org.gnome.Settings-display-symbolic\n");

  /* Neither file declares a window class: the icon is the identity they share,
   * and with the same name it is the same application again. */
  write_app(apps, "org.gtk.WidgetFactory4.desktop",
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Widget Factory\n"
            "Exec=gtk4-widget-factory\n"
            "Icon=org.gtk.WidgetFactory4\n");
  write_app(apps, "widget-factory-old.desktop",
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Widget Factory\n"
            "Exec=gtk4-widget-factory --old\n"
            "Icon=org.gtk.WidgetFactory4\n");

  GPtrArray *entries = lambdock_desktop_entries_scan();
  g_assert_nonnull(entries);
  g_assert_cmpint(entries_count_named(entries, "LibreOffice Writer"), ==, 1);
  g_assert_cmpint(entries_count_named(entries, "Widget Factory"), ==, 1);
  g_assert_cmpint(entries_count_named(entries, "Displays"), ==, 2);

  /* The copies that lose are kept on the winner instead of thrown away: the
   * launchpad offers them from the tile's menu, so picking the copy to start
   * is the user's call. Which of the two files wins depends on the directory
   * order, so this checks that the other one is the one attached. */
  LambdockDesktopEntry *writer = entry_named(entries, "LibreOffice Writer");
  g_assert_nonnull(writer);
  g_assert_nonnull(writer->alternates);
  g_assert_cmpuint(writer->alternates->len, ==, 1);

  LambdockDesktopEntry *copy = g_ptr_array_index(writer->alternates, 0);
  g_assert_cmpstr(copy->name, ==, "LibreOffice Writer");
  g_assert_cmpstr(copy->id, !=, writer->id);
  g_assert_cmpstr(copy->exec, !=, writer->exec);

  /* The two applications called "Displays" are not copies of each other, so
   * neither carries the other. */
  int displays = 0;
  for (guint i = 0; i < entries->len; i++) {
    LambdockDesktopEntry *entry = g_ptr_array_index(entries, i);
    if (g_strcmp0(entry->name, "Displays") != 0)
      continue;
    displays++;
    g_assert_null(entry->alternates);
  }
  g_assert_cmpint(displays, ==, 2);

  lambdock_desktop_entries_free(entries);
  g_free(apps);
}

/* Where a copy comes from is what tells two of them apart in the menu. */
static void test_source_label(void) {
  LambdockDesktopEntry *entry = lambdock_desktop_entry_new_from_data(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=Files\n"
      "Exec=nautilus\n",
      "org.gnome.Nautilus.desktop");
  g_assert_nonnull(entry);

  /* An entry that was never read from a file has no origin to name. */
  g_assert_null(entry->path);
  char *label = lambdock_desktop_entry_source_label(entry);
  g_assert_cmpstr(label, ==, "Application");
  g_free(label);

  /* A Flatpak export... */
  entry->path = g_strdup("/home/joe/.local/share/flatpak/exports/share/"
                         "applications/org.gnome.Nautilus.desktop");
  label = lambdock_desktop_entry_source_label(entry);
  g_assert_cmpstr(label, ==, "Flatpak");
  g_free(label);

  /* ...a file the distribution installed... */
  g_free(entry->path);
  entry->path = g_strdup("/var/lib/flatpak/exports/share/applications/"
                         "org.gnome.Nautilus.desktop");
  label = lambdock_desktop_entry_source_label(entry);
  g_assert_cmpstr(label, ==, "Flatpak");
  g_free(label);

  g_free(entry->path);
  entry->path = g_strdup("/usr/share/applications/org.gnome.Nautilus.desktop");
  label = lambdock_desktop_entry_source_label(entry);
  g_assert_cmpstr(label, ==, "System");
  g_free(label);

  /* ...and one of the user's own, which is what the private XDG tree of this
   * test process looks like. */
  g_free(entry->path);
  entry->path = g_build_filename(g_get_user_data_dir(), "applications",
                                 "org.gnome.Nautilus.desktop", NULL);
  label = lambdock_desktop_entry_source_label(entry);
  g_assert_cmpstr(label, ==, "Your files");
  g_free(label);

  lambdock_desktop_entry_free(entry);
  g_assert_null(lambdock_desktop_entry_source_label(NULL));
}

static void test_cache(const char *data_dir) {
  char *apps = g_build_filename(data_dir, "applications", NULL);

  write_app(apps, "org.gnome.Nautilus.desktop",
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Files\n"
            "Exec=nautilus --new-window %U\n"
            "Icon=org.gnome.Nautilus\n"
            "StartupWMClass=org.gnome.Nautilus\n");
  write_app(apps, "org.wezfurlong.wezterm.desktop",
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=WezTerm\n"
            "Exec=wezterm start --always-new-process\n"
            "Icon=org.wezfurlong.wezterm\n"
            "StartupWMClass=org.wezfurlong.wezterm\n");
  write_app(apps, "google-chrome.desktop",
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Google Chrome\n"
            "Exec=/opt/google/chrome/chrome %U\n"
            "Icon=google-chrome\n");
  write_app(apps, "org.gnome.SystemMonitor.desktop",
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=System Monitor\n"
            "Name[de]=Systemüberwachung\n"
            "Exec=gnome-system-monitor\n"
            "Icon=org.gnome.SystemMonitor\n");
  write_app(apps, "hidden.desktop",
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Hidden\n"
            "Exec=hidden\n"
            "Hidden=true\n");

  lambdock_desktop_cache_invalidate();

  /* Every spelling a compositor may report resolves to the same entry. */
  struct {
    const char *app_id;
    const char *expected_id;
  } lookups[] = {
      {"org.gnome.Nautilus", "org.gnome.Nautilus.desktop"},
      {"org.gnome.Nautilus.desktop", "org.gnome.Nautilus.desktop"},
      {"nautilus", "org.gnome.Nautilus.desktop"},      /* exec base name */
      {"NAUTILUS", "org.gnome.Nautilus.desktop"},      /* case-insensitive */
      {"org.wezfurlong.wezterm", "org.wezfurlong.wezterm.desktop"},
      {"wezterm", "org.wezfurlong.wezterm.desktop"},
      {"WezTerm", "org.wezfurlong.wezterm.desktop"},
      {"google-chrome", "google-chrome.desktop"},
      {"chrome", "google-chrome.desktop"},          /* last part of the exec */
      {"org.gnome.SystemMonitor", "org.gnome.SystemMonitor.desktop"},
      {"SystemMonitor", "org.gnome.SystemMonitor.desktop"}, /* short form */
  };

  for (gsize i = 0; i < G_N_ELEMENTS(lookups); i++) {
    const LambdockDesktopEntry *entry =
        lambdock_desktop_entry_lookup(lookups[i].app_id);
    g_assert_nonnull(entry);
    g_assert_cmpstr(entry->id, ==, lookups[i].expected_id);
  }

  g_assert_null(lambdock_desktop_entry_lookup("no-such-app"));
  g_assert_null(lambdock_desktop_entry_lookup(NULL));

  /* Hidden entries are not indexed, so a window that reports one gets the
   * derived name instead. */
  const LambdockDesktopEntry *hidden = lambdock_desktop_entry_lookup("hidden");
  g_assert_null(hidden);

  /* The cached list is what the launchpad would show: the visible entries,
   * name-sorted. (The scan test wrote a few entries of its own into the same
   * directory, so this checks order and membership rather than a count.) */
  GPtrArray *cached = lambdock_desktop_entries_cached();
  g_assert_nonnull(cached);
  g_assert_cmpuint(cached->len, >=, 4);

  gboolean saw_files = FALSE;
  gboolean saw_system_monitor = FALSE;
  const char *previous = NULL;
  for (guint i = 0; i < cached->len; i++) {
    LambdockDesktopEntry *entry = g_ptr_array_index(cached, i);
    g_assert_cmpstr(entry->name, !=, "Hidden");
    if (previous)
      g_assert_cmpint(g_utf8_collate(previous, entry->name), <=, 0);
    previous = entry->name;

    if (g_strcmp0(entry->name, "Files") == 0)
      saw_files = TRUE;
    if (g_strcmp0(entry->name, "System Monitor") == 0)
      saw_system_monitor = TRUE;
  }
  g_assert_true(saw_files);
  g_assert_true(saw_system_monitor);

  /* Names and icons come from the entry when there is one... */
  char *name = lambdock_app_name_for("SystemMonitor");
  g_assert_cmpstr(name, ==, "System Monitor");
  g_free(name);

  char *icon = lambdock_app_icon_for("nautilus");
  g_assert_cmpstr(icon, ==, "org.gnome.Nautilus");
  g_free(icon);

  /* ... and are derived from the id when there is not. */
  name = lambdock_app_name_for("org.gnome.SomeUnknownApp");
  g_assert_cmpstr(name, ==, "Some Unknown App");
  g_free(name);

  icon = lambdock_app_icon_for("org.gnome.SomeUnknownApp");
  g_assert_cmpstr(icon, ==, "org.gnome.SomeUnknownApp");
  g_free(icon);

  g_assert_null(lambdock_app_name_for(NULL));
  g_assert_null(lambdock_app_icon_for(NULL));

  /* Installing an application: the cache picks it up once it is invalidated. */
  write_app(apps, "late-comer.desktop",
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Late Comer\n"
            "Exec=late-comer\n"
            "Icon=late-comer\n");
  guint before = lambdock_desktop_entries_cached()->len;
  g_assert_null(lambdock_desktop_entry_lookup("late-comer"));
  lambdock_desktop_cache_invalidate();
  g_assert_nonnull(lambdock_desktop_entry_lookup("late-comer"));
  g_assert_cmpuint(lambdock_desktop_entries_cached()->len, ==, before + 1);

  g_free(apps);
}

/* The names have to be localized: the entry's Name[xx] for the session's
 * language. g_get_language_names() caches per process, so this runs in a
 * subprocess with LANGUAGE=de. */
static void test_localized_names(void) {
  if (g_test_subprocess()) {
    g_setenv("LANGUAGE", "de", TRUE);

    const LambdockDesktopEntry *entry =
        lambdock_desktop_entry_lookup("org.gnome.SystemMonitor");
    g_assert_nonnull(entry);
    g_assert_cmpstr(entry->name, ==, "Systemüberwachung");

    char *name = lambdock_app_name_for("org.gnome.SystemMonitor");
    g_assert_cmpstr(name, ==, "Systemüberwachung");
    g_free(name);
    return;
  }

  g_test_trap_subprocess(NULL, 0, 0);
  g_test_trap_assert_passed();
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  /* Point every XDG data lookup at a private tree *before* glib caches the
   * values, so the scan sees only the entries written here. A test subprocess
   * inherits the environment and therefore the same tree. */
  char *data_dir;
  if (g_test_subprocess()) {
    data_dir = g_strdup(g_getenv("XDG_DATA_HOME"));
  } else {
    data_dir = g_dir_make_tmp("lambdock-xdg-XXXXXX", NULL);
    char *apps = g_build_filename(data_dir, "applications", NULL);
    g_assert_cmpint(g_mkdir_with_parents(apps, 0755), ==, 0);
    g_free(apps);

    g_setenv("XDG_DATA_HOME", data_dir, TRUE);
    g_setenv("XDG_DATA_DIRS", data_dir, TRUE);
    g_setenv("HOME", data_dir, TRUE);
  }
  g_assert_nonnull(data_dir);

  g_test_add_func("/desktop_entry/strip_field_codes", test_strip_field_codes);
  g_test_add_func("/desktop_entry/parse_valid", test_parse_valid);
  g_test_add_func("/desktop_entry/parse_rejects", test_parse_rejects);
  g_test_add_func("/desktop_entry/parse_flags",
                  test_parse_flags_and_icon_default);
  g_test_add_func("/desktop_entry/parse_localized", test_parse_localized_name);
  g_test_add_func("/desktop_entry/parse_file", test_parse_file);
  g_test_add_func("/desktop_entry/wm_class", test_parse_wm_class_and_stem);
  g_test_add_func("/desktop_entry/categories", test_parse_categories);
  g_test_add_func("/desktop_entry/app_id_to_name", test_app_id_to_name);
  g_test_add_data_func("/desktop_entry/scan", data_dir,
                       (GTestDataFunc)test_scan);
  g_test_add_data_func("/desktop_entry/scan_duplicates", data_dir,
                       (GTestDataFunc)test_scan_collapses_duplicate_applications);
  g_test_add_func("/desktop_entry/source_label", test_source_label);
  g_test_add_data_func("/desktop_entry/cache", data_dir,
                       (GTestDataFunc)test_cache);
  g_test_add_func("/desktop_entry/localized_names", test_localized_names);

  return g_test_run();
}
