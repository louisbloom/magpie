#include "mail-about.h"

#include "magpie-version.h"

#include <adwaita.h>

void
mail_about_present (GtkWindow *parent)
{
  AdwDialog *dialog = adw_about_dialog_new ();
  AdwAboutDialog *about = ADW_ABOUT_DIALOG (dialog);

  adw_about_dialog_set_application_name (about, "Magpie");
  adw_about_dialog_set_application_icon (about, "org.gnome.Magpie");
  adw_about_dialog_set_developer_name (about, "Thomas Christensen");
  adw_about_dialog_set_version (about, MAGPIE_VERSION);
  adw_about_dialog_set_copyright (about, "© 2026 Thomas Christensen");
  adw_about_dialog_set_license_type (about, GTK_LICENSE_MIT_X11);
  adw_about_dialog_set_issue_url (
      about, "https://gitlab.gnome.org/thomasc1971/magpie/-/issues");

  adw_dialog_present (dialog, GTK_WIDGET (parent));
}
