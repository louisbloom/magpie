# Spool

Spool is a native GNOME mail client under the `bloom` umbrella —
alongside `bloom-boba`, `bloom-lisp`, `bloom-vt`, `bloom-terminal`,
and `bloom-telnet`. C + GTK4 + libadwaita, built on top of
`GNOME Online Accounts` for account discovery, with provider-specific
backends for Microsoft Graph and IMAP.

## For users

A fast, HIG-correct mail reader that fits the GNOME desktop the same
way Files, Calendar, or Contacts do — no Electron, no JavaScript, no
per-provider GUI quirks. Accounts come from your GOA session; Spool
discovers them, syncs each account's messages to a local Maildir +
sqlite index, and renders the result with stock libadwaita widgets.

### Your mail stays on disk, in a format other tools understand

Storage is local-first (mutt / gnus-style). Each account's messages
live under `~/Mail/<identity>/` as a regular Maildir tree, and Spool's
UI always reads from disk. Remote providers are contacted only when
you explicitly trigger a sync from the account page. The on-disk
Maildir is authoritative — point mutt or another Spool instance at
the same `~/Mail` and everything interoperates cleanly. If mutt marks
a message read while Spool is open, the boldness drops off the row in
real time; the reverse holds too.

### What you can do today

- **Browse folders.** A toggleable sidebar lists every account from
  `GoaClient` with provider icons, and the folders under each account.
  Sidebar width is pinned to the widest row so it doesn't balloon on
  widescreen displays.
- **Filter the message list.** A virtualizing list opens 10k-row
  folders instantly and scrolls smoothly. The header bar carries an
  unread-only toggle that hides read mail without reloading.
- **Read messages three ways.** Toggle group in the message-view header
  bar: _Rendered_ (HTML in a sandboxed WebKit view, or the text/plain
  alternative per RFC 2046 §5.1.4), _Plain_ (forced text/plain
  alternative; greyed out when none exists), and _Source_ (raw
  RFC 5322). Unsupported content types surface an explanatory placeholder
  instead of dumping binary.
- **Reply.** A Reply button in the message-view header opens a compose
  dialog pre-filled Gnus-style: attribution line plus `> `-quoted
  text/plain body (or HTML→text fallback when no plain alternative
  exists).
- **Trigger a sync.** Click an account row to see its sync page —
  "Sync now" in the idle state, a centered progress ring with a live
  status line, a sliding-window ETA ("About 3 minutes remaining"), and
  a Cancel button while a pass is running.
- **Live disk updates.** Reading a mail in Spool, or marking it read
  in a parallel mutt session, both flip the sidebar unread badge and
  the row boldness in the same frame. A per-account watcher with a
  120 ms debounce reconciles disk → sqlite drift in real time and at
  startup, so changes accumulated while Spool was closed are picked
  up before you click anything.

Providers covered: Microsoft Graph (Outlook / Office 365) and IMAP via
SASL XOAUTH2 (Gmail tested). Selection is automatic per-account based
on what GOA reports.

### What's not in yet

Spool is an early prototype. SMTP send is not wired — the Send button
in the compose dialog appends to a debug `Outbox.mbox` in the account
root so you can read your draft back with mutt or `less`. Search,
threading, and push notifications are explicit follow-ups, not
held-back features.

### Install and run

Spool ships as source. On Fedora:

```sh
sudo dnf install gnome-online-accounts-devel libsoup3-devel \
                 json-glib-devel libetpan-devel libxml2-devel \
                 gtk4-devel libadwaita-devel gmime30-devel \
                 sqlite-devel webkitgtk6.0-devel

./autogen.sh
mkdir -p build && cd build
../configure --prefix=$HOME/.local --enable-debug
make -j$(nproc)
make install
spool
```

Configure your mail accounts in GNOME Settings → Online Accounts
first; Spool picks them up automatically.

## For contributors

### Principles

These guide every code-level decision in the repo. They're listed first
because they're how to evaluate a change, not just describe one.

