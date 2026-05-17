# GNOME Mail

A modern, native GNOME mail client. C + GTK4 + libadwaita, built on top
of `GNOME Online Accounts` for account discovery, with provider-specific
backends for Microsoft Graph and IMAP.

## Goal

Provide a fast, HIG-correct mail reader that fits the GNOME desktop the
same way Files, Calendar, or Contacts do — no Electron, no JavaScript,
no per-provider GUI quirks. Accounts come from the user's GOA session;
the app discovers them, talks to each provider through a small backend
vtable, and renders the result with stock libadwaita widgets.

The scope is deliberately narrow at this stage: browse the folders of
your configured accounts, read messages (raw RFC822 with an optional
decoded text/plain view), and nothing else. Sending, search, threading,
and rich rendering are explicit follow-ups, not held-back features.

## Principles

These guide every code-level decision in the repo. They're listed first
because they're how to evaluate a change, not just describe one.

- **Native GNOME, not a port.** `org.gnome.Mail` app id, GTK4 widgets,
  libadwaita layout primitives (`AdwOverlaySplitView`,
  `AdwNavigationView`, `AdwActionRow`, `AdwHeaderBar`). The HIG is the
  reference; if a behaviour clashes with the HIG, the HIG wins.

- **C, not Rust.** The codebase is plain C against GLib / GTK / GIO /
  libsoup / json-glib / GMime / libetpan. Autotools builds. No new
  language runtimes pulled in for convenience.

- **Memory reuse over malloc/free on hot paths.** Provider backends own
  a chunked arena (`MailArena`) plus persistent `GByteArray` / `GString`
  scratch buffers. Per-request parsing reuses them instead of
  allocating fresh. The arena is pointer-stable across grows — its
  blocks form a linked list and are never `realloc`'d, so callers can
  hold returned pointers across subsequent allocations within the same
  reset window.

- **Documented lifetime contracts at the seams.** Each `MailBackend`
  vtable entry documents how long the pointers it returns stay valid.
  Folder pointers live until the next `list_folders_async`; message
  pointers live until the next `list_messages_async`;
  `fetch_message_raw_async` must not invalidate either, because the
  message-list pane still holds prior message pointers. These
  contracts are pinned by tests, not just comments.

- **Standards over heuristics.** When the spec dictates behaviour (e.g.
  the text/plain alternative in `multipart/alternative` per RFC 2046
  §5.1.4), follow it exactly. Deviations need a written reason.

- **Every bug ships with a regression test.** A fix must include a test
  that fails without it and passes with it, verified in-session. The
  test suite is the project's memory of why the code looks the way it
  does.

- **Provider-agnostic core.** The UI talks only to `MailBackend`. New
  providers are new vtable implementations; the existing widgets don't
  know about HTTP, IMAP, or OAuth. A `mail-backend-fake` lives under
  `tests/` so regression tests run headless without network or GOA
  accounts.

- **Reuse the GNOME platform.** GMime for MIME, libsoup for HTTP,
  json-glib for JSON, libetpan for IMAP, GOA for OAuth refresh. We do
  not reinvent what the platform already maintains.

## Status

Early prototype. The initial step provides:

- A toggleable sidebar listing accounts from `GoaClient` and the
  folders inside each account, with provider icons.
- A right-hand pane that switches between a message list (root) and a
  message viewer (pushed via `AdwNavigationView`) for the selected
  message, with a header-bar toggle between raw RFC822 and the decoded
  text/plain alternative when one is available.
- Microsoft Graph backend (functional) and IMAP backend (compile-only
  stub) selected per-account via GOA's reported provider type.
- A `tests/` suite with arena, accounts, sidebar, backend-contract,
  message-list, and MIME extractor coverage running under
  `gtk_test_init`.

## Build

```sh
sudo dnf install gnome-online-accounts-devel libsoup3-devel \
                 json-glib-devel libetpan-devel libsecret-devel \
                 gtk4-devel libadwaita-devel gmime30-devel

./autogen.sh
mkdir -p build && cd build
../configure --prefix=$HOME/.local --enable-debug
make -j$(nproc)
make check
./src/mail
```

`make format` runs `clang-format` on the C tree and `xmllint` (2-space
GNOME style) on the XML / GtkBuilder files under `data/`.
