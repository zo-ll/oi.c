# Store sessions privately per user

Date: 2026-07-28

Interactive sessions will live by default in the platform’s per-user state
location as one private directory per session. Each directory contains an
authoritative `history.oilog` and an atomically replaced, rebuildable
`metadata.json` used for fast selection; derived indexes may live beside them
without becoming authoritative. Each session stores and restores the working
directory in which its tools operate. Existing project-local `.oilog` files are
imported only through an explicit selective prompt that records the current
directory as their working directory. This makes sessions discoverable across
invocations without silently changing tool scope, exposing records to other
users, or moving legacy data behind the user’s back. Interactive deletion moves
session directories into a private internal trash that supports restore and
explicit permanent deletion; it is never automatically purged.