- **Charm.land identity, bloom umbrella.** The project name, CLI
  binary, and documentation voice follow charm.land's short,
  function-rooted naming aesthetic, and sit alongside the rest of
  the `bloom` family (`bloom-boba`, `bloom-lisp`, `bloom-vt`,
  `bloom-terminal`, `bloom-telnet`). The visible UI is **not**
  charm.land — it follows the GNOME HIG exactly, per the next
  bullet. When the two pull in different directions, the HIG wins.

- **Native GNOME, not a port.** `org.gnome.Spool` app id, GTK4 widgets,
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

- **Maildir is the source of truth, sqlite is an optimization.** Every
  state change writes the on-disk Maildir first and reflects to sqlite
  second. sqlite earns its place as an indexed cache for message-list
  rendering and unread counts, not as a second authority. When the two
  disagree, disk wins — a `MailMaildirWatcher` per account reconciles
  drift in real time and at startup.

- **Reuse the GNOME platform.** GMime for MIME, libsoup for HTTP,
  json-glib for JSON, libetpan for IMAP, sqlite3 for the local index,
  GOA for OAuth refresh. We do not reinvent what the platform already
  maintains.

### Architecture

- **Sync engine.** `MailSync` is a per-account one-shot reconciler:
  list folders → list messages per folder → fetch new bodies → upsert
  into the local store. Triggered manually from the account page; no
  startup sync, no timer. Pagination runs to completion (no per-pass
  message cap); a `messages.remote_id` UNIQUE constraint keeps repeat
  passes idempotent.
- **Local store.** `MailStore` owns a Maildir tree + sqlite index at
  `~/Mail/<identity>/`. The Maildir is authoritative; sqlite is an
  indexed cache over it. The UI reads through `mail-backend-store` (a
  `MailBackend` implementation that wraps the store), so the rendering
  path never blocks on the network and the sync engine is the only
  thing that talks to providers.
- **Disk → UI change-notification spine.** A per-account
  `MailMaildirWatcher` arms one `GFileMonitor` per folder's `cur/`
  with a 120 ms debounce; watcher events run the disk → sqlite
  reconciler, which emits the same events as the local mark-read
  path. The reconciler also runs once at startup so drift accumulated
  while Spool was closed is corrected before the user clicks anything.
- **Providers.** Microsoft Graph (functional, with `@odata.nextLink`
  pagination); IMAP via libetpan with SASL XOAUTH2 (Gmail tested),
  with cross-folder body deduplication keyed on the RFC 5322
  `Message-ID` header — a message in INBOX and `[Gmail]/All Mail` is
  fetched once and the Maildir bodies hardlink-share an inode. Fetches
  are batched (one `UID FETCH BODY.PEEK[]` per ~50 messages) and the
  GOA OAuth token is cached across the pass, so an initial-sync's
  per-message round-trip cost amortises into a near-constant overhead.

### Build from source

```sh
sudo dnf install gnome-online-accounts-devel libsoup3-devel \
                 json-glib-devel libetpan-devel libxml2-devel \
                 gtk4-devel libadwaita-devel gmime30-devel \
                 sqlite-devel webkitgtk6.0-devel

./autogen.sh
mkdir -p build && cd build
../configure --prefix=$HOME/.local --enable-debug
make -j$(nproc)
make check
./src/spool
```

### Tests

Twenty-one test binaries under `tests/`, running under `gtk_test_init`
where they touch widgets: `test-about`, `test-account-page`,
`test-accounts`, `test-arena`, `test-backend-contract`,
`test-backend-store`, `test-compose-window`, `test-eta`,
`test-html-to-text`, `test-imap-id`, `test-imap-retry`,
`test-maildir-watcher`, `test-message-list`, `test-message-view`,
`test-mime`, `test-outbox`, `test-quote`, `test-sidebar`, `test-store`,
`test-sync`, `test-window`. Every bug fix lands with a regression test
(see the principles).

### Developer tooling

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
