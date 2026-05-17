/* mail-progress-ring.c - Cairo-snapshot circular progress arc. */

#include "config.h"

#include "mail-progress-ring.h"

struct _MailProgressRing
{
  GtkWidget parent;
  double fraction;
  gboolean track_visible;
  int size_px;
};

enum
{
  PROP_0,
  PROP_FRACTION,
  PROP_TRACK_VISIBLE,
  PROP_SIZE,
  N_PROPS,
};

static GParamSpec *properties[N_PROPS] = {
  NULL,
};

G_DEFINE_FINAL_TYPE (MailProgressRing, mail_progress_ring, GTK_TYPE_WIDGET)

static void
mail_progress_ring_measure (GtkWidget *widget,
                            GtkOrientation orientation,
                            int for_size,
                            int *minimum,
                            int *natural,
                            int *minimum_baseline,
                            int *natural_baseline)
{
  MailProgressRing *self = MAIL_PROGRESS_RING (widget);
  *minimum = self->size_px;
  *natural = self->size_px;
}

static void
mail_progress_ring_snapshot (GtkWidget *widget,
                             GtkSnapshot *snapshot)
{
  MailProgressRing *self = MAIL_PROGRESS_RING (widget);
  int w = gtk_widget_get_width (widget);
  int h = gtk_widget_get_height (widget);
  if (w <= 0 || h <= 0)
    return;

  /* Stroke proportional to the smaller dimension; thicker at large
   * sizes, thinner in the sidebar button. */
  double side = MIN (w, h);
  double stroke = MAX (2.0, side * 0.12);
  double cx = w / 2.0;
  double cy = h / 2.0;
  double radius = side / 2.0 - stroke / 2.0 - 1.0;
  if (radius <= 0)
    return;

  /* Pull the foreground/accent colour from the widget's style. */
  GdkRGBA color;
  gtk_widget_get_color (widget, &color);

  graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, 0, w, h);
  cairo_t *cr = gtk_snapshot_append_cairo (snapshot, &bounds);

  if (self->track_visible)
    {
      cairo_set_source_rgba (cr, color.red, color.green, color.blue, 0.25 * color.alpha);
      cairo_set_line_width (cr, stroke);
      cairo_arc (cr, cx, cy, radius, 0, 2 * G_PI);
      cairo_stroke (cr);
    }

  if (self->fraction > 0.0)
    {
      cairo_set_source_rgba (cr, color.red, color.green, color.blue, color.alpha);
      cairo_set_line_width (cr, stroke);
      cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
      /* Start at 12 o'clock = -π/2; sweep clockwise. */
      double start = -G_PI / 2.0;
      double end = start + 2 * G_PI * self->fraction;
      cairo_arc (cr, cx, cy, radius, start, end);
      cairo_stroke (cr);
    }

  cairo_destroy (cr);
}

static void
mail_progress_ring_get_property (GObject *object,
                                 guint prop_id,
                                 GValue *value,
                                 GParamSpec *pspec)
{
  MailProgressRing *self = MAIL_PROGRESS_RING (object);
  switch (prop_id)
    {
    case PROP_FRACTION:
      g_value_set_double (value, self->fraction);
      break;
    case PROP_TRACK_VISIBLE:
      g_value_set_boolean (value, self->track_visible);
      break;
    case PROP_SIZE:
      g_value_set_int (value, self->size_px);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
mail_progress_ring_set_property (GObject *object,
                                 guint prop_id,
                                 const GValue *value,
                                 GParamSpec *pspec)
{
  MailProgressRing *self = MAIL_PROGRESS_RING (object);
  switch (prop_id)
    {
    case PROP_FRACTION:
      mail_progress_ring_set_fraction (self, g_value_get_double (value));
      break;
    case PROP_TRACK_VISIBLE:
      mail_progress_ring_set_track_visible (self, g_value_get_boolean (value));
      break;
    case PROP_SIZE:
      mail_progress_ring_set_size (self, g_value_get_int (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
mail_progress_ring_init (MailProgressRing *self)
{
  self->fraction = 0.0;
  self->track_visible = TRUE;
  self->size_px = 24;
}

static void
mail_progress_ring_class_init (MailProgressRingClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = mail_progress_ring_get_property;
  object_class->set_property = mail_progress_ring_set_property;
  widget_class->measure = mail_progress_ring_measure;
  widget_class->snapshot = mail_progress_ring_snapshot;

  properties[PROP_FRACTION] = g_param_spec_double ("fraction", NULL, NULL,
                                                   0.0, 1.0, 0.0,
                                                   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_TRACK_VISIBLE] = g_param_spec_boolean ("track-visible", NULL, NULL,
                                                         TRUE,
                                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_SIZE] = g_param_spec_int ("size", NULL, NULL,
                                            8, 512, 24,
                                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, properties);

  gtk_widget_class_set_css_name (widget_class, "progressring");
}

GtkWidget *
mail_progress_ring_new (void)
{
  return g_object_new (MAIL_TYPE_PROGRESS_RING, NULL);
}

void
mail_progress_ring_set_fraction (MailProgressRing *self,
                                 double fraction)
{
  g_return_if_fail (MAIL_IS_PROGRESS_RING (self));
  if (fraction < 0.0)
    fraction = 0.0;
  if (fraction > 1.0)
    fraction = 1.0;
  if (self->fraction == fraction)
    return;
  self->fraction = fraction;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_FRACTION]);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

double
mail_progress_ring_get_fraction (MailProgressRing *self)
{
  g_return_val_if_fail (MAIL_IS_PROGRESS_RING (self), 0.0);
  return self->fraction;
}

void
mail_progress_ring_set_track_visible (MailProgressRing *self,
                                      gboolean visible)
{
  g_return_if_fail (MAIL_IS_PROGRESS_RING (self));
  visible = !!visible;
  if (self->track_visible == visible)
    return;
  self->track_visible = visible;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TRACK_VISIBLE]);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
mail_progress_ring_get_track_visible (MailProgressRing *self)
{
  g_return_val_if_fail (MAIL_IS_PROGRESS_RING (self), FALSE);
  return self->track_visible;
}

void
mail_progress_ring_set_size (MailProgressRing *self,
                             int px)
{
  g_return_if_fail (MAIL_IS_PROGRESS_RING (self));
  if (self->size_px == px)
    return;
  self->size_px = px;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SIZE]);
  gtk_widget_queue_resize (GTK_WIDGET (self));
}
