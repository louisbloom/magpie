/* mail-message-list.c - Message list pane.
 *
 * Backed by a GtkListView fed from a GListStore of MailMessageRowItem
 * GObjects. GtkListView virtualises: only the rows in the visible
 * viewport (plus a small scroll buffer) are ever constructed, so the
 * model can grow to tens of thousands of items without paying a
 * widget-per-row cost. Each row item holds a borrowed
 * `const MailMessageMeta*` that points into the backend's arena and
 * is valid until the next load on the same backend
 * (per [[feedback-memory-reuse]]).
 *
 * States are surfaced through a GtkStack: "empty" (no folder selected),
 * "loading" (transient), "folder-empty" (folder selected, zero rows),
 * and "list" (populated).
 */

#include "config.h"

#include "mail-message-list.h"

#include <adwaita.h>

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
  GtkScrolledWindow *scroller; /* borrowed; lives inside stack as the "list" page */
  GtkListView *list_view;
  GListStore *store;
  GtkNoSelection *selection;

  /* Current in-flight load. We hold an owned ref to the backend's
   * GoaObject-equivalent? No — we just hold a borrowed pointer to the
   * backend, plus a cancellable we can cancel on subsequent loads. */
  MailBackend *current_backend; /* borrowed */
  GCancellable *cancellable;

  /* Subscription to current_backend's change signal, plus the loaded
   * folder id so the handler can ignore events for other folders. The
   * listener is rebound on every load and dropped in dispose. */
  guint backend_listener_id;
  char *current_folder_id; /* g_strdup; tracks the most recently loaded folder */
};

G_DEFINE_FINAL_TYPE (MailMessageList, mail_message_list, GTK_TYPE_WIDGET)

/* "now" is taken as a parameter so the formatter is deterministic
 * under test. Three branches: same day → HH:MM, same year (older
 * than today) → "Mon  D", different year → "Mon  D, YYYY". The last
 * branch is what disambiguates a message from Feb 2025 from one
 * from Feb 2026 once the year rolls over. */
static char *
format_received_at (gint64 received_unix,
                    GDateTime *now)
{
  if (received_unix <= 0)
    return g_strdup ("");
  g_autoptr (GDateTime) when = g_date_time_new_from_unix_local (received_unix);
  if (when == NULL)
    return g_strdup ("");
  int when_year = g_date_time_get_year (when);
  int now_year = g_date_time_get_year (now);
  if (when_year == now_year && g_date_time_get_day_of_year (when) == g_date_time_get_day_of_year (now))
    return g_date_time_format (when, "%H:%M");
  if (when_year == now_year)
    return g_date_time_format (when, "%b %e");
  return g_date_time_format (when, "%b %e, %Y");
}

static char *
format_received (gint64 received_unix)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  return format_received_at (received_unix, now);
}

char *
_mail_message_list_format_received_for_test (gint64 received_unix,
                                             GDateTime *now)
{
  return format_received_at (received_unix, now);
}

/* --- factory: setup/bind/unbind for recyclable row widgets ----- */

/* AdwActionRow is a GtkListBoxRow subclass and asserts its parent is a
 * GtkListBox when grabbing focus — fatal under GtkListView. So we hand-
 * roll a row widget: an hbox containing a vbox of [title, subtitle] and
 * a trailing badge label. Subjects/froms are set as plain text via
 * gtk_label_set_text (NOT set_markup), which handles '<','>','&','@'. */
