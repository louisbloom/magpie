/* tests/test-about.c - Source-level tripwire for the About-dialog wiring.
 *
 * Reading src/mail-application.c as text is simpler than instantiating
 * MailApplication just to inspect its action map — the application
 * type transitively pulls in MailWindow and the rest of the widget
 * graph. The strings pinned below are the load-bearing pieces: if any
 * of them disappears, the Ctrl+Shift+A shortcut no longer reaches
 * mail_about_present() and the About dialog becomes unreachable
 * (currently there is no menu entry; the shortcut is the only way in).
 *
 * Same approach as tests/test-window.c's XML pins.
 */

#include <glib.h>
#include <string.h>

static char *
load_application_c (void)
{
  g_autofree char *path = g_build_filename (MAGPIE_TOP_SRCDIR, "src", "mail-application.c", NULL);
  char *contents = NULL;
  g_autoptr (GError) error = NULL;
  g_assert_true (g_file_get_contents (path, &contents, NULL, &error));
  g_assert_no_error (error);
  g_assert_nonnull (contents);
  return contents;
}

static void
test_about_action_registered (void)
{
  g_autofree char *src = load_application_c ();
  g_assert_nonnull (strstr (src, ".name = \"about\""));
  g_assert_nonnull (strstr (src, "mail_about_present"));
}

static void
test_about_accel_bound (void)
{
  g_autofree char *src = load_application_c ();
  g_assert_nonnull (strstr (src, "\"app.about\""));
  g_assert_nonnull (strstr (src, "\"<Ctrl><Shift>a\""));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/about/action-registered", test_about_action_registered);
  g_test_add_func ("/about/accel-bound", test_about_accel_bound);
  return g_test_run ();
}
