/* mail-arena.c - Chunked bump allocator implementation.
 *
 * Each chunk owns a flexibly-sized data array; chunks are never
 * realloc'd, so pointers handed out by mail_arena_alloc remain stable
 * across subsequent allocations. Chunks form a singly-linked list
 * starting at MailArena::first; the arena bumps in MailArena::current.
 * When current fills, allocation either spills into the next existing
 * chunk (left over from a prior pre-reset cycle) or appends a fresh
 * chunk.
 */

#include "mail-arena.h"

#include <string.h>

#define MAIL_ARENA_MIN_CAP 4096

struct MailArenaChunk
{
  MailArenaChunk *next;
  size_t cap;
  size_t used;
  /* Flexible-array data follows. Aligned at offset
   * sizeof(MailArenaChunk), which is at least alignof(size_t) and on
   * 64-bit Linux is 8 — sufficient for our uses (<=int64_t). */
  uint8_t data[];
};

static inline size_t
align_up (size_t v,
          size_t align)
{
  return (v + (align - 1)) & ~(align - 1);
}

static MailArenaChunk *
chunk_new (size_t cap)
{
  MailArenaChunk *c = g_malloc (sizeof (MailArenaChunk) + cap);
  c->next = NULL;
  c->cap = cap;
  c->used = 0;
  return c;
}

void
mail_arena_init (MailArena *a,
                 size_t initial_cap)
{
  a->first = NULL;
  a->current = NULL;
  if (initial_cap > 0)
    {
      a->first = chunk_new (initial_cap);
      a->current = a->first;
    }
}

void *
mail_arena_alloc (MailArena *a,
                  size_t bytes,
                  size_t align)
{
  if (bytes == 0)
    return NULL;
  g_assert (align > 0 && (align & (align - 1)) == 0);

  /* Try the current chunk. */
  if (a->current != NULL)
    {
      size_t start = align_up (a->current->used, align);
      size_t end = start + bytes;
      if (end <= a->current->cap)
        {
          void *p = a->current->data + start;
          a->current->used = end;
          return p;
        }
    }

  /* Current is full (or absent). If there's a reused chunk further
   * down the list, advance to it and try again. */
  if (a->current != NULL && a->current->next != NULL && bytes <= a->current->next->cap)
    {
      a->current = a->current->next;
      a->current->used = bytes;
      return a->current->data;
    }

  /* Need a fresh chunk. Double the previous capacity (or start from
   * MAIL_ARENA_MIN_CAP), and ensure it fits the requested allocation
   * including alignment padding. */
  size_t base_cap = a->current != NULL ? a->current->cap * 2 : MAIL_ARENA_MIN_CAP;
  if (base_cap < bytes + align)
    base_cap = bytes + align;
  MailArenaChunk *new_chunk = chunk_new (base_cap);

  if (a->first == NULL)
    {
      a->first = new_chunk;
    }
  else
    {
      /* Walk to the tail of the list (a->current may be in the middle
       * after a reset cycle). */
      MailArenaChunk *tail = a->current != NULL ? a->current : a->first;
      while (tail->next != NULL)
        tail = tail->next;
      tail->next = new_chunk;
    }
  a->current = new_chunk;

  /* New chunk: data starts at offset 0 within the chunk, already
   * aligned to alignof(size_t). For larger alignment, align inside. */
  size_t start = align_up (0, align);
  size_t end = start + bytes;
  void *p = new_chunk->data + start;
  new_chunk->used = end;
  return p;
}

char *
mail_arena_strdup (MailArena *a,
                   const char *s)
{
  if (s == NULL)
    return NULL;
  size_t n = strlen (s);
  char *out = mail_arena_alloc (a, n + 1, 1);
  memcpy (out, s, n);
  out[n] = '\0';
  return out;
}

char *
mail_arena_strndup (MailArena *a,
                    const char *s,
                    size_t n)
{
  if (s == NULL)
    return NULL;
  size_t actual = strnlen (s, n);
  char *out = mail_arena_alloc (a, actual + 1, 1);
  memcpy (out, s, actual);
  out[actual] = '\0';
  return out;
}

void
mail_arena_reset (MailArena *a)
{
  for (MailArenaChunk *c = a->first; c != NULL; c = c->next)
    c->used = 0;
  a->current = a->first;
}

void
mail_arena_destroy (MailArena *a)
{
  MailArenaChunk *c = a->first;
  while (c != NULL)
    {
      MailArenaChunk *next = c->next;
      g_free (c);
      c = next;
    }
  a->first = NULL;
  a->current = NULL;
}

size_t
mail_arena_used (const MailArena *a)
{
  size_t total = 0;
  for (MailArenaChunk *c = a->first; c != NULL; c = c->next)
    total += c->used;
  return total;
}

size_t
mail_arena_capacity (const MailArena *a)
{
  size_t total = 0;
  for (MailArenaChunk *c = a->first; c != NULL; c = c->next)
    total += c->cap;
  return total;
}
