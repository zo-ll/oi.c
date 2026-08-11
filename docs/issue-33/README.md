# Reproducing the #33 measurements

The byte counts in `versioned-memory-design.md` were produced with the
repository's own test mock server. To re-run them:

1. Generate the six-turn session logs (requires Python 3):

   ```sh
   python3 docs/issue-33/measure_context.py /tmp/oi-s1
   ```

   This writes a valid binary sesslog (`"OISESLOG"` header, length-prefixed
   codec records) for a realistic six-turn bug-fix session with `shell` tool
   calls, plus a compacted variant where a checkpoint replaces turns 1-4.
   Records were validated against `oi_cli_history_store_load` and the replay
   state machine (25 context messages uncompacted, 10 compacted).

2. Measure the request bytes oi actually sends (needs a C compiler):

   ```sh
   cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
     -I test/integration docs/issue-33/measure_replay.c -o /tmp/measure_replay \
     -pthread -lssl -lcrypto
   cp /tmp/oi-s1/history.oilog /tmp/oi-s1/test.oilog
   cp /tmp/oi-s1/history-compacted.oilog /tmp/oi-s1/test-c.oilog
   touch /tmp/oi-fresh.oilog
   /tmp/measure_replay /tmp oi-fresh "what is the status?"   # fresh
   /tmp/measure_replay /tmp/oi-s1 test "what is the status?" # resume
   /tmp/measure_replay /tmp/oi-s1 test-c "what is the status?" # compacted
   ```

   Each run prints wire bytes, JSON body bytes, and dumps the request body to
   `/tmp/oi-body.json`. Use a fresh log per run — the CLI appends the turn it
   just sent, so repeating a run grows the session.

3. `--dry-run` is not a substitute: it resolves config and prints the request
   before the session is opened, so it cannot measure replayed context.
