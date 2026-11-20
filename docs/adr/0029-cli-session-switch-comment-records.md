# cli_session_switch: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_session_switch.c`, `src/cli_session_switch.h`, `test/integration/test_cli_session_switch.c`.

## `oi_cli_history_store_free` (src/cli_session_switch.c)

Never destroys `session`: on success the caller owns it, and on any other outcome the switch already rolled its registration back.

## `prepare_history` (src/cli_session_switch.c)

Appends the schema transition and any interrupted-turn repairs the replay asked for -- the same preparation every other session-open path performs, so a switched-to session is indistinguishable from one opened directly.

## `oi_cli_session_switch` (src/cli_session_switch.c)

Both "absent" and "present but not a plain directory" mean there is nothing here the user can switch to.

From here on, every failure must undo whatever it registered.

Running out of memory is structural, not a property of the target: reporting it as "corrupt" would blame a session that is perfectly fine and hide a real failure from the caller.

OI_ERR_EXISTS is the flock already being held -- either by another process, or by this one if the id is somehow already registered. Either way it is not switchable right now.

Captured before any append below, so it reflects whether this session had any pre-existing records at all -- the same convention every other open path uses to decide is_new_session.

An allocation failure here is structural; a durable-append failure means this session is not usable. Distinguish so the REPL does not end over a merely unusable target.

restore_settings chdir()s into the target's working directory. If it then fails, the caller keeps using the old session, so the process must not be left standing in the target's directory.

That rollback is only possible if the current directory is known, so a getcwd failure refuses the switch outright rather than proceeding into a chdir it could not undo. Better to decline a switch than to leave the old session running from an unknown directory.

## `oi_cli_session_switch.restored` (src/cli_session_switch.c)

Put the process back where the caller left it. If even that fails, the old session cannot safely continue -- it would be running from the target's directory -- so this stops being a recoverable business outcome and becomes a real error. Reporting it as merely "corrupt target" would tell the caller to carry on in a directory it did not choose.

## `oi_cli_session_switch.out_result` (src/cli_session_switch.c)

Committed. Hand everything over in one move; the source is re-inited rather than freed, since ownership transferred rather than ended.

## `oi_cli_session_switch_outcome` (src/cli_session_switch.h)

Why every business-logic failure is an outcome rather than a status: a /session switch that cannot happen must never end the REPL. Only a genuine structural failure (allocation) comes back as a non-OK oi_status; everything a user can provoke lands here instead.

## `oi_cli_session_switch_result` (src/cli_session_switch.h)

What a successful switch hands over. On OI_CLI_SESSION_SWITCH_OK the caller adopts every field: `session` is already open and registered, `store`/`state` are moved out and owned by the caller, and the remaining allocations are the caller's to release.

`session` is never owned by this struct -- on success the caller owns it, and on any other outcome it is NULL because the switch already rolled its own registration back.

A caller that takes ownership of a field must zero or re-init that field before calling oi_cli_session_switch_result_free, which frees whatever is still present. Calling free on a moved-from result is otherwise a double free.

## `oi_cli_session_switch_result.model_origin` (src/cli_session_switch.h)

Where `model` came from, for a caller that reports provenance. Never OI_CLI_SESSION_MODEL_EXPLICIT: a switch restores the target's own settings and deliberately passes no override.

## `oi_cli_session_switch` (src/cli_session_switch.h)

Opens, replays, repairs, and settings-restores the session `target_id` under the sessions root, without touching the currently-active session.

Purely additive to the registry until the outcome is OK: on every other outcome anything registered here has already been destroyed, so the registry is exactly as it was on entry. That is what lets a failed switch leave the original session active and usable -- this function never has the old session to damage in the first place.

Restoring the target's working directory chdir()s the process. If a later step then fails, the original working directory is restored before returning, so a failed switch does not leave the process sitting in the target's directory while the caller keeps using the old session.

