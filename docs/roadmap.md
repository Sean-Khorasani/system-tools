# WinTCP — Roadmap

**Created 2026-10-10** by splitting the open-work tracker (9.6.2) along its
one real fault line: **audience**. `todo.md` mixes product features a user
would ask about with code-perfection chores a maintainer does, and a reader
cannot tell them apart.

That fault line turned out to be real rather than assumed, so the split is a
separation rather than an arbitrary partition: **five items are open, and only
one of them is a user-facing feature.**

## Open

- **F5.5 — ETW DNS snooping.** Answer `host:` from the machine's own DNS
  traffic instead of a PTR query per peer, so the Host column populates without
  `--dns` slowness. Elevated only.

## Blocked, with the reason recorded

- **The GUI process-tree view.** The engine half (`ProcessTree`) and the
  `killtree` verb both shipped; what remains is the view — a tree control where
  a list view is. That is new chrome, so it needs a decision about what the
  tree shows rather than a build task. The engine it would sit on is tested.

## Deferred by user decision

- **W4.1 / 9.6.1 — `MainWindow.cpp` split.** Deferred 2026-10-04. (Both entries
  are the same item recorded in two places; the tracker's own rule says a second
  pointer is a second place to forget, so the roadmap keeps one line.)

## Not work

`todo.md` §6 "Carried risks" holds things that are neither features nor
defects — the deliberate sampler leak, the "never hold a row pointer across a
modal dialog" rule, the three build paths staying in step. They move with
`todo.md`, not here, because a roadmap is a list of what will change and those
are what must not.

## Recommended order

1. **F5.5** — the only user-facing item open. Blocked on a provider finding
   recorded in `todo.md` rather than on work: three probes on
   `Microsoft-Windows-DNS-Client` `{1C950233-…}`, elevated, with the provider
   enabling cleanly each time, delivered **zero events** on this machine. A
   parser written against an assumed schema would have compiled, passed its
   own tests, and silently never fired. Resuming needs a machine where the
   provider does deliver events.
2. **The GUI tree view**, once there is a decision on what it shows.
