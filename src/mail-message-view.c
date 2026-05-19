/* mail-message-view.c - MIME-aware message viewer.
 *
 * Three exclusive view modes, switched from the header bar's
 * AdwToggleGroup (rendered / plain / source) bound to the
 * MailMessageViewMode enum on this widget:
 *
 *   RENDERED — default after every load. mail_mime_pick_best picks
 *     the richest displayable part per RFC 2046 §5.1.4:
 *       text/html  → WebKitWebView page (sandboxed; see below)
 *       text/plain → the existing GtkTextView (monospace, no wrap)
 *       neither    → AdwStatusPage explaining what we found
 *   PLAIN    — forces the text/plain alternative via
 *     mail_mime_extract_text_plain. Insensitive when no plain part
 *     exists (has-plain-part property reports this).
 *   SOURCE   — the raw RFC822 bytes in the GtkTextView. This is the
 *     pre-MIME-rewrite default behaviour, kept for debugging.
 *
 * Layout: GtkBinLayout root hosts a GtkStack with three named pages:
 *   "web"    — WebKitWebView for HTML rendering
 *   "text"   — GtkScrolledWindow > GtkTextView for plain & source
 *   "status" — AdwStatusPage for the unsupported branch
 *
 * The text page reuses the GtkTextBuffer across every message — see
 * [[feedback-memory-reuse]].
 *
 * Each fetch caches on the view:
 *   raw_bytes    : bytes returned by the backend (GBytes, owned)
 *   plain_text   : decoded text/plain alternative (gchar *, may be NULL)
 *   best_kind    : MailMimeKind from the picker
 *   best_content : decoded best part (gchar *, NULL for UNSUPPORTED)
 *   detail       : MIME type that produced UNSUPPORTED, or NULL
 *
 * WebKit sandboxing — we never want a tracking pixel to leak the user's
 * IP to a sender. The web view is configured with:
 *   - ephemeral NetworkSession (no on-disk cache / cookies)
 *   - JavaScript disabled
 *   - HTML5 database / local-storage disabled
 *   - decide-policy refuses every navigation that isn't the initial
 *     load of about:blank-style data (i.e. our load_html call). Remote
 *     image loads still go through resource-load-started, where we
 *     simply do nothing — the resource load proceeds for the in-memory
 *     HTML render, but the ephemeral session ensures no persistence.
 *     A full network blackout is a follow-up.
 *
 * Scrollbar gating (text page only) — two cooperating mechanisms
 * (both required):
 *
 *  (1) propagate-natural-{width,height} = FALSE on the scroller.
 *      Without this, opening a message with base64 attachments (~1500
 *      wrapped lines, ~17 kpx) causes the GtkTextView's natural to
 *      flow upward and AdwOverlaySplitView warns about exceeding the
 *      MailWindow height on every measure pass.
 *
 *  (2) POLICY_EXTERNAL ↔ POLICY_AUTOMATIC dance keyed on the
 *      containing AdwNavigationPage's `showing`/`hiding`/`shown`
 *      signals. While the page is animating in or hidden, policy is
 *      EXTERNAL so no scrollbar widget exists (and thus no internal
 *      trough/slider GtkGizmo subtree to snapshot before allocation
 *      settles); once `shown` fires we flip to AUTOMATIC so a real
 *      scrollbar appears as needed.
 *
 *      Important: POLICY_NEVER does NOT work here — it also hides
 *      the scrollbar widget, but it forces the scroller to allocate
 *      its child the full natural size (ignoring propagate-natural-*),
 *      which re-triggers the AdwOverlaySplitView height warning.
 *      POLICY_EXTERNAL hides the scrollbar and still honours
 *      propagate-natural-*=FALSE, so it satisfies both constraints.
 *
 *      Without the dance, the AUTOMATIC scrollbar's internal gizmo
 *      subtree races against AdwNavigationView's first allocation
 *      pass and the frame clock emits
 *        Gtk-WARNING: Trying to snapshot GtkGizmo without a current allocation
 *      (confirmed by debug_break_on_gizmo_warning in main.c: the bad
 *      widget is always the scrollbar trough).
 */

#include "config.h"

#include "mail-message-view.h"
#include "mail-mime.h"

#include <adwaita.h>
#include <webkit/webkit.h>

struct _MailMessageView
{
  GtkWidget parent_instance;

