/* mail-eta.c - Sliding-window ETA. See header for rationale. */

#include "config.h"

#include "mail-eta.h"

#include <math.h>

#define MAIL_ETA_RING_CAP 64
#define MAIL_ETA_MIN_SPAN_US ((gint64) (1 * G_USEC_PER_SEC))

typedef struct
{
  gint64 t_us;
  double fraction;
} Sample;

struct _MailEta
{
  gint64 window_us;
  Sample ring[MAIL_ETA_RING_CAP];
  int head;  /* index of the oldest sample */
  int count; /* number of valid samples */
};

MailEta *
mail_eta_new (gint64 window_us)
{
  MailEta *eta = g_new0 (MailEta, 1);
  eta->window_us = window_us > 0 ? window_us : 10 * G_USEC_PER_SEC;
  return eta;
}

void
mail_eta_free (MailEta *eta)
{
  g_free (eta);
}

void
mail_eta_reset (MailEta *eta)
{
  if (eta == NULL)
    return;
  eta->head = 0;
  eta->count = 0;
}

static inline Sample *
sample_at (MailEta *eta, int i)
{
  return &eta->ring[(eta->head + i) % MAIL_ETA_RING_CAP];
}

static void
drop_stale (MailEta *eta, gint64 now_us)
{
  /* A sample is stale when it's older than (now - window). Drop from
   * the head until the oldest surviving sample is within the window.
   * Always keep at least one sample so we can still compute a span. */
  while (eta->count > 1)
    {
      Sample *oldest = sample_at (eta, 0);
      if (now_us - oldest->t_us <= eta->window_us)
        break;
      eta->head = (eta->head + 1) % MAIL_ETA_RING_CAP;
      eta->count--;
    }
}

void
mail_eta_record (MailEta *eta, gint64 now_us, double fraction)
{
  if (eta == NULL)
    return;
  drop_stale (eta, now_us);

  /* If the ring is full, evict the oldest sample. */
  if (eta->count == MAIL_ETA_RING_CAP)
    {
      eta->head = (eta->head + 1) % MAIL_ETA_RING_CAP;
      eta->count--;
    }

  Sample *slot = sample_at (eta, eta->count);
  slot->t_us = now_us;
  slot->fraction = fraction;
  eta->count++;
}

double
mail_eta_seconds_remaining (MailEta *eta)
{
  if (eta == NULL || eta->count < 2)
    return -1.0;

  Sample *first = sample_at (eta, 0);
  Sample *last = sample_at (eta, eta->count - 1);

  /* Need at least a 1-second span before the rate is meaningful. */
  gint64 dt_us = last->t_us - first->t_us;
  if (dt_us < MAIL_ETA_MIN_SPAN_US)
    return -1.0;

  /* Sync is done. */
  if (last->fraction >= 1.0)
    return -1.0;

  /* Stalled or regressed: no usable rate. */
  double df = last->fraction - first->fraction;
  if (df <= 0.0)
    return -1.0;

  double rate_per_us = df / (double) dt_us;
  double remaining = 1.0 - last->fraction;
  double seconds = remaining / rate_per_us / 1e6;

  /* Cap absurd extrapolations (e.g., a tiny early-run rate predicting
   * hundreds of hours) — the caller's bucket formatter handles >2h as
   * "About N hours remaining" anyway, but keep the number finite. */
  if (!isfinite (seconds) || seconds < 0.0)
    return -1.0;

  return seconds;
}

char *
mail_eta_format (double seconds)
{
  if (seconds < 0.0)
    return g_strdup ("Calculating…");
  if (seconds < 60.0)
    return g_strdup ("Less than a minute remaining");
  if (seconds < 120.0)
    return g_strdup ("About 1 minute remaining");
  if (seconds < 3600.0)
    {
      int minutes = (int) (seconds / 60.0);
      return g_strdup_printf ("About %d minutes remaining", minutes);
    }
  if (seconds < 7200.0)
    return g_strdup ("About 1 hour remaining");
  int hours = (int) (seconds / 3600.0);
  return g_strdup_printf ("About %d hours remaining", hours);
}
