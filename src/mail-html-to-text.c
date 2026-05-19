/* mail-html-to-text.c - libxml2-backed HTML → plain-text converter.
 *
 * See mail-html-to-text.h for the contract. Internals:
 *
 *   walk(node, ctx)
 *     ├─ TEXT          → flush_break() + collapse-whitespace-emit
 *     ├─ skipped tag   → return (script/style/head/noscript)
 *     ├─ <br>          → request_break(1)
 *     ├─ <hr>          → request_break(2); emit "---"; request_break(2)
 *     ├─ <blockquote>  → recurse into a child GString, then prefix
 *     │                  every emitted line with "> " before flushing
 *     │                  to the parent (so existing quote-builder
 *     │                  stacks correctly)
 *     ├─ <a href=X>    → walk children; if collected text != X, append
 *     │                  " [X]"
 *     ├─ block tag     → request_break(2); recurse; request_break(2)
 *     └─ inline tag    → recurse
 *
 * pending_break_ is the deferred line-break state: 0 = none, 1 =
 * single newline, 2 = blank-line separator. Requests merge by max(),
 * so adjacent block elements don't produce 4 newlines.
 */

#include "config.h"

#include "mail-html-to-text.h"

#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <string.h>

typedef struct
{
  GString *out;
  int pending_break;      /* 0, 1, or 2 (clamped) */
  gboolean at_line_start; /* TRUE if next char would be column 0 */
} Ctx;

static void walk (xmlNode *node, Ctx *ctx);

static void
request_break (Ctx *ctx, int level)
{
  if (level > ctx->pending_break)
    ctx->pending_break = level;
}

static void
flush_break (Ctx *ctx)
{
  if (ctx->pending_break == 0)
    return;
  if (ctx->out->len == 0)
    {
      /* Don't emit leading newlines for a document that starts with a
       * block element — visually pointless. */
      ctx->pending_break = 0;
      ctx->at_line_start = TRUE;
      return;
    }
  for (int i = 0; i < ctx->pending_break; i++)
    g_string_append_c (ctx->out, '\n');
  ctx->pending_break = 0;
  ctx->at_line_start = TRUE;
}

static gboolean
tag_eq (const xmlNode *n, const char *name)
{
  return n->name != NULL && g_ascii_strcasecmp ((const char *) n->name, name) == 0;
}

static gboolean
is_block (const xmlNode *n)
{
  static const char *const blocks[] = {
    "p",
    "div",
    "h1",
    "h2",
    "h3",
    "h4",
    "h5",
    "h6",
    "li",
    "ul",
    "ol",
    "tr",
    "table",
    "pre",
    "section",
    "article",
    "header",
    "footer",
    "nav",
    "aside",
    "main",
    "figure",
    "figcaption",
    "dl",
    "dd",
    "dt",
    NULL,
  };
  for (int i = 0; blocks[i] != NULL; i++)
    if (tag_eq (n, blocks[i]))
      return TRUE;
  return FALSE;
}

static gboolean
is_skipped (const xmlNode *n)
{
  return tag_eq (n, "script") || tag_eq (n, "style") || tag_eq (n, "head") || tag_eq (n, "noscript") || tag_eq (n, "template");
}

/* Emit a TEXT node: collapse runs of whitespace (incl. newlines) to a
 * single space; trim leading space when at line start. */
static void
emit_text (Ctx *ctx, const xmlChar *text)
{
  if (text == NULL)
    return;
  flush_break (ctx);

  gboolean in_ws = ctx->at_line_start; /* swallow leading WS at BOL */
  for (const xmlChar *p = text; *p != '\0'; p++)
    {
      unsigned char c = *p;
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
        {
          in_ws = TRUE;
        }
      else
        {
          if (in_ws && ctx->out->len > 0 && !ctx->at_line_start)
            g_string_append_c (ctx->out, ' ');
          g_string_append_c (ctx->out, (char) c);
          in_ws = FALSE;
          ctx->at_line_start = FALSE;
        }
    }
}

/* Walk children of `parent` into a fresh GString, then prefix every
 * line with "> " and flush into ctx->out. Used for <blockquote>. */
static void
walk_blockquote (xmlNode *parent, Ctx *parent_ctx)
{
  GString *inner = g_string_new (NULL);
  Ctx child_ctx = {
    .out = inner,
    .pending_break = 0,
    .at_line_start = TRUE,
  };
  for (xmlNode *c = parent->children; c != NULL; c = c->next)
    walk (c, &child_ctx);

  /* Drop trailing whitespace / blank lines in the child. */
  while (inner->len > 0 && (inner->str[inner->len - 1] == '\n' || inner->str[inner->len - 1] == ' '))
    g_string_truncate (inner, inner->len - 1);

  if (inner->len == 0)
    {
      g_string_free (inner, TRUE);
      return;
    }

  request_break (parent_ctx, 2);
  flush_break (parent_ctx);

  /* Prefix each line. */
  const char *p = inner->str;
  const char *end = inner->str + inner->len;
  while (p < end)
    {
      const char *nl = memchr (p, '\n', (size_t) (end - p));
      const char *line_end = nl != NULL ? nl : end;
      g_string_append (parent_ctx->out, "> ");
      g_string_append_len (parent_ctx->out, p, (gssize) (line_end - p));
      g_string_append_c (parent_ctx->out, '\n');
      p = nl != NULL ? nl + 1 : end;
    }

  g_string_free (inner, TRUE);
  parent_ctx->at_line_start = TRUE;
  request_break (parent_ctx, 2);
}