  GtkStack *stack;             /* borrowed; child of self */
  GtkScrolledWindow *scroller; /* borrowed; child of stack */
  GtkTextView *text_view;
  GtkTextBuffer *buffer;
  WebKitWebView *web_view;
  AdwStatusPage *status_page;

  GCancellable *cancellable;

  GBytes *raw_bytes;      /* owned; NULL until first successful fetch */
  gchar *plain_text;      /* owned; NULL if no text/plain alternative */
  MailMimeKind best_kind; /* result of mail_mime_pick_best for last load */
  gchar *best_content;    /* owned; NULL on UNSUPPORTED or before first load */
  gchar *detail;          /* owned; MIME type behind UNSUPPORTED, or NULL */

  MailMessageViewMode mode; /* current display mode */

  gboolean signals_connected; /* idempotency guard for realize-time hookup */
};

enum
{
  PROP_0,
  PROP_HAS_PLAIN_PART,
  PROP_HAS_RAW,
  PROP_VIEW_MODE,
  N_PROPS,
};

static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE (MailMessageView, mail_message_view, GTK_TYPE_WIDGET)

GType
mail_message_view_mode_get_type (void)
{
  static gsize once = 0;
  static GType the_type = 0;
  if (g_once_init_enter (&once))
    {
      static const GEnumValue values[] = {
        { MAIL_MESSAGE_VIEW_MODE_RENDERED, "MAIL_MESSAGE_VIEW_MODE_RENDERED", "rendered" },
        { MAIL_MESSAGE_VIEW_MODE_PLAIN, "MAIL_MESSAGE_VIEW_MODE_PLAIN", "plain" },
        { MAIL_MESSAGE_VIEW_MODE_SOURCE, "MAIL_MESSAGE_VIEW_MODE_SOURCE", "source" },
        { 0, NULL, NULL },
      };
      the_type = g_enum_register_static ("MailMessageViewMode", values);
      g_once_init_leave (&once, 1);
    }
  return the_type;
}

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
show_text_page (MailMessageView *self, const char *data, gssize len)
{
  set_buffer_utf8 (self, data, len);
  gtk_stack_set_visible_child_name (self->stack, "text");
}

static void
show_html_page (MailMessageView *self, const char *html)
{
  /* about:blank base URI ensures relative URLs and document.URL don't
   * leak anything message-specific. */
  webkit_web_view_load_html (self->web_view, html != NULL ? html : "",
                             "about:blank");
  gtk_stack_set_visible_child_name (self->stack, "web");
}

static void
show_status_page (MailMessageView *self, const char *title, const char *description)
{
  adw_status_page_set_title (self->status_page, title);
  adw_status_page_set_description (self->status_page, description);
  gtk_stack_set_visible_child_name (self->stack, "status");
}

static void
render (MailMessageView *self)
{
  switch (self->mode)
    {
    case MAIL_MESSAGE_VIEW_MODE_PLAIN:
      if (self->plain_text != NULL)
        {
          show_text_page (self, self->plain_text, -1);
        }
      else
        {
          show_status_page (self,
                            "Nothing to display",
                            "This message has no plain-text alternative.");
        }
      return;

    case MAIL_MESSAGE_VIEW_MODE_SOURCE:
      if (self->raw_bytes != NULL)
        {
          gsize len = 0;
          const char *data = g_bytes_get_data (self->raw_bytes, &len);
          show_text_page (self, data, (gssize) len);
        }
      else
        {
          show_text_page (self, "", 0);
        }
      return;

    case MAIL_MESSAGE_VIEW_MODE_RENDERED:
    default:
      break;
    }

  switch (self->best_kind)
    {
    case MAIL_MIME_KIND_HTML:
      show_html_page (self, self->best_content);
      return;
    case MAIL_MIME_KIND_PLAIN:
      show_text_page (self, self->best_content != NULL ? self->best_content : "", -1);
      return;
    case MAIL_MIME_KIND_UNSUPPORTED:
      {
        g_autofree char *description = NULL;
        if (self->detail != NULL)
          description = g_strdup_printf ("Unsupported content type: %s.\n"
                                         "Use the Source toggle to view the raw message.",
                                         self->detail);
        else
          description = g_strdup ("This message could not be parsed.\n"
                                  "Use the Source toggle to view the raw bytes.");
        show_status_page (self, "Nothing to display", description);
        return;
      }
    }
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
          g_autofree char *msg = g_strdup_printf ("Failed to fetch message: %s",
                                                  error->message);
          show_status_page (self, "Could not load message", msg);
        }
      g_object_unref (self);
      g_free (ctx);
      return;
    }

  g_clear_pointer (&self->raw_bytes, g_bytes_unref);
  self->raw_bytes = body;

  g_clear_pointer (&self->plain_text, g_free);
  g_clear_pointer (&self->best_content, g_free);
  g_clear_pointer (&self->detail, g_free);

  gsize len = 0;
  const guint8 *data = g_bytes_get_data (body, &len);
  self->plain_text = mail_mime_extract_text_plain (data, len);
  self->best_kind = mail_mime_pick_best (data, len, &self->best_content, &self->detail);

  /* Reset to the default mode on every load. */
  MailMessageViewMode prev_mode = self->mode;
  self->mode = MAIL_MESSAGE_VIEW_MODE_RENDERED;

  render (self);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HAS_PLAIN_PART]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HAS_RAW]);
  if (prev_mode != MAIL_MESSAGE_VIEW_MODE_RENDERED)
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_VIEW_MODE]);

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

  show_text_page (self, "Loading…\n", -1);

  LoadMessageCtx *ctx = g_new (LoadMessageCtx, 1);
  ctx->self = g_object_ref (self);
  ctx->backend = backend;
  mail_backend_fetch_message_raw_async (backend, message_id,
                                        self->cancellable,
                                        on_fetch_done, ctx);
}

