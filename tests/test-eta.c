/* tests/test-eta.c - Unit tests for the sliding-window ETA estimator.
 *
 * Pure logic tests — no GTK, no sync engine, no I/O. Feed synthetic
 * (t, fraction) sample streams to MailEta and assert the resulting
 * seconds-remaining estimate behaves correctly under constant rate,
 * speedup, slowdown, and edge cases.
 */

#include "../src/mail-eta.h"

#include <glib.h>

#define WINDOW_US ((gint64) (10 * G_USEC_PER_SEC))

/* Feed a sample stream at uniform interval and uniform fraction step. */
static void
feed_uniform (MailEta *eta,
              gint64 start_us,
              gint64 dt_us,
              double start_fraction,
              double df_per_sample,
              int n_samples)
{
  for (int i = 0; i < n_samples; i++)
    mail_eta_record (eta, start_us + i * dt_us, start_fraction + i * df_per_sample);
}

static void
test_constant_rate_extrapolates (void)
{
  /* 0.1 fraction per second = 10 seconds to complete starting from 0.
   * After 1 second of samples (we need at least 1s span for the rate
   * to be reported), we should see ~9 seconds remaining. */
  MailEta *eta = mail_eta_new (WINDOW_US);
  /* 11 samples 100ms apart: 1 second of span, fraction 0..0.1. */
  feed_uniform (eta, 0, 100 * 1000 /* 100 ms */, 0.0, 0.01, 11);

  double s = mail_eta_seconds_remaining (eta);
  /* Last fraction = 0.10, remaining = 0.90, rate = 0.10/sec → 9.0s. */
  g_assert_cmpfloat (s, >=, 8.5);
  g_assert_cmpfloat (s, <=, 9.5);

  mail_eta_free (eta);
}

static void
test_speedup_mid_stream_shortens_eta (void)
{
  /* Phase 1: 10 seconds at rate 0.02/s (would take 50s total at this
   * rate, so ~40s remaining after phase 1). Phase 2: 5 seconds at
   * rate 0.08/s (4× faster). After phase 2, the window contains only
   * (or mostly) phase-2 samples, so the projected remaining time
   * should be small — meaningfully shorter than the linear-from-start
   * extrapolation would predict. */
  MailEta *eta = mail_eta_new (WINDOW_US);

  /* Phase 1: 100 samples over 10s, fraction 0..0.20. */
  feed_uniform (eta, 0, 100 * 1000, 0.0, 0.002, 101);
  double eta_slow = mail_eta_seconds_remaining (eta);
  /* At rate 0.02/s with 0.80 remaining ⇒ 40s. */
  g_assert_cmpfloat (eta_slow, >=, 35.0);
  g_assert_cmpfloat (eta_slow, <=, 45.0);

  /* Phase 2: 50 samples over 5s, fraction 0.20..0.60 (8× faster — 0.4 in 5s = 0.08/s).
   * Begins at t=10s+100ms (right after the last phase-1 sample). */
  gint64 t0 = 10 * G_USEC_PER_SEC + 100 * 1000;
  feed_uniform (eta, t0, 100 * 1000, 0.202, 0.008, 51);

  double eta_fast = mail_eta_seconds_remaining (eta);
  /* Window now holds the last 10s of samples, dominated by phase 2.
   * Rate ≈ 0.08/s, remaining ≈ 0.4 ⇒ ETA ≈ 5s. */
  g_assert_cmpfloat (eta_fast, >=, 3.0);
  g_assert_cmpfloat (eta_fast, <=, 9.0);

  /* The whole point: post-speedup ETA must be meaningfully shorter
   * than the pre-speedup one. A linear-from-start algorithm would
   * have given us roughly (1-0.6)/0.6 * 15s ≈ 10s, biased high. */
  g_assert_cmpfloat (eta_fast, <, eta_slow / 2.0);

  mail_eta_free (eta);
}

