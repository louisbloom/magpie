/* mail-account-page.c - Centered ring + status + Cancel.
 *
 * The page is owned by mail-window and re-bound to a different
 * (sync, cancellable, account identity) on every account selection.
 * Old signal handlers are disconnected on rebind. Passing sync=NULL
 * to set_state renders the idle state (account selected, no active
 * sync): heading is just the identity, no ring, no cancel button.
 */

#include "config.h"

#include "mail-account-page.h"
#include "mail-eta.h"
#include "mail-progress-ring.h"

struct _MailAccountPage
{
  AdwNavigationPage parent;

  AdwWindowTitle *header_title; /* identity (title) + provider (subtitle) */
  AdwStatusPage *status_page;   /* HIG-shaped body container */
  MailProgressRing *ring;
  GtkLabel *eta_label;
  GtkButton *sync_button;   /* "Sync now" — visible when idle */
  GtkButton *cancel_button; /* "Cancel"   — visible when active */

  MailSync *sync;            /* ref'd */
  GCancellable *cancellable; /* ref'd */
  MailEta *eta;
  char *identity; /* g_strdup'd; the bound account's identity */
  gulong notify_progress_id;
  gulong notify_status_id;
  gulong notify_running_id;
};

enum
{
  SIGNAL_SYNC_REQUESTED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (MailAccountPage, mail_account_page, ADW_TYPE_NAVIGATION_PAGE)

static gboolean
sync_is_active (MailAccountPage *self)
{
  return self->sync != NULL && mail_sync_is_running (self->sync);
}

static void
apply_render_mode (MailAccountPage *self)
{
  /* The page has two visual modes, driven by whether the bound sync
   * is currently running. The sync indicator (ring + ETA + Cancel)
   * shows only in active; idle shows the Sync-now button. The
   * description (AdwStatusPage's main text) is driven by update_status
   * and reads the bound sync's :status in both modes (so the user can
   * still see the sync's final status text after the ring is hidden). */
  gboolean active = sync_is_active (self);

  gtk_widget_set_visible (GTK_WIDGET (self->ring), active);
  gtk_widget_set_visible (GTK_WIDGET (self->eta_label), active);
  gtk_widget_set_visible (GTK_WIDGET (self->cancel_button), active);
  gtk_widget_set_sensitive (GTK_WIDGET (self->cancel_button),
                            active && self->cancellable != NULL);
  gtk_widget_set_visible (GTK_WIDGET (self->sync_button), !active);
}

static void
update_progress (MailAccountPage *self)
{
  if (!sync_is_active (self))
    return;
  double fraction = mail_sync_get_progress (self->sync);
  mail_progress_ring_set_fraction (self->ring, fraction);

  if (self->eta != NULL)
    {
      /* Only feed PHASE_FETCH samples to the estimator. PHASE_FOLDERS
       * and PHASE_LISTS move the fraction up ~60× faster per second
       * than PHASE_FETCH does, so admitting them poisons the
       * sliding-window rate (the ETA reads "About 1 minute" while the
       * real remaining time is "About 30 minutes"). With the gate in
       * place, the label reads "Calculating…" through the first few
       * seconds of the pass and then converges to a stable FETCH-only
       * estimate after the first batch. */
      if (mail_sync_progress_is_in_fetch_phase (fraction))
        mail_eta_record (self->eta, g_get_monotonic_time (), fraction);
      double s = mail_eta_seconds_remaining (self->eta);
      g_autofree char *text = mail_eta_format (s);
      gtk_label_set_label (self->eta_label, text);
    }
}

static void
update_status (MailAccountPage *self)
{
  if (self->sync != NULL)
    adw_status_page_set_description (self->status_page,
                                     mail_sync_get_status (self->sync));
  else
    adw_status_page_set_description (self->status_page, "No sync in progress.");
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
on_running_notify (GObject *src,
                   GParamSpec *pspec,
                   gpointer user_data)
{
  /* The pass ended (or started) while we're showing this page. Flip
   * the heading/ring/cancel back to idle so the user isn't left with
   * a "Syncing X" header and a Cancel button after the work is done. */
  apply_render_mode (user_data);
}

static void
on_cancel_clicked (GtkButton *btn,
                   gpointer user_data)
{
  MailAccountPage *self = user_data;
  if (self->cancellable != NULL && !g_cancellable_is_cancelled (self->cancellable))
    g_cancellable_cancel (self->cancellable);
  gtk_widget_set_sensitive (GTK_WIDGET (btn), FALSE);
  /* Immediate feedback while cancellation propagates through the
   * sync engine — the in-flight HTTP request may take a moment to
   * unwind. finish_pass overwrites this with "Canceled." once the
   * pass actually ends. */
  adw_status_page_set_description (self->status_page, "Canceling…");
}

static void
on_sync_clicked (GtkButton *btn,
                 gpointer user_data)
{
  MailAccountPage *self = user_data;
  g_signal_emit (self, signals[SIGNAL_SYNC_REQUESTED], 0);
}

static void
disconnect_sync (MailAccountPage *self)
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
  if (self->notify_running_id != 0)
    {
      g_signal_handler_disconnect (self->sync, self->notify_running_id);
      self->notify_running_id = 0;
    }
  g_clear_object (&self->sync);
}

void
mail_account_page_set_state (MailAccountPage *self,
                             MailSync *sync,
                             GCancellable *cancellable,
                             const char *account_identity,
                             const char *account_provider)
{
  g_return_if_fail (MAIL_IS_ACCOUNT_PAGE (self));

  disconnect_sync (self);
  g_clear_object (&self->cancellable);

  /* Fresh sync ⇒ fresh ETA window. Sample history from a previous
   * pass would skew the rate at the start of the new one. */
  if (self->eta != NULL)
    mail_eta_reset (self->eta);
  gtk_label_set_label (self->eta_label, "Calculating…");

  if (sync != NULL)
    {
      self->sync = g_object_ref (sync);
      self->notify_progress_id = g_signal_connect (sync, "notify::progress",
                                                   G_CALLBACK (on_progress_notify), self);
      self->notify_status_id = g_signal_connect (sync, "notify::status",
                                                 G_CALLBACK (on_status_notify), self);
      self->notify_running_id = g_signal_connect (sync, "notify::running",
                                                  G_CALLBACK (on_running_notify), self);
    }
  if (cancellable != NULL)
    self->cancellable = g_object_ref (cancellable);

  g_free (self->identity);
  self->identity = g_strdup (account_identity != NULL ? account_identity : "");

  /* Persistent header bar: identity on top, provider below. Survives
   * the sync's running ↔ idle transitions; only set_state changes it. */
  const char *provider = account_provider != NULL ? account_provider : "";
  adw_window_title_set_title (self->header_title, self->identity);
  adw_window_title_set_subtitle (self->header_title, provider);
  /* Keep AdwNavigationPage::title in sync — it's what screen readers
   * and the collapsed-stack back-button label read. */
  adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self), self->identity);

  apply_render_mode (self);
  update_progress (self);
  update_status (self);
}

