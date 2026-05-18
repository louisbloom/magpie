# GNOME Mail

A modern, native GNOME mail client. C + GTK4 + libadwaita, built on top
of `GNOME Online Accounts` for account discovery, with provider-specific
backends for Microsoft Graph and IMAP.

## Goal

Provide a fast, HIG-correct mail reader that fits the GNOME desktop the
same way Files, Calendar, or Contacts do — no Electron, no JavaScript,
no per-provider GUI quirks. Accounts come from the user's GOA session;
the app discovers them, syncs each account's messages to a local
Maildir + sqlite index, and renders the result with stock libadwaita
widgets.

Storage is local-first (mutt / gnus-style): the UI always reads from
the on-disk store under `~/Mail/<identity>/`, and remote providers are
contacted only when the user explicitly triggers a sync from the
account page. The provider then reconciles its message list into the
store, pulling new bodies and pruning ones that have disappeared
upstream.

The scope is deliberately narrow at this stage: browse the folders of
your configured accounts, trigger and watch syncs, and read messages
(MIME-aware: HTML in WebKit by default, with toggles for the
text/plain alternative and the raw RFC822 source). Sending, search,
threading, and push notifications are explicit follow-ups, not
held-back features.

## Principles

These guide every code-level decision in the repo. They're listed first
because they're how to evaluate a change, not just describe one.

- **Native GNOME, not a port.** `org.gnome.Mail` app id, GTK4 widgets,
  libadwaita layout primitives (`AdwOverlaySplitView`,
  `AdwNavigationView`, `AdwHeaderBar`, `AdwWindowTitle`,
  `AdwStatusPage`). The HIG is the reference; if a behaviour clashes
  with the HIG, the HIG wins.

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
  know about HTTP, IMAP, or OAuth. The same vtable is implemented by
  `mail-backend-store` (UI-facing, reads from the local sqlite + Maildir
  store) and by the per-provider backends (`mail-backend-msgraph`,
  `mail-backend-imap`) consumed by `MailSync`. A `mail-backend-fake`
  lives under `tests/` so regression tests run headless without network
  or GOA accounts.

- **Reuse the GNOME platform.** GMime for MIME, libsoup for HTTP,
  json-glib for JSON, libetpan for IMAP, sqlite3 for the local index,
  GOA for OAuth refresh. We do not reinvent what the platform already
  maintains.

## Status

Early prototype. The current shape:

- **Sidebar.** Toggleable left pane listing accounts from `GoaClient`
  with provider icons; per-account folder rows below each header. Both
  account and folder rows are selectable navigation targets. Sidebar
  width is content-driven (pinned to the widest row) so it doesn't
  ballooon on widescreen displays.
- **Right pane.** Three pages routed through `AdwNavigationView`: the
  message list (a virtualizing `GtkListView` over the local store, so
  10k-row folders open instantly and scroll smoothly), the message
  viewer (pushed on row activation, with an `AdwToggleGroup` in the
  header bar exposing three exclusive view modes — _Rendered_ (HTML in
  a sandboxed `WebKitWebView` or the text/plain alternative, per
  RFC 2046 §5.1.4), _Plain_ (forced text/plain alternative; insensitive
  when none exists), and _Source_ (raw RFC822). Unsupported content
  types surface an `AdwStatusPage` placeholder instead of dumping
  binary), and the account page (shown when an account row is
  selected, or auto-switched-to when a sync starts on the current
  account).
- **Account page.** `AdwStatusPage`-based body with a "Sync now" button
  in the idle state; during a pass the same slot shows a centered
  progress ring, a live status line, a sliding-window ETA ("About 3
  minutes remaining"), and a Cancel button. The header bar carries an
  `AdwWindowTitle` with the account identity over the provider name.
- **Sync engine.** `MailSync` is a per-account one-shot reconciler:
  list folders → list messages per folder → fetch new bodies → upsert
  into the local store. Triggered manually from the account page; no
  startup sync, no timer. Pagination runs to completion (no per-pass
  message cap); a `messages.remote_id` UNIQUE constraint keeps repeat
  passes idempotent.
- **Local store.** `MailStore` owns a Maildir tree + sqlite index at
  `~/Mail/<identity>/`. The UI reads through `mail-backend-store` (a
  `MailBackend` implementation that wraps the store), so the
  rendering path never blocks on the network and the sync engine is
  the only thing that talks to providers.
- **Providers.** Microsoft Graph (functional, with `@odata.nextLink`
  pagination); IMAP via libetpan with SASL XOAUTH2 (Gmail tested),
  with cross-folder body deduplication keyed on the RFC 5322
  `Message-ID` header — a message in INBOX and `[Gmail]/All Mail`
  is fetched once and the Maildir bodies hardlink-share an inode.
  Fetches are batched (one `UID FETCH BODY.PEEK[]` per ~50 messages)
  and the GOA OAuth token is cached across the pass, so an
  initial-sync's per-message round-trip cost amortises into a
  near-constant overhead. Selection is per-account via
  GOA's reported provider type.
- **Tests.** Thirteen test binaries under `tests/`, running under
  `gtk_test_init` where they touch widgets: `test-arena`,
  `test-accounts`, `test-sidebar`, `test-backend-contract`,
  `test-message-list`, `test-message-view`, `test-mime`,
  `test-imap-id`, `test-store`, `test-backend-store`, `test-sync`,
  `test-eta`, `test-account-page`. Every bug fix lands with a
  regression test (see the principles).

## Build

```sh
sudo dnf install gnome-online-accounts-devel libsoup3-devel \
                 json-glib-devel libetpan-devel libsecret-devel \
                 gtk4-devel libadwaita-devel gmime30-devel \
                 sqlite-devel webkitgtk6.0-devel

./autogen.sh
mkdir -p build && cd build
../configure --prefix=$HOME/.local --enable-debug
make -j$(nproc)
make check
./src/mail
```

## Developer tooling

- `make format` — `clang-format` on the C tree, `xmllint` (2-space GNOME
  style) on the XML / GtkBuilder files under `data/`, and `prettier` on
  Markdown. Each pass is skipped with a notice if the tool isn't on
  `$PATH`, so a partial dev environment still works.
- `make bear` — clean rebuild under [bear](https://github.com/rizsotto/Bear),
  emits `compile_commands.json` at the project root for clangd. Builds
  tests too so the database covers everything under `src/` and
  `tests/`.

Optional dev tools:

```sh
sudo dnf install clang-tools-extra libxml2 bear
npm install -g prettier
```
