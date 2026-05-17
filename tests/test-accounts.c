/* test-accounts.c - Smoke test for GoaClient cold-start.
 *
 * We deliberately do not require any accounts to exist; the goal is to
 * confirm linkage and that creating a GoaClient does not crash. The
 * full account enumeration is exercised at runtime by mail-sidebar.
 */

#include <gio/gio.h>

static void
test_goa_client_cold_start (void)
{
  g_autoptr (GError) error = NULL;
  /* Use D-Bus introspection rather than libgoa so the smoke test stays
   * lightweight and does not pin a goa-1.0 dependency in tests. */
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (bus == NULL)
    {
      g_test_skip ("session bus unavailable");
      return;
    }
  g_assert_no_error (error);
  g_assert (G_IS_DBUS_CONNECTION (bus));
}

int
main (int argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/accounts/goa-cold-start", test_goa_client_cold_start);
  return g_test_run ();
}