GtkWidget *
mail_account_page_new (void)
{
  return g_object_new (MAIL_TYPE_ACCOUNT_PAGE, NULL);
}

gboolean
_mail_account_page_is_cancel_visible_for_test (MailAccountPage *self)
{
  g_return_val_if_fail (MAIL_IS_ACCOUNT_PAGE (self), FALSE);
  return gtk_widget_get_visible (GTK_WIDGET (self->cancel_button));
}

gboolean
_mail_account_page_is_sync_button_visible_for_test (MailAccountPage *self)
{
  g_return_val_if_fail (MAIL_IS_ACCOUNT_PAGE (self), FALSE);
  return gtk_widget_get_visible (GTK_WIDGET (self->sync_button));
}

GtkButton *
_mail_account_page_get_sync_button_for_test (MailAccountPage *self)
{
  g_return_val_if_fail (MAIL_IS_ACCOUNT_PAGE (self), NULL);
  return self->sync_button;
}

const char *
_mail_account_page_get_subtitle_for_test (MailAccountPage *self)
{
  g_return_val_if_fail (MAIL_IS_ACCOUNT_PAGE (self), NULL);
  return adw_window_title_get_subtitle (self->header_title);
}

const char *
_mail_account_page_get_description_for_test (MailAccountPage *self)
{
  g_return_val_if_fail (MAIL_IS_ACCOUNT_PAGE (self), NULL);
  return adw_status_page_get_description (self->status_page);
}