static void
factory_setup (GtkSignalListItemFactory *factory,
               GtkListItem *list_item,
               gpointer user_data)
{
  GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top (hbox, 6);
  gtk_widget_set_margin_bottom (hbox, 6);
  gtk_widget_set_margin_start (hbox, 12);
  gtk_widget_set_margin_end (hbox, 12);

  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (vbox, TRUE);

  GtkWidget *title = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (title), TRUE);
  gtk_box_append (GTK_BOX (vbox), title);

  GtkWidget *subtitle = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (subtitle), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (subtitle), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (subtitle), TRUE);
  gtk_widget_add_css_class (subtitle, "caption");
  gtk_widget_add_css_class (subtitle, "dim-label");
  gtk_box_append (GTK_BOX (vbox), subtitle);

  gtk_box_append (GTK_BOX (hbox), vbox);

  GtkWidget *badge = gtk_label_new (NULL);
  gtk_widget_add_css_class (badge, "numeric");
  gtk_widget_add_css_class (badge, "caption");
  gtk_widget_add_css_class (badge, "dim-label");
  gtk_widget_set_visible (badge, FALSE);
  gtk_box_append (GTK_BOX (hbox), badge);

  g_object_set_data (G_OBJECT (hbox), "mail-title", title);
  g_object_set_data (G_OBJECT (hbox), "mail-subtitle", subtitle);
  g_object_set_data (G_OBJECT (hbox), "mail-badge", badge);

  gtk_list_item_set_child (list_item, hbox);
}

static void
factory_bind (GtkSignalListItemFactory *factory,
              GtkListItem *list_item,
              gpointer user_data)
{
  GtkWidget *row = gtk_list_item_get_child (list_item);
  MailMessageRowItem *item = MAIL_MESSAGE_ROW_ITEM (gtk_list_item_get_item (list_item));
  const MailMessageMeta *m = item->meta;

  GtkLabel *title = g_object_get_data (G_OBJECT (row), "mail-title");
  GtkLabel *subtitle = g_object_get_data (G_OBJECT (row), "mail-subtitle");
  GtkWidget *badge = g_object_get_data (G_OBJECT (row), "mail-badge");

  const char *subject = (m->subject != NULL && m->subject[0] != '\0') ? m->subject : "(no subject)";
  gtk_label_set_text (title, subject);

  g_autofree char *date_str = format_received (m->received_unix);
  g_autofree char *subtitle_str = NULL;
  if (m->from != NULL && date_str[0] != '\0')
    subtitle_str = g_strdup_printf ("%s · %s", m->from, date_str);
  else if (m->from != NULL)
    subtitle_str = g_strdup (m->from);
  else
    subtitle_str = g_strdup (date_str);
  gtk_label_set_text (subtitle, subtitle_str);

  /* unread is a boolean; use bold "heading" style on the title as the
   * indicator. The badge slot stays wired for a future per-row value. */
  gtk_widget_set_visible (badge, FALSE);

  if (m->unread)
    gtk_widget_add_css_class (GTK_WIDGET (title), "heading");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (title), "heading");
}

static void
factory_unbind (GtkSignalListItemFactory *factory,
                GtkListItem *list_item,
                gpointer user_data)
{
  GtkWidget *row = gtk_list_item_get_child (list_item);
  GtkWidget *title = g_object_get_data (G_OBJECT (row), "mail-title");
  gtk_widget_remove_css_class (title, "heading");
}

/* --- activation ------------------------------------------------- */

static void
on_list_view_activate (GtkListView *list_view,
                       guint position,
                       gpointer user_data)
{
  MailMessageList *self = MAIL_MESSAGE_LIST (user_data);
  g_autoptr (MailMessageRowItem) item = g_list_model_get_item (G_LIST_MODEL (self->store), position);
  if (item == NULL)
    return;
  g_signal_emit (self, signals[SIGNAL_MESSAGE_ACTIVATED], 0,
                 self->current_backend, item->meta->id, item->meta->subject);
}

/* Find the row for @message_id and force a rebind with the new unread
 * bit. Borrowed arena memory is mutated in place — see the const-vs-
 * enforcement note inline. */
