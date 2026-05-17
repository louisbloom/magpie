/* mail-window.c - AdwApplicationWindow with OverlaySplitView + NavigationView. */

#include "config.h"

#include "mail-message-list.h"
#include "mail-message-view.h"
#include "mail-sidebar.h"
#include "mail-window.h"

struct _MailWindow
{
  AdwApplicationWindow parent_instance;

  /* template children — borrowed pointers, owned by the template. */
  AdwOverlaySplitView *split_view;
  AdwNavigationView *nav_view;
  GtkToggleButton *sidebar_toggle;
  MailSidebar *sidebar;
  MailMessageList *message_list;
  MailMessageView *message_view;
  AdwNavigationPage *message_view_page;
  GtkToggleButton *plain_toggle;
};

G_DEFINE_FINAL_TYPE (MailWindow, mail_window, ADW_TYPE_APPLICATION_WINDOW)

static void
on_folder_selected (MailSidebar *sidebar,
                    gpointer backend_ptr,
                    const char *folder_id,
                    gpointer user_data)
{
  MailWindow *self = MAIL_WINDOW (user_data);
  MailBackend *backend = (MailBackend *) backend_ptr;
  if (backend == NULL || folder_id == NULL)
    return;
  adw_navigation_view_pop_to_tag (self->nav_view, "message-list");
  mail_message_list_load (self->message_list, backend, folder_id, 50);
}

static void
on_message_activated (MailMessageList *list,
                      gpointer backend_ptr,
                      const char *message_id,
                      const char *subject,
                      gpointer user_data)
{
  MailWindow *self = MAIL_WINDOW (user_data);
  MailBackend *backend = (MailBackend *) backend_ptr;
  if (backend == NULL || message_id == NULL)
    return;
  adw_navigation_page_set_title (self->message_view_page,
                                 (subject != NULL && subject[0] != '\0') ? subject : "Message");
  mail_message_view_load (self->message_view, backend, message_id);
  adw_navigation_view_push_by_tag (self->nav_view, "message-view");
}

static void
mail_window_class_init (MailWindowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  /* Register custom widget types so the template parser can find them. */
  g_type_ensure (MAIL_TYPE_SIDEBAR);
  g_type_ensure (MAIL_TYPE_MESSAGE_LIST);
  g_type_ensure (MAIL_TYPE_MESSAGE_VIEW);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/org/gnome/Mail/ui/window.ui");
  gtk_widget_class_bind_template_child (widget_class, MailWindow, split_view);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, nav_view);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, sidebar_toggle);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, sidebar);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, message_list);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, message_view);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, message_view_page);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, plain_toggle);
}

static void
on_plain_toggle_active_changed (GObject *object,
                                GParamSpec *pspec,
                                gpointer user_data)
{
  GtkToggleButton *btn = GTK_TOGGLE_BUTTON (object);
  gboolean active = gtk_toggle_button_get_active (btn);
  gtk_button_set_icon_name (GTK_BUTTON (btn),
                            active ? "text-x-generic-symbolic"
                                   : "view-paged-symbolic");
}

static void
mail_window_init (MailWindow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  g_object_bind_property (self->split_view, "show-sidebar",
                          self->sidebar_toggle, "active",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

  g_object_bind_property (self->message_view, "has-plain-part",
                          self->plain_toggle, "visible",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (self->message_view, "show-plain",
                          self->plain_toggle, "active",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
  g_signal_connect (self->plain_toggle, "notify::active",
                    G_CALLBACK (on_plain_toggle_active_changed), self);

  g_signal_connect (self->sidebar, "folder-selected",
                    G_CALLBACK (on_folder_selected), self);
  g_signal_connect (self->message_list, "message-activated",
                    G_CALLBACK (on_message_activated), self);
}

MailWindow *
mail_window_new (MailApplication *app)
{
  return g_object_new (MAIL_TYPE_WINDOW,
                       "application", app,
                       NULL);
}