That guarantee is upheld rather than attempted. If the current directory cannot be read up front, the switch is refused with OI_ERR_IO before any chdir happens, since a rollback would then be impossible. If the rollback chdir itself fails, the result is OI_ERR_IO too, not a recoverable outcome: the caller must not be told to carry on with the old session from a directory it never chose.

`default_model` and `default_cwd` must both be non-empty; they are the fallbacks used when neither the target's metadata nor its replayed history names one. `diagnostics` may be NULL. `current_session_id` may be NULL when no session is active.

Returns non-OK only for a structural failure. Check `out_result->outcome` on OI_OK.

## `file scope` (test/integration/test_cli_session_switch.c)

Exercises oi_cli_session_switch directly against a real registry, real logs, and a real second process holding a lock -- no REPL, no PTY, no mock server. The REPL-level wiring is covered separately in test_cli.c.

## `set_user_record` (test/integration/test_cli_session_switch.c)

Wraps a plain user or assistant message into a durable record, the same way the REPL's own persistence callbacks do.

## `seed_session` (test/integration/test_cli_session_switch.c)

Builds a session directory with a real log carrying `turns` completed user/assistant exchanges plus durable model and cwd settings.

## `switch_opens_replays_and_restores_the_target` (test/integration/test_cli_session_switch.c)

And the provenance restore_settings decided is propagated out, not recomputed or dropped: the seeded target carries a model setting record, so history is what won. A switch passes no override at all, so this can never be OI_CLI_SESSION_MODEL_EXPLICIT -- that is structural, not incidental, and the REPL adopts whatever arrives here as the newly-active model's story.

## `switch_rejects_unsafe_ids_and_missing_sessions` (test/integration/test_cli_session_switch.c)

A symlink standing in for a session directory is not switchable.

Bad arguments are the caller's bug, not a business outcome.

## `switch_to_a_session_held_elsewhere_reports_busy_and_registers_nothing.cwd_buf` (test/integration/test_cli_session_switch.c)

The child below frees the heap blocks of this test first: under valgrind it runs its own leak check at _exit, and a report there changes its exit status.

## `switch_to_a_session_held_elsewhere_reports_busy_and_registers_nothing` (test/integration/test_cli_session_switch.c)

Busy is an ordinary outcome, not an error that ends the REPL.

## `snprintf` (test/integration/test_cli_session_switch.c)

A record left incomplete by a crash is recovered, not rejected -- oi_sesslog_open truncates the fragment and everything before it is preserved, so the session is still switchable.

A log whose header is not an oi log at all cannot be opened, and is reported as corrupt rather than crashing or ending the REPL.

## `a_failed_switch_leaves_the_working_directory_alone` (test/integration/test_cli_session_switch.c)

Restoring a target's settings chdir()s the process. A switch that fails afterwards must not leave the caller -- which keeps using the old session -- standing in the target's directory.

Here the failure is provoked before any chdir (an undecodable log), which is the case that must clearly not move it.

## `a_switch_failing_after_the_target_cwd_is_applied_still_rolls_back` (test/integration/test_cli_session_switch.c)

A session with a transition but no durable settings and no metadata cache, so both model and cwd resolve to the defaults passed in.

A default model longer than a durable setting value may be. Settings restoration chdir()s into the resolved cwd *first* and only then appends the model record, so this fails strictly after the working directory has already moved -- the case the earlier rollback test, which fails before any chdir, does not reach.

## `oi_cli_session_location_init` (test/integration/test_cli_session_switch.c)

A session whose last turn has a user message and no reply -- exactly what a crash mid-turn leaves behind.

## `switch_repairs_an_interrupted_turn_in_the_target` (test/integration/test_cli_session_switch.c)

The interrupted turn was repaired durably during the switch, so the replay state no longer asks for it.

## `oi_cli_session_switch_result_init` (test/integration/test_cli_session_switch.c)

Reopening sees the repair already on disk rather than needing it again -- the switch's appends were durable, not in-memory.
