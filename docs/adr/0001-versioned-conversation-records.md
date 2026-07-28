# Persist conversations as versioned message records

Date: 2026-07-28

The CLI will persist the complete model-visible message sequence by appending
each completed message as a versioned JSON envelope inside the generic session
log; interrupted turns remain visible and unresolved interactions are repaired
explicitly on resume. Repair supplies an outcome-unknown result for every
unresolved tool call that may have started, or a not-executed result when it is
known not to have started, and then an assistant interruption marker. This
produces a protocol-valid completed prefix before new input is accepted. Legacy
alternating user/assistant records remain in place, with an appended
schema-transition marker separating them from typed records. Partial assistant
text already displayed before interruption is retained as audit content but
excluded from active context. A user message queued behind an active turn is
persisted immediately, but after process interruption it is restored as
editable input for confirmation rather than sent automatically. This preserves
evidence of potentially side-effecting tool activity, keeps the format
evolvable, and retains append-only compatibility without an in-place migration.
