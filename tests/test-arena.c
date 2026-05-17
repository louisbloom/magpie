/* test-arena.c - Unit tests for the MailArena bump allocator. */

#include "mail-arena.h"

#include <glib.h>
#include <stdint.h>
#include <string.h>

static void
test_init_zero (void)
{
  MailArena a;
  mail_arena_init (&a, 0);
  g_assert_cmpuint (mail_arena_capacity (&a), ==, 0);
  g_assert_cmpuint (mail_arena_used (&a), ==, 0);
  mail_arena_destroy (&a);
}

static void
test_basic_alloc (void)
{
  MailArena a;
  mail_arena_init (&a, 16);

  int *x = mail_arena_alloc (&a, sizeof (int), _Alignof (int));
  *x = 42;
  g_assert_cmpint (*x, ==, 42);

  int *y = mail_arena_alloc (&a, sizeof (int), _Alignof (int));
  *y = 99;
  g_assert_cmpint (*y, ==, 99);
  g_assert (x != y);
  g_assert_cmpint (*x, ==, 42);

  mail_arena_destroy (&a);
}

static void
test_alignment (void)
{
  MailArena a;
  mail_arena_init (&a, 64);

  /* odd-sized allocation forces non-zero used pointer */
  (void) mail_arena_alloc (&a, 1, 1);

  /* request 8-byte alignment */
  uint64_t *p = mail_arena_alloc (&a, sizeof (uint64_t), 8);
  g_assert_cmpuint (((uintptr_t) p) & 7, ==, 0);

  mail_arena_destroy (&a);
}

static void
test_strdup (void)
{
  MailArena a;
  mail_arena_init (&a, 16);

  char *s = mail_arena_strdup (&a, "hello, arena");
  g_assert_cmpstr (s, ==, "hello, arena");

  char *t = mail_arena_strndup (&a, "hello, world", 5);
  g_assert_cmpstr (t, ==, "hello");

  mail_arena_destroy (&a);
}

static void
test_grow (void)
{
  MailArena a;
  mail_arena_init (&a, 16);
  size_t initial_cap = mail_arena_capacity (&a);

  /* Force several grows. */
  for (int i = 0; i < 1000; i++)
    {
      char *buf = mail_arena_alloc (&a, 64, 1);
      memset (buf, 'a' + (i % 26), 64);
    }
  g_assert_cmpuint (mail_arena_capacity (&a), >, initial_cap);
  g_assert_cmpuint (mail_arena_used (&a), >=, 1000 * 64);

  mail_arena_destroy (&a);
}

static void
test_reset_preserves_capacity (void)
{
  MailArena a;
  mail_arena_init (&a, 16);

  (void) mail_arena_alloc (&a, 10000, 1); /* forces grow */
  size_t cap_after_grow = mail_arena_capacity (&a);

  mail_arena_reset (&a);
  g_assert_cmpuint (mail_arena_used (&a), ==, 0);
  g_assert_cmpuint (mail_arena_capacity (&a), ==, cap_after_grow);

  /* Subsequent allocations reuse the buffer without further grows. */
  (void) mail_arena_alloc (&a, 8000, 1);
  g_assert_cmpuint (mail_arena_capacity (&a), ==, cap_after_grow);

  mail_arena_destroy (&a);
}

static void
test_zero_byte_alloc_returns_null (void)
{
  MailArena a;
  mail_arena_init (&a, 16);
  g_assert_null (mail_arena_alloc (&a, 0, 1));
  mail_arena_destroy (&a);
}

