/* mail-eta.h - Sliding-window ETA estimator.
 *
 * Pure logic, no GTK dependency, so it unit-tests against a synthetic
 * sample stream. Used by mail-account-page to render an ETA below the
 * sync progress ring.
 *
 * Algorithm: ring buffer of (monotonic_time_us, fraction) samples.
 * On each record, samples older than `window_us` are aged out. Rate =
 * Δfraction / Δt over the surviving window. ETA = (1 − fraction) /
 * rate. The window cap (rather than EWMA or linear-from-start)
 * absorbs single-message jitter while reacting to genuine speed
 * changes within `window_us`.
 *
 * Why not linear-from-start: it averages over a phase whose rate
 * isn't representative of the future (e.g. fast metadata then slow
 * body fetches). Why not instantaneous rate: per-event variance is
 * huge. Why not EWMA: the time constant is opaque; window_us is self-
 * documenting and easier to test.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _MailEta MailEta;

/* @window_us is the sliding window in microseconds. 10 * G_USEC_PER_SEC
 * is a sensible default — long enough to absorb jitter, short enough
 * to track real speed changes within a few seconds. */
MailEta *mail_eta_new (gint64 window_us);
void mail_eta_free (MailEta *eta);

/* Clear all recorded samples. Call this on a fresh sync pass so the
 * old run doesn't contaminate the new run's rate estimate. */
void mail_eta_reset (MailEta *eta);

/* Record an observation. @now_us must be monotonically non-decreasing
 * across calls; pass g_get_monotonic_time(). @fraction is in [0, 1]. */
void mail_eta_record (MailEta *eta, gint64 now_us, double fraction);

/* Returns seconds remaining, or -1 if no estimate is available
 * (fewer than 2 samples spanning at least 1 second, stalled, or
 * fraction has reached 1.0). */
double mail_eta_seconds_remaining (MailEta *eta);

/* Format a seconds-remaining value (or -1) into a human string.
 * Returns a g_strdup'd string the caller frees. Buckets:
 *   < 0   → "Calculating…"
 *   < 60  → "Less than a minute remaining"
 *   < 120 → "About 1 minute remaining"
 *   < 3600 → "About N minutes remaining"
 *   < 7200 → "About 1 hour remaining"
 *   else  → "About N hours remaining"
 * The bucketed display prevents jitter — only crossing a threshold
 * changes the rendered text. */
char *mail_eta_format (double seconds);

G_END_DECLS
