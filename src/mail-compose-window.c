/* mail-compose-window.c - Plain-text compose dialog. */

#include "config.h"

#include "mail-compose-window.h"

#include "mail-html-to-text.h"
#include "mail-mime.h"
#include "mail-outbox.h"
#include "mail-quote.h"

struct _MailComposeWindow
{
  AdwDialog parent_instance;

  MailAccount *account; /* borrowed */

  /* Reply context: kept alive while the dialog is open so In-Reply-To
   * / References can be set at Send time. NULL for fresh compose. */
  MailMimeReplySource *reply_src;

  /* Template children. */
  GtkWidget *to_row;
  GtkWidget *subject_row;
  GtkWidget *body_view;
  GtkWidget *cancel_button;
  GtkWidget *send_button;
  GtkWidget *toasts;
};

G_DEFINE_FINAL_TYPE (MailComposeWindow, mail_compose_window, ADW_TYPE_DIALOG)

static void
on_cancel_clicked (GtkButton *button, MailComposeWindow *self)
{
  (void) button;
  adw_dialog_close (ADW_DIALOG (self));
}

static char *
dup_body_text (MailComposeWindow *self)
{
  GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->body_view));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (buf, &start, &end);
  return gtk_text_buffer_get_text (buf, &start, &end, FALSE);
}

static void
show_toast (MailComposeWindow *self, const char *text)
{
  AdwToast *toast = adw_toast_new (text);
  adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toasts), toast);
}

static void
on_send_clicked (GtkButton *button, MailComposeWindow *self)
{
  (void) button;
  g_autoptr (GError) error = NULL;
  if (!mail_compose_window_send (self, &error))
    {
      g_autofree char *msg = g_strdup_printf ("Send failed: %s",
                                              error != NULL ? error->message : "(unknown error)");
      show_toast (self, msg);
      return;
    }
  adw_dialog_close (ADW_DIALOG (self));
}

gboolean
mail_compose_window_send (MailComposeWindow *self, GError **error)
{
  g_return_val_if_fail (MAIL_IS_COMPOSE_WINDOW (self), FALSE);
  if (self->account == NULL)
    {
      g_set_error_literal (error, g_quark_from_static_string ("mail-compose-window"), 0,
                           "no account associated with dialog");
      return FALSE;
    }
  const char *to = gtk_editable_get_text (GTK_EDITABLE (self->to_row));
  const char *subject = gtk_editable_get_text (GTK_EDITABLE (self->subject_row));
  g_autofree char *body = dup_body_text (self);
  return mail_outbox_append (self->account, to, subject, body, self->reply_src, error);
}

const char *
mail_compose_window_peek_to (MailComposeWindow *self)
{
  g_return_val_if_fail (MAIL_IS_COMPOSE_WINDOW (self), NULL);
  return gtk_editable_get_text (GTK_EDITABLE (self->to_row));
}

const char *
mail_compose_window_peek_subject (MailComposeWindow *self)
{
  g_return_val_if_fail (MAIL_IS_COMPOSE_WINDOW (self), NULL);
  return gtk_editable_get_text (GTK_EDITABLE (self->subject_row));
}

char *
mail_compose_window_dup_body (MailComposeWindow *self)
{
  g_return_val_if_fail (MAIL_IS_COMPOSE_WINDOW (self), NULL);
  return dup_body_text (self);
}

static void
mail_compose_window_dispose (GObject *object)
{
  MailComposeWindow *self = MAIL_COMPOSE_WINDOW (object);
  g_clear_pointer (&self->reply_src, mail_mime_reply_source_free);
  gtk_widget_dispose_template (GTK_WIDGET (object), MAIL_TYPE_COMPOSE_WINDOW);
  G_OBJECT_CLASS (mail_compose_window_parent_class)->dispose (object);
}

static void
mail_compose_window_class_init (MailComposeWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = mail_compose_window_dispose;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Magpie/ui/compose-window.ui");
  gtk_widget_class_bind_template_child (widget_class, MailComposeWindow, to_row);
  gtk_widget_class_bind_template_child (widget_class, MailComposeWindow, subject_row);
  gtk_widget_class_bind_template_child (widget_class, MailComposeWindow, body_view);
  gtk_widget_class_bind_template_child (widget_class, MailComposeWindow, cancel_button);
  gtk_widget_class_bind_template_child (widget_class, MailComposeWindow, send_button);
  gtk_widget_class_bind_template_child (widget_class, MailComposeWindow, toasts);
}

static void
mail_compose_window_init (MailComposeWindow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  g_signal_connect (self->cancel_button, "clicked",
                    G_CALLBACK (on_cancel_clicked), self);
  g_signal_connect (self->send_button, "clicked",
                    G_CALLBACK (on_send_clicked), self);
}

GtkWidget *
mail_compose_window_new (MailAccount *account)
{
  MailComposeWindow *self = g_object_new (MAIL_TYPE_COMPOSE_WINDOW, NULL);
  self->account = account;
  return GTK_WIDGET (self);
}

GtkWidget *
mail_compose_window_new_reply (MailAccount *account, GBytes *original_raw)
{
  g_return_val_if_fail (account != NULL, NULL);
  g_return_val_if_fail (original_raw != NULL, NULL);

  MailComposeWindow *self = MAIL_COMPOSE_WINDOW (mail_compose_window_new (account));

  gsize len = 0;
  const guint8 *raw = g_bytes_get_data (original_raw, &len);
  MailMimeReplySource *src = mail_mime_extract_reply_source (raw, len);
  if (src == NULL)
    return GTK_WIDGET (self);
  self->reply_src = src;

  /* To: prefer Reply-To, then From. */
  const char *to = src->reply_to_addr != NULL ? src->reply_to_addr : src->from_addr;
  if (to != NULL)
    gtk_editable_set_text (GTK_EDITABLE (self->to_row), to);

  /* Subject. */
  g_autofree char *subj = mail_quote_subject_reply (src->subject);
  gtk_editable_set_text (GTK_EDITABLE (self->subject_row), subj);

  /* Body: build the quoted reply from text/plain, falling back to
   * html→text. */
  g_autofree char *plain = NULL;
  if (src->body_plain != NULL)
    plain = g_strdup (src->body_plain);
  else if (src->body_html != NULL)
    plain = mail_html_to_text (src->body_html, -1);

  g_autofree char *body = mail_quote_build_reply_body (plain != NULL ? plain : "",
                                                       src->from_name, src->from_addr);

  GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->body_view));
  gtk_text_buffer_set_text (buf, body != NULL ? body : "", -1);

  /* Cursor at offset 0 (mutt-style — above the attribution). */
  GtkTextIter start;
  gtk_text_buffer_get_start_iter (buf, &start);
  gtk_text_buffer_place_cursor (buf, &start);

  return GTK_WIDGET (self);
}
