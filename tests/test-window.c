/* tests/test-window.c - Structural pin for the window's GtkBuilder XML.
 *
 * The main thing this catches is regressions in
 * data/ui/window.ui's content-pane navigation pages — every page that
 * can sit on top of nav_view (message-list, message-view) must carry
 * its own sidebar toggle, since AdwOverlaySplitView + the nested
 * AdwNavigationView only ever displays the topmost page's header bar.
 * The account-page toggle is constructed in C, not XML, and is pinned
 * by tests/test-account-page.c::test_sidebar_toggle_is_present.
 *
 * The test reads window.ui as raw XML rather than instantiating a
 * MailWindow: spinning up the window template needs the compiled
 * GResource bundle and an AdwApplication, neither of which the rest
 * of the test suite depends on. A file-level pin is enough — if a
 * future refactor accidentally drops one of the toggle ids, the
 * template-child binding in mail-window.c silently leaves a NULL
 * pointer and the bind_property call crashes only at runtime; this
 * test fails at `make check` instead.
 */

#include <glib.h>
#include <string.h>

static char *
load_window_ui (void)
{
  /* MAGPIE_TOP_SRCDIR is injected at compile time from automake's
   * $(top_srcdir), so the test works regardless of where it's invoked
   * from (make check, ./build/tests/test-window, gdb, etc.). */
  g_autofree char *path = g_build_filename (MAGPIE_TOP_SRCDIR,
                                            "data", "ui", "window.ui", NULL);
  char *contents = NULL;
  g_autoptr (GError) error = NULL;
  g_assert_true (g_file_get_contents (path, &contents, NULL, &error));
  g_assert_no_error (error);
  g_assert_nonnull (contents);
  return contents;
}

static void
test_message_list_has_sidebar_toggle (void)
{
  g_autofree char *xml = load_window_ui ();
  g_assert_nonnull (strstr (xml, "id=\"sidebar_toggle\""));
}

/* Regression: when AdwNavigationView pushed message-view over message-
 * list, the sidebar toggle that lives on message-list's header bar
 * was no longer reachable. The fix is a second toggle on the message-
 * view header bar; this pins that it stays there. */
static void
test_message_view_has_sidebar_toggle (void)
{
  g_autofree char *xml = load_window_ui ();
  g_assert_nonnull (strstr (xml, "id=\"message_view_sidebar_toggle\""));
}

/* Belt-and-braces: every sidebar-toggle button in the XML uses the
 * same state-reflective icon, so the three header bars (message-list,
 * message-view, account-page) look identical and the GtkToggleButton's
 * pressed state communicates the current sidebar visibility. The
 * count is exactly two (the account-page toggle is built in C). */
static void
test_sidebar_toggle_icon_consistency (void)
{
  g_autofree char *xml = load_window_ui ();
  const char *needle = "<property name=\"icon-name\">sidebar-show-symbolic</property>";
  guint count = 0;
  const char *p = xml;
  while ((p = strstr (p, needle)) != NULL)
    {
      count++;
      p += strlen (needle);
    }
  g_assert_cmpuint (count, ==, 2);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/window/message-list-has-sidebar-toggle",
                   test_message_list_has_sidebar_toggle);
  g_test_add_func ("/window/message-view-has-sidebar-toggle",
                   test_message_view_has_sidebar_toggle);
  g_test_add_func ("/window/sidebar-toggle-icon-consistency",
                   test_sidebar_toggle_icon_consistency);
  return g_test_run ();
}