GBytes *
mail_message_view_peek_raw (MailMessageView *self)
{
  g_return_val_if_fail (MAIL_IS_MESSAGE_VIEW (self), NULL);
  return self->raw_bytes;
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
    case PROP_HAS_RAW:
      g_value_set_boolean (value, self->raw_bytes != NULL);
      break;
    case PROP_VIEW_MODE:
      g_value_set_enum (value, self->mode);
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
    case PROP_VIEW_MODE:
      {
        MailMessageViewMode v = g_value_get_enum (value);
        if (v == self->mode)
          return;
        self->mode = v;
        render (self);
        g_object_notify_by_pspec (object, props[PROP_VIEW_MODE]);
        break;
      }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
on_page_to_never (AdwNavigationPage *page,
                  gpointer user_data)
{
  MailMessageView *self = MAIL_MESSAGE_VIEW (user_data);
  gtk_scrolled_window_set_policy (self->scroller,
                                  GTK_POLICY_EXTERNAL, GTK_POLICY_EXTERNAL);
}

static void
on_page_to_automatic (AdwNavigationPage *page,
                      gpointer user_data)
{
  MailMessageView *self = MAIL_MESSAGE_VIEW (user_data);
  gtk_scrolled_window_set_policy (self->scroller,
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
}

static void
mail_message_view_realize (GtkWidget *widget)
{
  GTK_WIDGET_CLASS (mail_message_view_parent_class)->realize (widget);

  MailMessageView *self = MAIL_MESSAGE_VIEW (widget);
  if (self->signals_connected)
    return;

  GtkWidget *ancestor = gtk_widget_get_ancestor (widget, ADW_TYPE_NAVIGATION_PAGE);
  if (ancestor == NULL)
    return;
  AdwNavigationPage *page = ADW_NAVIGATION_PAGE (ancestor);

  g_signal_connect_object (page, "showing",
                           G_CALLBACK (on_page_to_never), self, 0);
  g_signal_connect_object (page, "hiding",
                           G_CALLBACK (on_page_to_never), self, 0);
  g_signal_connect_object (page, "shown",
                           G_CALLBACK (on_page_to_automatic), self, 0);
  self->signals_connected = TRUE;

  GtkWidget *view = gtk_widget_get_ancestor (widget, ADW_TYPE_NAVIGATION_VIEW);
  if (view != NULL && adw_navigation_view_get_visible_page (ADW_NAVIGATION_VIEW (view)) == page)
    on_page_to_automatic (page, self);
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
  g_clear_pointer (&self->best_content, g_free);
  g_clear_pointer (&self->detail, g_free);

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
  widget_class->realize = mail_message_view_realize;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);

  props[PROP_HAS_PLAIN_PART] = g_param_spec_boolean ("has-plain-part",
                                                     NULL, NULL,
                                                     FALSE,
                                                     G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  props[PROP_HAS_RAW] = g_param_spec_boolean ("has-raw",
                                              NULL, NULL,
                                              FALSE,
                                              G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  props[PROP_VIEW_MODE] = g_param_spec_enum ("view-mode",
                                             NULL, NULL,
                                             MAIL_TYPE_MESSAGE_VIEW_MODE,
                                             MAIL_MESSAGE_VIEW_MODE_RENDERED,
                                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, props);
}

/* Refuse anything beyond the initial load_html("about:blank") call.
 * load_html itself shows up as a navigation to about:blank; allow that,
 * deny everything else (clicking a link inside an HTML email, redirects,
 * etc.). Without this, clicking a link would replace our message view
 * with the destination page. */
static gboolean
on_webkit_decide_policy (WebKitWebView *view,
                         WebKitPolicyDecision *decision,
                         WebKitPolicyDecisionType type,
                         gpointer user_data)
{
  if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION && type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
    return FALSE;

  WebKitNavigationPolicyDecision *nav = WEBKIT_NAVIGATION_POLICY_DECISION (decision);
  WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action (nav);
  WebKitNavigationType ntype = webkit_navigation_action_get_navigation_type (action);

  /* WEBKIT_NAVIGATION_TYPE_OTHER covers the initial load_html call. */
  if (ntype == WEBKIT_NAVIGATION_TYPE_OTHER)
    return FALSE;

  webkit_policy_decision_ignore (decision);
  return TRUE;
}

static void
mail_message_view_init (MailMessageView *self)
{
  self->cancellable = g_cancellable_new ();
  self->mode = MAIL_MESSAGE_VIEW_MODE_RENDERED;
  self->best_kind = MAIL_MIME_KIND_UNSUPPORTED;

  self->stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_transition_type (self->stack, GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_widget_set_parent (GTK_WIDGET (self->stack), GTK_WIDGET (self));

  /* "text" page — scrolled GtkTextView (plain + source modes). The
   * scroller's policy dance is preserved verbatim from the previous
   * implementation; see the top-of-file comment for the rationale. */
  self->scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_policy (self->scroller,
                                  GTK_POLICY_EXTERNAL, GTK_POLICY_EXTERNAL);
  gtk_scrolled_window_set_overlay_scrolling (self->scroller, FALSE);
  gtk_scrolled_window_set_propagate_natural_width (self->scroller, FALSE);
  gtk_scrolled_window_set_propagate_natural_height (self->scroller, FALSE);

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

  gtk_scrolled_window_set_child (self->scroller, GTK_WIDGET (self->text_view));
  gtk_stack_add_named (self->stack, GTK_WIDGET (self->scroller), "text");

  /* "web" page — WebKitWebView for HTML rendering. Ephemeral session
   * so no on-disk cache or cookies persist; remote loads are still
   * issued by the engine for inline <img> etc. — that's a known limit,
   * see top-of-file comment. */
  WebKitNetworkSession *ephemeral = webkit_network_session_new_ephemeral ();
  self->web_view = WEBKIT_WEB_VIEW (g_object_new (WEBKIT_TYPE_WEB_VIEW,
                                                  "network-session", ephemeral,
                                                  NULL));
  g_object_unref (ephemeral);

  WebKitSettings *settings = webkit_web_view_get_settings (self->web_view);
  webkit_settings_set_enable_javascript (settings, FALSE);
  webkit_settings_set_enable_html5_database (settings, FALSE);
  webkit_settings_set_enable_html5_local_storage (settings, FALSE);
  webkit_settings_set_enable_developer_extras (settings, FALSE);

  g_signal_connect (self->web_view, "decide-policy",
                    G_CALLBACK (on_webkit_decide_policy), NULL);

  gtk_widget_set_hexpand (GTK_WIDGET (self->web_view), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->web_view), TRUE);
  gtk_stack_add_named (self->stack, GTK_WIDGET (self->web_view), "web");

  /* "status" page — AdwStatusPage for unsupported content / errors. */
  self->status_page = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_icon_name (self->status_page, "mail-mark-junk-symbolic");
  adw_status_page_set_title (self->status_page, "Nothing to display");
  gtk_stack_add_named (self->stack, GTK_WIDGET (self->status_page), "status");

  gtk_stack_set_visible_child_name (self->stack, "text");
}