static void
walk_anchor (xmlNode *n, Ctx *ctx)
{
  g_autofree char *href = (char *) xmlGetProp (n, (const xmlChar *) "href");

  /* Capture the anchor's text in a side buffer so we can compare it
   * to the href. */
  GString *text_buf = g_string_new (NULL);
  Ctx child_ctx = {
    .out = text_buf,
    .pending_break = 0,
    .at_line_start = TRUE,
  };
  for (xmlNode *c = n->children; c != NULL; c = c->next)
    walk (c, &child_ctx);

  flush_break (ctx);
  g_string_append (ctx->out, text_buf->str);
  ctx->at_line_start = text_buf->len == 0;

  /* Add the URL if it adds information (and isn't a #fragment or
   * javascript: noise). */
  if (href != NULL && href[0] != '\0' && href[0] != '#' && g_ascii_strncasecmp (href, "javascript:", 11) != 0)
    {
      if (text_buf->len == 0 || strcmp (href, text_buf->str) != 0)
        g_string_append_printf (ctx->out, " [%s]", href);
    }

  g_string_free (text_buf, TRUE);
}

static void
walk (xmlNode *node, Ctx *ctx)
{
  if (node == NULL)
    return;

  if (node->type == XML_TEXT_NODE || node->type == XML_CDATA_SECTION_NODE)
    {
      emit_text (ctx, node->content);
      return;
    }
  if (node->type != XML_ELEMENT_NODE)
    {
      /* Comments, PIs, etc. — ignore. */
      return;
    }

  if (is_skipped (node))
    return;

  if (tag_eq (node, "br"))
    {
      request_break (ctx, 1);
      return;
    }
  if (tag_eq (node, "hr"))
    {
      request_break (ctx, 2);
      flush_break (ctx);
      g_string_append (ctx->out, "---");
      ctx->at_line_start = FALSE;
      request_break (ctx, 2);
      return;
    }
  if (tag_eq (node, "blockquote"))
    {
      walk_blockquote (node, ctx);
      return;
    }
  if (tag_eq (node, "a"))
    {
      walk_anchor (node, ctx);
      return;
    }

  gboolean block = is_block (node);
  if (block)
    request_break (ctx, 2);

  for (xmlNode *c = node->children; c != NULL; c = c->next)
    walk (c, ctx);

  if (block)
    request_break (ctx, 2);
}

/* Collapse runs of 3+ newlines down to 2 (one blank line). Strip
 * trailing whitespace on each line. */
static void
post_clean (GString *s)
{
  /* Trim trailing whitespace on each line. */
  GString *out = g_string_sized_new (s->len);
  const char *p = s->str;
  const char *end = s->str + s->len;
  while (p < end)
    {
      const char *nl = memchr (p, '\n', (size_t) (end - p));
      const char *line_end = nl != NULL ? nl : end;
      const char *trim = line_end;
      while (trim > p && (trim[-1] == ' ' || trim[-1] == '\t'))
        trim--;
      g_string_append_len (out, p, (gssize) (trim - p));
      if (nl != NULL)
        g_string_append_c (out, '\n');
      p = nl != NULL ? nl + 1 : end;
    }

  /* Collapse runs of 3+ '\n' into 2. */
  g_string_truncate (s, 0);
  int run = 0;
  for (gsize i = 0; i < out->len; i++)
    {
      char c = out->str[i];
      if (c == '\n')
        {
          run++;
          if (run <= 2)
            g_string_append_c (s, c);
        }
      else
        {
          run = 0;
          g_string_append_c (s, c);
        }
    }

  /* Trim leading and trailing newlines. */
  gsize start = 0;
  while (start < s->len && s->str[start] == '\n')
    start++;
  if (start > 0)
    g_string_erase (s, 0, (gssize) start);
  while (s->len > 0 && s->str[s->len - 1] == '\n')
    g_string_truncate (s, s->len - 1);

  g_string_free (out, TRUE);
}

gchar *
mail_html_to_text (const char *html, gssize len)
{
  if (html == NULL)
    return NULL;
  if (len < 0)
    len = (gssize) strlen (html);
  if (len == 0)
    return g_strdup ("");

  /* htmlReadMemory with recovery + silent errors + no network. UTF-8
   * is the only encoding we promise to handle — incoming text/html
   * parts go through GMime's charset conversion first. */
  htmlDocPtr doc = htmlReadMemory (html, (int) len, NULL, "utf-8",
                                   HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_NONET | HTML_PARSE_NOIMPLIED);
  if (doc == NULL)
    return NULL;

  GString *out = g_string_new (NULL);
  Ctx ctx = {
    .out = out,
    .pending_break = 0,
    .at_line_start = TRUE,
  };
  xmlNode *root = xmlDocGetRootElement (doc);
  for (xmlNode *c = root; c != NULL; c = c->next)
    walk (c, &ctx);

  xmlFreeDoc (doc);

  post_clean (out);

  return g_string_free (out, FALSE);
}