static void
apply_unread_bit (MailMessageList *self,
                  const char *message_id,
                  gboolean unread)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->store));
  for (guint i = 0; i < n; i++)
    {
      g_autoptr (MailMessageRowItem) item = g_list_model_get_item (G_LIST_MODEL (self->store), i);
      if (item == NULL || item->meta == NULL || item->meta->id == NULL)
        continue;
      if (g_strcmp0 (item->meta->id, message_id) != 0)
        continue;

      if (item->meta->unread == unread)
        return; /* nothing to refresh */

      /* The MailMessageMeta is borrowed from the backend's arena. The
       * `const` on the row item's pointer is borrow discipline (so
       * factory_bind treats it as read-only), not enforcement — the
       * arena memory is plain malloc. Mutating the unread bit here
       * lets the next bind pick up the new value; emitting
       * items-changed on the row drives GtkListView to rebind it. */
      MailMessageMeta *meta = (MailMessageMeta *) item->meta;
      meta->unread = unread;
      g_list_model_items_changed (G_LIST_MODEL (self->store), i, 1, 1);
      return;
    }
}

void
mail_message_list_mark_read (MailMessageList *self,
                             const char *message_id)
{
  g_return_if_fail (MAIL_IS_MESSAGE_LIST (self));
  if (message_id == NULL)
    return;
  apply_unread_bit (self, message_id, FALSE);
}

/* MailBackendChange handler. Updates the row that matches the event,
 * but only when the event names the currently loaded folder — events
 * for sibling folders are no-ops. The local-read path also calls
 * mail_message_list_mark_read synchronously (mail-window.c:168) which
 * is idempotent given apply_unread_bit's "nothing to refresh" guard. */
static void
on_backend_change (MailBackend *backend,
                   const MailBackendChange *change,
                   gpointer user_data)
{
  (void) backend;
  MailMessageList *self = user_data;
  if (change->kind != MAIL_BACKEND_CHANGE_MESSAGE_FLAGS)
    return;
  if (change->folder_id == NULL || change->message_id == NULL)
    return;
  if (g_strcmp0 (change->folder_id, self->current_folder_id) != 0)
    return;
  apply_unread_bit (self, change->message_id, change->unread);
}

/* --- load -------------------------------------------------------- */

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
  gtk_stack_set_visible_child_name (self->stack,
                                    messages->len > 0 ? "list" : "folder-empty");

  g_object_unref (self);
  g_free (ctx);
}

void
mail_message_list_load (MailMessageList *self,
                        MailBackend *backend,
                        const char *folder_id)
{
  g_return_if_fail (MAIL_IS_MESSAGE_LIST (self));
  g_return_if_fail (backend != NULL);
  g_return_if_fail (folder_id != NULL);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  /* Rebind the backend change subscription if the backend changed.
   * The single subscription covers every loaded folder — the handler
   * filters by current_folder_id so events for other folders are
   * dropped on the floor. */
  if (backend != self->current_backend)
    {
      if (self->backend_listener_id != 0 && self->current_backend != NULL)
        mail_backend_remove_listener (self->current_backend, self->backend_listener_id);
      self->backend_listener_id = mail_backend_add_listener (backend, on_backend_change, self, NULL);
    }
  self->current_backend = backend;

  g_free (self->current_folder_id);
  self->current_folder_id = g_strdup (folder_id);

  g_list_store_remove_all (self->store);
  gtk_stack_set_visible_child_name (self->stack, "loading");

  LoadMessagesCtx *ctx = g_new (LoadMessagesCtx, 1);
  ctx->self = g_object_ref (self);
  ctx->backend = backend;
  /* G_MAXINT → SQL LIMIT effectively unbounded; the messages_folder_
   * received index keeps cost O(rows_returned). GtkListView's
   * virtualisation absorbs any model size. */
  mail_backend_list_messages_async (backend, folder_id, G_MAXINT,
                                    self->cancellable,
                                    on_messages_loaded, ctx);
}

GtkWidget *
mail_message_list_new (void)
{
  return g_object_new (MAIL_TYPE_MESSAGE_LIST, NULL);
}

GtkListView *
_mail_message_list_get_list_view_for_test (MailMessageList *self)
{
  g_return_val_if_fail (MAIL_IS_MESSAGE_LIST (self), NULL);
  return self->list_view;
}

