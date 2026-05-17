/* mail-message-list.c - Message list pane.
 *
 * Backed by a GtkListBox with .navigation-sidebar styling, bound to a
 * GListStore of MailMessageRowItem GObjects. Each row item holds a
 * borrowed `const MailMessageMeta*` that points into the backend's
 * arena; the items are valid until the next load on the same backend
 * (per [[feedback-memory-reuse]]).
 *
 * States are surfaced through a GtkStack: "empty" (no folder
 * selected), "loading" (spinner), and "list" (messages).
 */

#include "config.h"

#include "mail-message-list.h"

#include <adwaita.h>
#include <string.h>

#define MAIL_TYPE_MESSAGE_ROW_ITEM (mail_message_row_item_get_type ())
G_DECLARE_FINAL_TYPE (MailMessageRowItem, mail_message_row_item, MAIL, MESSAGE_ROW_ITEM, GObject)

struct _MailMessageRowItem
{
  GObject parent_instance;

  const MailMessageMeta *meta; /* arena-borrowed; valid for the life of this row */
};

G_DEFINE_FINAL_TYPE (MailMessageRowItem, mail_message_row_item, G_TYPE_OBJECT)

static void
mail_message_row_item_class_init (MailMessageRowItemClass *klass)
{
}

static void
mail_message_row_item_init (MailMessageRowItem *self)
{
}

static MailMessageRowItem *
mail_message_row_item_new (const MailMessageMeta *meta)
{
  MailMessageRowItem *self = g_object_new (MAIL_TYPE_MESSAGE_ROW_ITEM, NULL);
  self->meta = meta;
  return self;
}

/* --------------------------------------------------------------- */

enum
{
  SIGNAL_MESSAGE_ACTIVATED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

struct _MailMessageList
{
  GtkWidget parent_instance;

  GtkStack *stack;
  GtkListBox *list_box;
  GListStore *store;

  /* Current in-flight load. We hold an owned ref to the backend's
   * GoaObject-equivalent? No — we just hold a borrowed pointer to the
   * backend, plus a cancellable we can cancel on subsequent loads. */
  MailBackend *current_backend; /* borrowed */
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (MailMessageList, mail_message_list, GTK_TYPE_WIDGET)

static char *
format_received (gint64 received_unix)
{
  if (received_unix <= 0)
    return g_strdup ("");
  g_autoptr (GDateTime) when = g_date_time_new_from_unix_local (received_unix);
  if (when == NULL)
    return g_strdup ("");
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  int same_day = (g_date_time_get_year (when) == g_date_time_get_year (now) && g_date_time_get_day_of_year (when) == g_date_time_get_day_of_year (now));
  return same_day ? g_date_time_format (when, "%H:%M") : g_date_time_format (when, "%b %e");
}

static GtkWidget *
build_row_widget (gpointer item,
                  gpointer user_data)
{
  MailMessageRowItem *row_item = MAIL_MESSAGE_ROW_ITEM (item);
  const MailMessageMeta *m = row_item->meta;

  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());

  /* AdwActionRow renders title and subtitle as Pango markup by default,
   * so a raw subject like "X & Y" or a "Name <addr@host>" subtitle
   * triggers markup-parse warnings (and produces garbled text). Mail
   * subjects and addresses are arbitrary user text; treat them as
   * literal characters. */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);

  /* AdwActionRow defaults to activatable=FALSE unless an activatable-
   * widget is set. Same fix as the sidebar's folder rows: opt in
   * explicitly so GtkListBox::row-activated fires on click. */
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);

  const char *subject = (m->subject != NULL && m->subject[0] != '\0') ? m->subject : "(no subject)";
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), subject);

  g_autofree char *date_str = format_received (m->received_unix);
  g_autofree char *subtitle = NULL;
  if (m->from != NULL && date_str[0] != '\0')
    subtitle = g_strdup_printf ("%s · %s", m->from, date_str);
  else if (m->from != NULL)
    subtitle = g_strdup (m->from);
  else
    subtitle = g_strdup (date_str);
  adw_action_row_set_subtitle (row, subtitle);

  if (m->unread)
    gtk_widget_add_css_class (GTK_WIDGET (row), "heading");

  return GTK_WIDGET (row);
}

static void
on_row_activated (GtkListBox *list_box,
                  GtkListBoxRow *row,
                  gpointer user_data)
{
  MailMessageList *self = MAIL_MESSAGE_LIST (user_data);
  guint index = gtk_list_box_row_get_index (row);
  g_autoptr (MailMessageRowItem) item = g_list_model_get_item (G_LIST_MODEL (self->store), index);
  if (item == NULL)
    return;
  g_signal_emit (self, signals[SIGNAL_MESSAGE_ACTIVATED], 0,
                 self->current_backend, item->meta->id, item->meta->subject);
}

typedef struct
{
  MailMessageList *self; /* ref'd */
  MailBackend *backend;  /* borrowed */
} LoadMessagesCtx;

