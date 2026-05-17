/* mail-arena.h - Chunked bump allocator for per-operation transient data.
 *
 * One MailArena is owned by a MailBackend. It is reset (not freed)
 * between operations so subsequent calls reuse the chunks instead of
 * paying malloc/free per request. Chunks are allocated lazily and form
 * a singly-linked list; *individual chunks are never realloc'd*, which
 * guarantees pointer stability — a critical invariant for code paths
 * that build a struct with mail_arena_alloc and then fill in its
 * fields via further mail_arena_strdup calls in the same loop.
 *
 * Lifetime contract: every pointer returned by mail_arena_alloc /
 * mail_arena_strdup / mail_arena_strndup is valid until the next
 * mail_arena_reset on the same arena. Callers that need to keep a
 * value beyond that must copy it out first.
 */

#pragma once

#include <glib.h>
#include <stddef.h>
#include <stdint.h>

G_BEGIN_DECLS

typedef struct MailArenaChunk MailArenaChunk;

typedef struct
{
  MailArenaChunk *first;
  MailArenaChunk *current;
} MailArena;

/* Initialise an arena. If initial_cap is non-zero, one chunk of that
 * size is allocated up front; otherwise the first allocation lazily
 * creates a chunk. */
void mail_arena_init (MailArena *a,
                      size_t initial_cap);

/* Allocate `bytes` aligned to `align`. align must be a power of two,
 * and not exceed alignof(max_align_t) for our chunk header. */
void *mail_arena_alloc (MailArena *a,
                        size_t bytes,
                        size_t align);

/* Convenience wrappers, byte-aligned. */
char *mail_arena_strdup (MailArena *a,
                         const char *s);
char *mail_arena_strndup (MailArena *a,
                          const char *s,
                          size_t n);

/* Reset the arena: every chunk's used pointer goes back to zero. The
 * chunk list is preserved, so subsequent allocations reuse memory. */
void mail_arena_reset (MailArena *a);

/* Free every chunk. After this the arena is invalid. */
void mail_arena_destroy (MailArena *a);

/* Bytes currently in use across all chunks (since the last reset). */
size_t mail_arena_used (const MailArena *a);

/* Total backing capacity across all chunks. */
size_t mail_arena_capacity (const MailArena *a);

G_END_DECLS
