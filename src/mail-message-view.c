/* mail-message-view.c - Raw RFC822 viewer with a plain-text toggle.
 *
 * Layout: a GtkScrolledWindow containing a monospace GtkTextView. The
 * GtkTextBuffer is created once and reused for every message — see
 * [[feedback-memory-reuse]].
 *
 * Each fetch caches two things on the view:
 *   - raw_bytes : the bytes returned by the backend (GBytes)
 *   - plain_text: the decoded text/plain body extracted via GMime,
 *                 or NULL if the message has no plain alternative.
 *
 * The viewer exposes:
 *   - has-plain-part (read-only): TRUE iff plain_text != NULL
 *   - show-plain     (read-write): TRUE renders plain_text, FALSE renders raw
 *
 * The window binds those to a GtkToggleButton in the header bar so
 * toggling the view re-renders the existing buffer with no re-fetch.
 */

#include "config.h"

#include "mail-message-view.h"
#include "mail-mime.h"

#include <adwaita.h>

struct _MailMessageView
{
  GtkWidget parent_instance;

  GtkTextView *text_view;
  GtkTextBuffer *buffer;
  GCancellable *cancellable;

  GBytes *raw_bytes;   /* owned; NULL until first successful fetch */
  gchar *plain_text;   /* owned; NULL if no text/plain part */
  gboolean show_plain; /* current display mode */
};

enum
{
  PROP_0,
  PROP_HAS_PLAIN_PART,
  PROP_SHOW_PLAIN,
  N_PROPS,
};

static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE (MailMessageView, mail_message_view, GTK_TYPE_WIDGET)

static void
set_buffer_utf8 (MailMessageView *self,
                 const char *data,
                 gssize len)
{
  if (data == NULL)
    {
      gtk_text_buffer_set_text (self->buffer, "", 0);
      return;
    }
  if (g_utf8_validate (data, len, NULL))
    {
      gtk_text_buffer_set_text (self->buffer, data, len);
    }
  else
    {
      g_autofree char *valid = g_utf8_make_valid (data, len);
      gtk_text_buffer_set_text (self->buffer, valid, -1);
    }
}

static void
render (MailMessageView *self)
{
  if (self->show_plain && self->plain_text != NULL)
    {
      set_buffer_utf8 (self, self->plain_text, -1);
      return;
    }
  if (self->raw_bytes != NULL)
    {
      gsize len = 0;
      const char *data = g_bytes_get_data (self->raw_bytes, &len);
      set_buffer_utf8 (self, data, (gssize) len);
      return;
    }
  gtk_text_buffer_set_text (self->buffer, "", 0);
}

typedef struct
{
  MailMessageView *self; /* ref'd */
  MailBackend *backend;  /* borrowed */
} LoadMessageCtx;

static void
on_fetch_done (GObject *source,
               GAsyncResult *result,
               gpointer user_data)
{
  LoadMessageCtx *ctx = user_data;
  MailMessageView *self = ctx->self;
  g_autoptr (GError) error = NULL;

  GBytes *body = mail_backend_fetch_message_raw_finish (ctx->backend, result, &error);
  if (body == NULL)
    {
      if (error != NULL && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          char *msg = g_strdup_printf ("(failed to fetch: %s)\n", error->message);
          gtk_text_buffer_set_text (self->buffer, msg, -1);
          g_free (msg);
        }
      g_object_unref (self);
      g_free (ctx);
      return;
    }

  g_clear_pointer (&self->raw_bytes, g_bytes_unref);
  self->raw_bytes = body;

  g_clear_pointer (&self->plain_text, g_free);
  gsize len = 0;
  const guint8 *data = g_bytes_get_data (body, &len);
  self->plain_text = mail_mime_extract_text_plain (data, len);

  /* Every new message starts in raw mode. */
  gboolean prev_show_plain = self->show_plain;
  self->show_plain = FALSE;

  render (self);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HAS_PLAIN_PART]);
  if (prev_show_plain)
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SHOW_PLAIN]);

  g_object_unref (self);
  g_free (ctx);
}