static void
mail_account_page_dispose (GObject *object)
{
  MailAccountPage *self = MAIL_ACCOUNT_PAGE (object);
  disconnect_sync (self);
  g_clear_object (&self->cancellable);
  g_clear_pointer (&self->eta, mail_eta_free);
  g_clear_pointer (&self->identity, g_free);
  G_OBJECT_CLASS (mail_account_page_parent_class)->dispose (object);
}

static void
mail_account_page_init (MailAccountPage *self)
{
  adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self), "Account");
  adw_navigation_page_set_tag (ADW_NAVIGATION_PAGE (self), "account");

  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  /* Two-line title widget — identity on top, provider name below.
   * Both are populated by mail_account_page_set_state. */
  self->header_title = ADW_WINDOW_TITLE (adw_window_title_new ("", ""));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header),
                                   GTK_WIDGET (self->header_title));
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  /* HIG: AdwStatusPage provides the centered/clamped layout pattern
   * for full-pane "what's going on here" displays. We set description
   * (live sync status) and child (the dynamic content) per state; the
   * title is left unset because the header bar already identifies the
   * account, and the icon-name is unset because the progress ring
   * stands in for it when active. */
  self->status_page = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_description (self->status_page, "");

  GtkWidget *child = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_halign (child, GTK_ALIGN_CENTER);

  self->ring = MAIL_PROGRESS_RING (mail_progress_ring_new ());
  mail_progress_ring_set_size (self->ring, 120);
  gtk_widget_set_halign (GTK_WIDGET (self->ring), GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (child), GTK_WIDGET (self->ring));

  self->eta_label = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->eta_label), "dim-label");
  gtk_widget_add_css_class (GTK_WIDGET (self->eta_label), "caption");
  gtk_widget_set_visible (GTK_WIDGET (self->eta_label), FALSE);
  gtk_box_append (GTK_BOX (child), GTK_WIDGET (self->eta_label));

  /* 120-second sliding window. The sync engine emits one
   * notify::progress per IMAP fetch batch (50 messages), so the ETA
   * receives one sample per batch wall-time — typically 1–15 s on
   * Gmail, occasionally more. A 10 s window made drop_stale evict
   * every prior sample whenever a batch exceeded the window, leaving
   * the ring with the lone surviving sample plus the new one and
   * computing the rate from a single batch's slope — fast and slow
   * batches differ by ~4×, so the ETA flipped between buckets
   * batch-to-batch. 120 s comfortably exceeds the observed worst-
   * case batch wall-time, holds 5–60 samples at typical cadence, and
   * still reacts to genuine speed changes inside the "About N
   * minutes" formatter's granularity. */
  self->eta = mail_eta_new (120 * G_USEC_PER_SEC);

  /* Two mutually-exclusive action buttons live in the same slot;
   * apply_render_mode toggles their visibility based on sync state.
   * "Sync now" gets the suggested-action style — HIG's placeholder-
   * page guidance explicitly recommends suggested for the primary
   * action on a page like this. */
  self->sync_button = GTK_BUTTON (gtk_button_new_with_label ("Sync now"));
  gtk_widget_set_halign (GTK_WIDGET (self->sync_button), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (self->sync_button), "suggested-action");
  gtk_widget_add_css_class (GTK_WIDGET (self->sync_button), "pill");
  g_signal_connect (self->sync_button, "clicked",
                    G_CALLBACK (on_sync_clicked), self);
  gtk_box_append (GTK_BOX (child), GTK_WIDGET (self->sync_button));

  self->cancel_button = GTK_BUTTON (gtk_button_new_with_label ("Cancel"));
  gtk_widget_set_halign (GTK_WIDGET (self->cancel_button), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (self->cancel_button), "pill");
  g_signal_connect (self->cancel_button, "clicked",
                    G_CALLBACK (on_cancel_clicked), self);
  gtk_box_append (GTK_BOX (child), GTK_WIDGET (self->cancel_button));

  adw_status_page_set_child (self->status_page, child);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar),
                                GTK_WIDGET (self->status_page));
  adw_navigation_page_set_child (ADW_NAVIGATION_PAGE (self), toolbar);
}

static void
mail_account_page_class_init (MailAccountPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = mail_account_page_dispose;

  signals[SIGNAL_SYNC_REQUESTED] = g_signal_new ("sync-requested",
                                                 G_TYPE_FROM_CLASS (klass),
                                                 G_SIGNAL_RUN_LAST,
                                                 0, NULL, NULL, NULL,
                                                 G_TYPE_NONE, 0);
}