static void
test_slowdown_mid_stream_lengthens_eta (void)
{
  /* Mirror of the speedup test: fast then slow. ETA after slowdown
   * should be longer than before. */
  MailEta *eta = mail_eta_new (WINDOW_US);

  /* Phase 1: 50 samples over 5s, rate 0.08/s, fraction 0..0.40. */
  feed_uniform (eta, 0, 100 * 1000, 0.0, 0.008, 51);
  double eta_fast = mail_eta_seconds_remaining (eta);
  /* Rate 0.08, remaining 0.60 ⇒ 7.5s. */
  g_assert_cmpfloat (eta_fast, >=, 5.0);
  g_assert_cmpfloat (eta_fast, <=, 10.0);

  /* Phase 2: 50 samples over 5s, rate 0.02/s (4× slower), starting
   * at t=5s+100ms; fraction 0.40..0.50. */
  gint64 t0 = 5 * G_USEC_PER_SEC + 100 * 1000;
  feed_uniform (eta, t0, 100 * 1000, 0.402, 0.002, 51);

  double eta_slow = mail_eta_seconds_remaining (eta);
  g_assert_cmpfloat (eta_slow, >, eta_fast * 2.0);

  mail_eta_free (eta);
}

static void
test_too_few_samples_returns_negative (void)
{
  MailEta *eta = mail_eta_new (WINDOW_US);
  g_assert_cmpfloat (mail_eta_seconds_remaining (eta), <, 0.0);
  mail_eta_record (eta, 0, 0.0);
  g_assert_cmpfloat (mail_eta_seconds_remaining (eta), <, 0.0);
  /* Two samples, but only 100ms apart — under the 1-second minimum. */
  mail_eta_record (eta, 100 * 1000, 0.1);
  g_assert_cmpfloat (mail_eta_seconds_remaining (eta), <, 0.0);
  mail_eta_free (eta);
}

static void
test_stalled_returns_negative (void)
{
  /* Same fraction across the whole window → no progress → no ETA. */
  MailEta *eta = mail_eta_new (WINDOW_US);
  feed_uniform (eta, 0, 100 * 1000, 0.5, 0.0, 101);
  g_assert_cmpfloat (mail_eta_seconds_remaining (eta), <, 0.0);
  mail_eta_free (eta);
}

static void
test_complete_returns_negative (void)
{
  MailEta *eta = mail_eta_new (WINDOW_US);
  feed_uniform (eta, 0, 100 * 1000, 0.95, 0.0005, 101);
  /* Last fraction = 1.0 exactly. */
  g_assert_cmpfloat (mail_eta_seconds_remaining (eta), <, 0.0);
  mail_eta_free (eta);
}

static void
test_format_buckets (void)
{
  struct
  {
    double s;
    const char *expected;
  } cases[] = {
    { -1.0, "Calculating…" },
    { 0.0, "Less than a minute remaining" },
    { 59.0, "Less than a minute remaining" },
    { 60.0, "About 1 minute remaining" },
    { 119.0, "About 1 minute remaining" },
    { 120.0, "About 2 minutes remaining" },
    { 3599.0, "About 59 minutes remaining" },
    { 3600.0, "About 1 hour remaining" },
    { 7199.0, "About 1 hour remaining" },
    { 7200.0, "About 2 hours remaining" },
    { 36000.0, "About 10 hours remaining" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *text = mail_eta_format (cases[i].s);
      g_assert_cmpstr (text, ==, cases[i].expected);
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/eta/constant-rate", test_constant_rate_extrapolates);
  g_test_add_func ("/eta/speedup-shortens", test_speedup_mid_stream_shortens_eta);
  g_test_add_func ("/eta/slowdown-lengthens", test_slowdown_mid_stream_lengthens_eta);
  g_test_add_func ("/eta/too-few-samples", test_too_few_samples_returns_negative);
  g_test_add_func ("/eta/stalled", test_stalled_returns_negative);
  g_test_add_func ("/eta/complete", test_complete_returns_negative);
  g_test_add_func ("/eta/format-buckets", test_format_buckets);
  return g_test_run ();
}