static void
on_messages_loaded (GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
  LoadMessagesCtx *ctx = user_data;
  MailMessageList *self = ctx->self;
  g_autoptr (GError) error = NULL;

  GPtrArray *messages = mail_backend_list_messages_finish (ctx->backend, result, &error);
  if (messages == NULL)
    {
      if (error == NULL || !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("list_messages failed: %s", error != NULL ? error->message : "(no error)");
          gtk_stack_set_visible_child_name (self->stack, "empty");
        }
      g_object_unref (self);
      g_free (ctx);
      return;
    }

  g_list_store_remove_all (self->store);
  for (guint i = 0; i < messages->len; i++)
    {
      const MailMessageMeta *m = g_ptr_array_index (messages, i);
      MailMessageRowItem *ri = mail_message_row_item_new (m);
      g_list_store_append (self->store, ri);
      g_object_unref (ri);
    }
  gtk_stack_set_visible_child_name (self->stack, messages->len > 0 ? "list" : "empty");

  g_object_unref (self);
  g_free (ctx);
}

void
mail_message_list_load (MailMessageList *self,
                        MailBackend *backend,
                        const char *folder_id,
                        int top_n)
{
  g_return_if_fail (MAIL_IS_MESSAGE_LIST (self));
  g_return_if_fail (backend != NULL);
  g_return_if_fail (folder_id != NULL);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  self->current_backend = backend;
  g_list_store_remove_all (self->store);
  gtk_stack_set_visible_child_name (self->stack, "loading");

  LoadMessagesCtx *ctx = g_new (LoadMessagesCtx, 1);
  ctx->self = g_object_ref (self);
  ctx->backend = backend;
  mail_backend_list_messages_async (backend, folder_id, top_n,
                                    self->cancellable,
                                    on_messages_loaded, ctx);
}

GtkWidget *
mail_message_list_new (void)
{
  return g_object_new (MAIL_TYPE_MESSAGE_LIST, NULL);
}

GtkListBox *
_mail_message_list_get_list_box_for_test (MailMessageList *self)
{
  g_return_val_if_fail (MAIL_IS_MESSAGE_LIST (self), NULL);
  return self->list_box;
}

static void
mail_message_list_dispose (GObject *object)
{
  MailMessageList *self = MAIL_MESSAGE_LIST (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->store);

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))) != NULL)
    gtk_widget_unparent (child);

  G_OBJECT_CLASS (mail_message_list_parent_class)->dispose (object);
}

static void
mail_message_list_class_init (MailMessageListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = mail_message_list_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);

  signals[SIGNAL_MESSAGE_ACTIVATED] = g_signal_new ("message-activated",
                                                    G_TYPE_FROM_CLASS (klass),
                                                    G_SIGNAL_RUN_LAST,
                                                    0,
                                                    NULL, NULL, NULL,
                                                    G_TYPE_NONE,
                                                    3,
                                                    G_TYPE_POINTER,
                                                    G_TYPE_STRING,
                                                    G_TYPE_STRING);
}

static void
mail_message_list_init (MailMessageList *self)
{
  self->store = g_list_store_new (MAIL_TYPE_MESSAGE_ROW_ITEM);
  self->cancellable = g_cancellable_new ();

  self->stack = GTK_STACK (gtk_stack_new ());
  gtk_widget_set_parent (GTK_WIDGET (self->stack), GTK_WIDGET (self));

  /* "empty" */
  AdwStatusPage *empty = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_icon_name (empty, "mail-unread-symbolic");
  adw_status_page_set_title (empty, "Select a folder");
  adw_status_page_set_description (empty, "Choose an account and folder in the sidebar.");
  gtk_stack_add_named (self->stack, GTK_WIDGET (empty), "empty");

  /* "loading" — kept deliberately simple. An earlier version embedded
   * an AdwSpinner via adw_status_page_set_child plus set_paintable(NULL);
   * that combination kept queuing snapshots while the stack page was
   * unmapped and produced
   *   Gtk-WARNING: Trying to snapshot GtkGizmo without a current allocation
   * on every redraw cycle. A static title is enough — the load is
   * usually under half a second. */
  AdwStatusPage *loading = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_title (loading, "Loading messages…");
  gtk_stack_add_named (self->stack, GTK_WIDGET (loading), "loading");

  /* "list" */
  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  self->list_box = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_widget_add_css_class (GTK_WIDGET (self->list_box), "navigation-sidebar");
  gtk_list_box_bind_model (self->list_box, G_LIST_MODEL (self->store),
                           build_row_widget, self, NULL);
  g_signal_connect (self->list_box, "row-activated",
                    G_CALLBACK (on_row_activated), self);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), GTK_WIDGET (self->list_box));
  gtk_stack_add_named (self->stack, scroller, "list");

  gtk_stack_set_visible_child_name (self->stack, "empty");
}