GListModel *
_mail_message_list_get_model_for_test (MailMessageList *self)
{
  g_return_val_if_fail (MAIL_IS_MESSAGE_LIST (self), NULL);
  return G_LIST_MODEL (self->store);
}

const MailMessageMeta *
_mail_message_list_get_meta_for_test (MailMessageList *self,
                                      guint index)
{
  g_return_val_if_fail (MAIL_IS_MESSAGE_LIST (self), NULL);
  g_autoptr (MailMessageRowItem) item = g_list_model_get_item (G_LIST_MODEL (self->store), index);
  return item != NULL ? item->meta : NULL;
}

GtkStack *
_mail_message_list_get_stack_for_test (MailMessageList *self)
{
  g_return_val_if_fail (MAIL_IS_MESSAGE_LIST (self), NULL);
  return self->stack;
}

static void
mail_message_list_dispose (GObject *object)
{
  MailMessageList *self = MAIL_MESSAGE_LIST (object);

  if (self->backend_listener_id != 0 && self->current_backend != NULL)
    {
      mail_backend_remove_listener (self->current_backend, self->backend_listener_id);
      self->backend_listener_id = 0;
    }
  self->current_backend = NULL;
  g_clear_pointer (&self->current_folder_id, g_free);

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

  /* "loading" — kept deliberately simple. A static title is enough; the
   * load is usually under half a second. (Earlier AdwSpinner experiments
   * tripped Gtk-WARNING snapshots; see git log if needed.) */
  AdwStatusPage *loading = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_title (loading, "Loading messages…");
  gtk_stack_add_named (self->stack, GTK_WIDGET (loading), "loading");

  /* "folder-empty" — distinct from "empty" so the wording matches the
   * situation: the user *did* select a folder, it just has nothing in
   * it. See mail-message-list.c::on_messages_loaded. */
  AdwStatusPage *folder_empty = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_icon_name (folder_empty, "mail-read-symbolic");
  adw_status_page_set_title (folder_empty, "No messages");
  adw_status_page_set_description (folder_empty, "This folder is empty.");
  gtk_stack_add_named (self->stack, GTK_WIDGET (folder_empty), "folder-empty");

  /* "list" — virtualising GtkListView inside a scroller.
   *
   * Scroller policy/propagate notes are unchanged from the GtkListBox
   * era: propagate-natural-{width,height}=FALSE so the list view's
   * natural size doesn't escape into AdwOverlaySplitView's measure
   * pass (a 10k-row natural would warn about exceeding window height
   * on every relayout). Overlay scrolling stays off so the scrollbar
   * doesn't fade-animate. */
  self->scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_policy (self->scroller,
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_overlay_scrolling (self->scroller, FALSE);
  gtk_scrolled_window_set_propagate_natural_width (self->scroller, FALSE);
  gtk_scrolled_window_set_propagate_natural_height (self->scroller, FALSE);

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (factory_setup), self);
  g_signal_connect (factory, "bind", G_CALLBACK (factory_bind), self);
  g_signal_connect (factory, "unbind", G_CALLBACK (factory_unbind), self);

  self->selection = GTK_NO_SELECTION (gtk_no_selection_new (G_LIST_MODEL (g_object_ref (self->store))));
  self->list_view = GTK_LIST_VIEW (gtk_list_view_new (GTK_SELECTION_MODEL (self->selection), factory));
  gtk_list_view_set_single_click_activate (self->list_view, TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->list_view), "navigation-sidebar");
  g_signal_connect (self->list_view, "activate",
                    G_CALLBACK (on_list_view_activate), self);

  gtk_scrolled_window_set_child (self->scroller, GTK_WIDGET (self->list_view));
  gtk_stack_add_named (self->stack, GTK_WIDGET (self->scroller), "list");

  gtk_stack_set_visible_child_name (self->stack, "empty");
}
