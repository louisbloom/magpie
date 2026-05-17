/* mail-sync-progress-page.c - Centered ring + status + Cancel.
 *
 * The page is owned by mail-window and re-bound to a different
 * (sync, cancellable, account identity) every time a new pass starts.
 * Old signal handlers are disconnected on rebind.
 */

#include "config.h"

#include "mail-progress-ring.h"
#include "mail-sync-progress-page.h"

struct _MailSyncProgressPage
{
  AdwNavigationPage parent;

  MailProgressRing *ring;
  GtkLabel *heading;
  GtkLabel *status;
  GtkButton *cancel_button;

  MailSync *sync;            /* ref'd */
  GCancellable *cancellable; /* ref'd */
  gulong notify_progress_id;
  gulong notify_status_id;
};

G_DEFINE_FINAL_TYPE (MailSyncProgressPage, mail_sync_progress_page, ADW_TYPE_NAVIGATION_PAGE)

static void
update_progress (MailSyncProgressPage *self)
{
  if (self->sync != NULL)
    mail_progress_ring_set_fraction (self->ring, mail_sync_get_progress (self->sync));
}

static void
update_status (MailSyncProgressPage *self)
{
  if (self->sync != NULL)
    gtk_label_set_label (self->status, mail_sync_get_status (self->sync));
}

static void
on_progress_notify (GObject *src,
                    GParamSpec *pspec,
                    gpointer user_data)
{
  update_progress (user_data);
}

static void
on_status_notify (GObject *src,
                  GParamSpec *pspec,
                  gpointer user_data)
{
  update_status (user_data);
}

static void
on_cancel_clicked (GtkButton *btn,
                   gpointer user_data)
{
  MailSyncProgressPage *self = user_data;
  if (self->cancellable != NULL && !g_cancellable_is_cancelled (self->cancellable))
    g_cancellable_cancel (self->cancellable);
  gtk_widget_set_sensitive (GTK_WIDGET (btn), FALSE);
}

static void
disconnect_sync (MailSyncProgressPage *self)
{
  if (self->sync == NULL)
    return;
  if (self->notify_progress_id != 0)
    {
      g_signal_handler_disconnect (self->sync, self->notify_progress_id);
      self->notify_progress_id = 0;
    }
  if (self->notify_status_id != 0)
    {
      g_signal_handler_disconnect (self->sync, self->notify_status_id);
      self->notify_status_id = 0;
    }
  g_clear_object (&self->sync);
}

void
mail_sync_progress_page_set_state (MailSyncProgressPage *self,
                                   MailSync *sync,
                                   GCancellable *cancellable,
                                   const char *account_identity)
{
  g_return_if_fail (MAIL_IS_SYNC_PROGRESS_PAGE (self));

  disconnect_sync (self);
  g_clear_object (&self->cancellable);

  if (sync != NULL)
    {
      self->sync = g_object_ref (sync);
      self->notify_progress_id = g_signal_connect (sync, "notify::progress",
                                                   G_CALLBACK (on_progress_notify), self);
      self->notify_status_id = g_signal_connect (sync, "notify::status",
                                                 G_CALLBACK (on_status_notify), self);
      update_progress (self);
      update_status (self);
    }
  if (cancellable != NULL)
    self->cancellable = g_object_ref (cancellable);

  if (account_identity != NULL)
    {
      g_autofree char *text = g_strdup_printf ("Syncing %s", account_identity);
      gtk_label_set_label (self->heading, text);
    }
  else
    {
      gtk_label_set_label (self->heading, "Syncing");
    }

  gtk_widget_set_sensitive (GTK_WIDGET (self->cancel_button), self->cancellable != NULL);
}

GtkWidget *
mail_sync_progress_page_new (void)
{
  return g_object_new (MAIL_TYPE_SYNC_PROGRESS_PAGE, NULL);
}

static void
mail_sync_progress_page_dispose (GObject *object)
{
  MailSyncProgressPage *self = MAIL_SYNC_PROGRESS_PAGE (object);
  disconnect_sync (self);
  g_clear_object (&self->cancellable);
  G_OBJECT_CLASS (mail_sync_progress_page_parent_class)->dispose (object);
}

static void
mail_sync_progress_page_init (MailSyncProgressPage *self)
{
  adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self), "Syncing");
  adw_navigation_page_set_tag (ADW_NAVIGATION_PAGE (self), "sync-progress");

  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);

  self->ring = MAIL_PROGRESS_RING (mail_progress_ring_new ());
  mail_progress_ring_set_size (self->ring, 120);
  gtk_widget_set_halign (GTK_WIDGET (self->ring), GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->ring));

  self->heading = GTK_LABEL (gtk_label_new ("Syncing"));
  gtk_widget_add_css_class (GTK_WIDGET (self->heading), "title-2");
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->heading));

  self->status = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->status), "dim-label");
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->status));

  self->cancel_button = GTK_BUTTON (gtk_button_new_with_label ("Cancel"));
  gtk_widget_set_halign (GTK_WIDGET (self->cancel_button), GTK_ALIGN_CENTER);
  g_signal_connect (self->cancel_button, "clicked",
                    G_CALLBACK (on_cancel_clicked), self);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->cancel_button));

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), box);
  adw_navigation_page_set_child (ADW_NAVIGATION_PAGE (self), toolbar);
}

static void
mail_sync_progress_page_class_init (MailSyncProgressPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = mail_sync_progress_page_dispose;
}
