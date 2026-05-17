/* mail-refresh-button.c - Refresh ↔ progress ring ↔ done/error states. */

#include "config.h"

#include "mail-progress-ring.h"
#include "mail-refresh-button.h"

#define MORPH_MS 200
#define DONE_LINGER_MS 600

typedef enum
{
  STATE_IDLE,
  STATE_SYNCING,
  STATE_DONE,
  STATE_ERR,
} State;

struct _MailRefreshButton
{
  GtkButton parent;

  GtkStack *stack;
  GtkWidget *icon_idle;
  GtkWidget *icon_done;
  GtkWidget *icon_err;
  MailProgressRing *ring;

  MailSync *sync; /* ref'd; may be NULL */
  gulong notify_running_id;
  gulong notify_progress_id;

  State state;
  guint linger_source;
};

G_DEFINE_FINAL_TYPE (MailRefreshButton, mail_refresh_button, GTK_TYPE_BUTTON)

static void
set_state (MailRefreshButton *self,
           State state)
{
  if (self->state == state)
    return;
  self->state = state;
  switch (state)
    {
    case STATE_IDLE:
      gtk_stack_set_visible_child (self->stack, self->icon_idle);
      break;
    case STATE_SYNCING:
      gtk_stack_set_visible_child (self->stack, GTK_WIDGET (self->ring));
      break;
    case STATE_DONE:
      gtk_stack_set_visible_child (self->stack, self->icon_done);
      break;
    case STATE_ERR:
      gtk_stack_set_visible_child (self->stack, self->icon_err);
      break;
    }
}

static gboolean
linger_finished (gpointer p)
{
  MailRefreshButton *self = p;
  self->linger_source = 0;
  /* Reset the ring to 0 so the next sync starts from empty. */
  mail_progress_ring_set_fraction (self->ring, 0.0);
  set_state (self, STATE_IDLE);
  return G_SOURCE_REMOVE;
}

static void
schedule_linger (MailRefreshButton *self)
{
  if (self->linger_source != 0)
    g_source_remove (self->linger_source);
  self->linger_source = g_timeout_add (DONE_LINGER_MS, linger_finished, self);
}

static void
on_sync_progress (MailSync *sync,
                  GParamSpec *pspec,
                  gpointer user_data)
{
  MailRefreshButton *self = user_data;
  mail_progress_ring_set_fraction (self->ring, mail_sync_get_progress (sync));
}

static void
on_sync_running (MailSync *sync,
                 GParamSpec *pspec,
                 gpointer user_data)
{
  MailRefreshButton *self = user_data;
  gboolean running = mail_sync_is_running (sync);
  if (running)
    {
      if (self->linger_source != 0)
        {
          g_source_remove (self->linger_source);
          self->linger_source = 0;
        }
      mail_progress_ring_set_fraction (self->ring, 0.0);
      set_state (self, STATE_SYNCING);
    }
  else
    {
      /* Sync just finished. We can't tell success vs failure from
       * notify::running alone — fraction reaching 1.0 implies success;
       * otherwise treat as error. */
      double f = mail_sync_get_progress (sync);
      set_state (self, f >= 1.0 ? STATE_DONE : STATE_ERR);
      schedule_linger (self);
    }
}

static void
disconnect_sync (MailRefreshButton *self)
{
  if (self->sync == NULL)
    return;
  if (self->notify_progress_id != 0)
    {
      g_signal_handler_disconnect (self->sync, self->notify_progress_id);
      self->notify_progress_id = 0;
    }
  if (self->notify_running_id != 0)
    {
      g_signal_handler_disconnect (self->sync, self->notify_running_id);
      self->notify_running_id = 0;
    }
  g_clear_object (&self->sync);
}

void
mail_refresh_button_set_sync (MailRefreshButton *self,
                              MailSync *sync)
{
  g_return_if_fail (MAIL_IS_REFRESH_BUTTON (self));
  disconnect_sync (self);
  if (sync != NULL)
    {
      self->sync = g_object_ref (sync);
      self->notify_progress_id = g_signal_connect (sync, "notify::progress",
                                                   G_CALLBACK (on_sync_progress), self);
      self->notify_running_id = g_signal_connect (sync, "notify::running",
                                                  G_CALLBACK (on_sync_running), self);
      /* Sync the visual to wherever the sync currently is. */
      mail_progress_ring_set_fraction (self->ring, mail_sync_get_progress (sync));
      if (mail_sync_is_running (sync))
        set_state (self, STATE_SYNCING);
    }
  gtk_widget_set_sensitive (GTK_WIDGET (self), sync != NULL);
}

MailSync *
mail_refresh_button_get_sync (MailRefreshButton *self)
{
  g_return_val_if_fail (MAIL_IS_REFRESH_BUTTON (self), NULL);
  return self->sync;
}

GtkWidget *
mail_refresh_button_new (MailSync *sync)
{
  MailRefreshButton *self = g_object_new (MAIL_TYPE_REFRESH_BUTTON, NULL);
  if (sync != NULL)
    mail_refresh_button_set_sync (self, sync);
  return GTK_WIDGET (self);
}

static void
mail_refresh_button_dispose (GObject *object)
{
  MailRefreshButton *self = MAIL_REFRESH_BUTTON (object);
  if (self->linger_source != 0)
    {
      g_source_remove (self->linger_source);
      self->linger_source = 0;
    }
  disconnect_sync (self);
  G_OBJECT_CLASS (mail_refresh_button_parent_class)->dispose (object);
}

static void
mail_refresh_button_init (MailRefreshButton *self)
{
  gtk_button_set_has_frame (GTK_BUTTON (self), FALSE);
  gtk_widget_add_css_class (GTK_WIDGET (self), "flat");
  gtk_widget_add_css_class (GTK_WIDGET (self), "circular");

  self->stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_transition_type (self->stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration (self->stack, MORPH_MS);

  self->icon_idle = gtk_image_new_from_icon_name ("view-refresh-symbolic");
  self->icon_done = gtk_image_new_from_icon_name ("object-select-symbolic");
  self->icon_err = gtk_image_new_from_icon_name ("dialog-warning-symbolic");
  self->ring = MAIL_PROGRESS_RING (mail_progress_ring_new ());
  mail_progress_ring_set_size (self->ring, 18);

  gtk_stack_add_named (self->stack, self->icon_idle, "idle");
  gtk_stack_add_named (self->stack, GTK_WIDGET (self->ring), "syncing");
  gtk_stack_add_named (self->stack, self->icon_done, "done");
  gtk_stack_add_named (self->stack, self->icon_err, "err");
  gtk_stack_set_visible_child (self->stack, self->icon_idle);

  gtk_button_set_child (GTK_BUTTON (self), GTK_WIDGET (self->stack));
  gtk_widget_set_sensitive (GTK_WIDGET (self), FALSE); /* no sync bound yet */
  self->state = STATE_IDLE;
}

static void
mail_refresh_button_class_init (MailRefreshButtonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = mail_refresh_button_dispose;
}
