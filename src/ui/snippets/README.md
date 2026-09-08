# src/ui/snippets/

The TUs about the user's snippets folder (`%APPDATA%\EclipseConsole\snippets`):

- `tree` — the file tree: page-relative paths and their validation,
  `.order`, the directory scans behind the sidebar and the in-game panel.
- `trash` — delete-to-`.trash` and restore, with the flat-name index.
- `watch` — the poller that notices external edits and re-pushes.
- `bundle` — export/import of the tree (plus settings and the offsets
  conf) as one readable text file, for handing a setup to someone else.

None of these owns a global: every function takes the snippets directory from
the caller (`trash.h` states the pattern). The page-message producers
(`push_snippets`, `push_hud_snippets`) stay in `eclipse_ui.cpp` with
`ui_post`/`hud_push`; this folder is the filesystem layer under them.

**The rule that governs everything here:** that directory is the user's real
work — their snippets, their tuned parameters. Nothing in the app modifies it
outside the user's own actions: no sync, no reset, no rebuild step, no
migration. Deleting goes through `.trash`, never through unlink. If a change
you are making would touch those files without a user gesture behind it, the
change is wrong.
