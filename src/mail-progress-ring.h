/* mail-progress-ring.h - Circular determinate-progress arc widget.
 *
 * Paints a track circle and an overlaid arc sweep from 12 o'clock
 * clockwise to `fraction * 2π`. Sized via the standard GTK4 measure
 * protocol — the parent picks the radius. Used at small size in the
 * sidebar refresh button and at full size in the account page.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define MAIL_TYPE_PROGRESS_RING (mail_progress_ring_get_type ())
G_DECLARE_FINAL_TYPE (MailProgressRing, mail_progress_ring, MAIL, PROGRESS_RING, GtkWidget)

GtkWidget *mail_progress_ring_new (void);

void mail_progress_ring_set_fraction (MailProgressRing *self, double fraction);
double mail_progress_ring_get_fraction (MailProgressRing *self);

void mail_progress_ring_set_track_visible (MailProgressRing *self, gboolean visible);
gboolean mail_progress_ring_get_track_visible (MailProgressRing *self);

/* Preferred size; parent measures pick this. Default 24 px. */
void mail_progress_ring_set_size (MailProgressRing *self, int px);

G_END_DECLS