static void
test_pointer_stability_across_grow (void)
{
  /* Regression: with a realloc-based bump allocator, growing the arena
   * could move every prior pointer out from under the caller. Our
   * chunked allocator must keep them stable.
   *
   * Production trigger: msgraph::parse_messages_page allocates a
   * MailMessageMeta with mail_arena_alloc and then fills its fields
   * with further mail_arena_strdup calls in the same loop body. If
   * the strdups grow the arena, the meta pointer dangles and the
   * bind_model factory later segfaults reading m->subject. */
  MailArena a;
  mail_arena_init (&a, 16); /* deliberately tiny to force growth */

  int *first = mail_arena_alloc (&a, sizeof (int), _Alignof (int));
  *first = 0xdeadbeef;

  /* Force many subsequent allocations that exceed the initial chunk. */
  for (int i = 0; i < 1000; i++)
    {
      char *buf = mail_arena_alloc (&a, 64, 1);
      memset (buf, (char) ('a' + (i % 26)), 64);
    }

  /* The original pointer must still resolve to its original value. */
  g_assert_cmpint (*first, ==, (int) 0xdeadbeef);

  mail_arena_destroy (&a);
}

static void
test_pointer_stability_struct_then_strdup (void)
{
  /* Real-world pattern from msgraph::parse_messages_page:
   *
   *   M *m = arena_alloc(sizeof *m);
   *   m->id = arena_strdup(arena, id_from_json);
   *
   * The strdup must not invalidate m. This was the production
   * SIGSEGV in build_row_widget when the user clicked Inbox: 1768
   * messages × hundreds of strdup-bytes overflowed the initial 4 KB
   * arena, realloc moved the buffer, and `m` dangled. */
  typedef struct
  {
    const char *a;
    const char *b;
    const char *c;
  } Triple;

  MailArena a;
  mail_arena_init (&a, 32);

  Triple *records[64];
  const char *expected_a[64];
  for (int i = 0; i < 64; i++)
    {
      records[i] = mail_arena_alloc (&a, sizeof (Triple), _Alignof (Triple));
      g_autofree char *a_str = g_strdup_printf ("a-%d", i);
      g_autofree char *b_str = g_strdup_printf ("b-with-padding-%d", i);
      g_autofree char *c_str = g_strdup_printf ("c-quite-a-bit-longer-string-%d", i);
      records[i]->a = mail_arena_strdup (&a, a_str);
      records[i]->b = mail_arena_strdup (&a, b_str);
      records[i]->c = mail_arena_strdup (&a, c_str);
      expected_a[i] = records[i]->a;
    }

  /* All records and their borrowed strings must still be valid. */
  for (int i = 0; i < 64; i++)
    {
      g_autofree char *want_a = g_strdup_printf ("a-%d", i);
      g_autofree char *want_b = g_strdup_printf ("b-with-padding-%d", i);
      g_autofree char *want_c = g_strdup_printf ("c-quite-a-bit-longer-string-%d", i);
      g_assert_cmpstr (records[i]->a, ==, want_a);
      g_assert_cmpstr (records[i]->b, ==, want_b);
      g_assert_cmpstr (records[i]->c, ==, want_c);
      g_assert_true (records[i]->a == expected_a[i]);
    }

  mail_arena_destroy (&a);
}

static void
test_null_strdup_returns_null (void)
{
  MailArena a;
  mail_arena_init (&a, 16);
  g_assert_null (mail_arena_strdup (&a, NULL));
  g_assert_null (mail_arena_strndup (&a, NULL, 10));
  mail_arena_destroy (&a);
}

int
main (int argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/arena/init-zero", test_init_zero);
  g_test_add_func ("/arena/basic", test_basic_alloc);
  g_test_add_func ("/arena/alignment", test_alignment);
  g_test_add_func ("/arena/strdup", test_strdup);
  g_test_add_func ("/arena/grow", test_grow);
  g_test_add_func ("/arena/reset-preserves-capacity", test_reset_preserves_capacity);
  g_test_add_func ("/arena/zero-byte-null", test_zero_byte_alloc_returns_null);
  g_test_add_func ("/arena/null-strdup-null", test_null_strdup_returns_null);
  g_test_add_func ("/arena/pointer-stability-across-grow", test_pointer_stability_across_grow);
  g_test_add_func ("/arena/pointer-stability-struct-then-strdup", test_pointer_stability_struct_then_strdup);
  return g_test_run ();
}