void
mail_message_view_load (MailMessageView *self,
                        MailBackend *backend,
                        const char *message_id)
{
  g_return_if_fail (MAIL_IS_MESSAGE_VIEW (self));
  g_return_if_fail (backend != NULL);
  g_return_if_fail (message_id != NULL);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  gtk_text_buffer_set_text (self->buffer, "Loading…\n", -1);

  LoadMessageCtx *ctx = g_new (LoadMessageCtx, 1);
  ctx->self = g_object_ref (self);
  ctx->backend = backend;
  mail_backend_fetch_message_raw_async (backend, message_id,
                                        self->cancellable,
                                        on_fetch_done, ctx);
}

GtkWidget *
mail_message_view_new (void)
{
  return g_object_new (MAIL_TYPE_MESSAGE_VIEW, NULL);
}

static void
mail_message_view_get_property (GObject *object,
                                guint prop_id,
                                GValue *value,
                                GParamSpec *pspec)
{
  MailMessageView *self = MAIL_MESSAGE_VIEW (object);
  switch (prop_id)
    {
    case PROP_HAS_PLAIN_PART:
      g_value_set_boolean (value, self->plain_text != NULL);
      break;
    case PROP_SHOW_PLAIN:
      g_value_set_boolean (value, self->show_plain);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
mail_message_view_set_property (GObject *object,
                                guint prop_id,
                                const GValue *value,
                                GParamSpec *pspec)
{
  MailMessageView *self = MAIL_MESSAGE_VIEW (object);
  switch (prop_id)
    {
    case PROP_SHOW_PLAIN:
      {
        gboolean v = g_value_get_boolean (value);
        if (v == self->show_plain)
          return;
        self->show_plain = v;
        render (self);
        g_object_notify_by_pspec (object, props[PROP_SHOW_PLAIN]);
        break;
      }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
mail_message_view_dispose (GObject *object)
{
  MailMessageView *self = MAIL_MESSAGE_VIEW (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_pointer (&self->raw_bytes, g_bytes_unref);
  g_clear_pointer (&self->plain_text, g_free);

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))) != NULL)
    gtk_widget_unparent (child);

  G_OBJECT_CLASS (mail_message_view_parent_class)->dispose (object);
}

static void
mail_message_view_class_init (MailMessageViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = mail_message_view_dispose;
  object_class->get_property = mail_message_view_get_property;
  object_class->set_property = mail_message_view_set_property;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);

  props[PROP_HAS_PLAIN_PART] = g_param_spec_boolean ("has-plain-part",
                                                     NULL, NULL,
                                                     FALSE,
                                                     G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  props[PROP_SHOW_PLAIN] = g_param_spec_boolean ("show-plain",
                                                 NULL, NULL,
                                                 FALSE,
                                                 G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
mail_message_view_init (MailMessageView *self)
{
  self->cancellable = g_cancellable_new ();

  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_parent (scroller, GTK_WIDGET (self));

  self->buffer = gtk_text_buffer_new (NULL);
  self->text_view = GTK_TEXT_VIEW (gtk_text_view_new_with_buffer (self->buffer));
  gtk_text_view_set_monospace (self->text_view, TRUE);
  gtk_text_view_set_editable (self->text_view, FALSE);
  gtk_text_view_set_cursor_visible (self->text_view, FALSE);
  gtk_text_view_set_wrap_mode (self->text_view, GTK_WRAP_NONE);
  gtk_text_view_set_top_margin (self->text_view, 8);
  gtk_text_view_set_bottom_margin (self->text_view, 8);
  gtk_text_view_set_left_margin (self->text_view, 12);
  gtk_text_view_set_right_margin (self->text_view, 12);

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), GTK_WIDGET (self->text_view));
}
