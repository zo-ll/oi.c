import json, os, sys

def msg(rid, turn, role, content, **kw):
    d = {"version":1,"record_id":str(rid),"turn_id":str(turn),"type":"message",
         "role":role,"content":content,"source":"normal"}
    d.update(kw)
    return d

# A representative bug-fix session: 6 turns, tool calls, file outputs.
turns = [
    ("Where is the epoll registration for timerfd in reactor_epoll.c?",
     ["grep -n 'timerfd' src/reactor_epoll.c"],
     ["src/reactor_epoll.c:214: static int register_timer(oi_reactor *r, struct oi_timer *t)\nsrc/reactor_epoll.c:231:         epoll_ctl(r->epoll_fd, EPOLL_CTL_ADD, t->fd, &event)\n"],
     "The timer registration lives at reactor_epoll.c:214, added via epoll_ctl at line 231."),
    ("Why does the timer callback never fire after the second registration?",
     ["sed -n '200,260p' src/reactor_epoll.c"],
     ["static int register_timer(oi_reactor *r, struct oi_timer *t) {\n    struct epoll_event event;\n    memset(&event, 0, sizeof event);\n    event.events = EPOLLIN;\n    event.data.ptr = t;\n    return epoll_ctl(r->epoll_fd, EPOLL_CTL_ADD, t->fd, &event);\n}\n"],
     "The second EPOLL_CTL_ADD fails with EEXIST because the fd was never removed; use EPOLL_CTL_MOD for re-registration."),
    ("Fix it and add a regression test.",
     ["sed -n '1,80p' test/test_reactor.c"],
     ["static void test_timer_rearm(void) {\n    oi_reactor *r = oi_reactor_create();\n    struct oi_timer t;\n    oi_timer_init(&t, 10);\n    CHECK_EQ(oi_reactor_add_timer(r, &t), OI_OK);\n}\n"],
     "Changed the second registration to EPOLL_CTL_MOD in register_timer and added test_timer_rearm."),
    ("The test passes locally but fails under TSan.",
     ["make tsan 2>&1 | tail -20"],
     ["WARNING: ThreadSanitizer: data race on reactor->polling\n  Read at 0x7f.. by thread T1\n  Previous write at 0x7f.. by main thread\n"],
     "TSan exposed a data race on the polling flag: the test writes it from a signal handler. Move the flag update into the reactor loop."),
    ("Apply the race fix.",
     ["sed -n '120,180p' src/reactor_epoll.c"],
     ["static void handle_signal(void) {\n    reactor->polling = 0;\n}\n"],
     "Moved the flag write into the loop's own signal handling path so only the reactor thread touches it."),
    ("Run the full suite and commit.",
     ["make -j24 check 2>&1 | tail -5", "git log --oneline -3"],
     ["== build/test_reactor ==\n12 test blocks, 0 assertion failures\nmake check: all green", "9cfc3c5 Track header dependencies\n83314c0 Prove every REPL exit path restores the terminal"],
     "Full suite is green; committed as 'Fix timer re-registration race'."),
]

records = []
rid = 1
records.append({"version":1,"record_id":"1","turn_id":"0","type":"transition","legacy_record_count":"0"})
rid = 2
turn = 1
for q, cmds, outputs, final in turns:
    records.append(msg(rid, turn, "user", q)); rid += 1
    calls = []
    for i, c in enumerate(cmds):
        calls.append({"id":"call_%d_%d"%(turn,i+1),"name":"shell","arguments":json.dumps({"command":c})})
    records.append(msg(rid, turn, "assistant", "", tool_calls=calls, model="gpt-4o-mini")); rid += 1
    for i, (c, o) in enumerate(zip(cmds, outputs)):
        import base64
        records.append({"version":1,"record_id":str(rid),"turn_id":str(turn),
                        "type":"tool_started","tool_call_id":"call_%d_%d"%(turn,i+1)}); rid += 1
        records.append(msg(rid, turn, "tool", o, tool_call_id="call_%d_%d"%(turn,i+1),
                           tool_outcome="completed",
                           raw_output_base64=base64.b64encode(o.encode()).decode())); rid += 1
    records.append(msg(rid, turn, "assistant", final, model="gpt-4o-mini")); rid += 1
    turn += 1

def write_sesslog(path, recs):
    with open(path, "wb") as f:
        f.write(b"OISESLOG" + (1).to_bytes(4, "little"))
        for r in recs:
            payload = json.dumps(r).encode()
            f.write(len(payload).to_bytes(4, "little") + payload)

os.makedirs(sys.argv[1], exist_ok=True)
write_sesslog(os.path.join(sys.argv[1], "history.oilog"), records)
print("wrote %d records" % len(records))

# Compacted variant: checkpoint replacing turns 1-4, kept records renumbered.
prefix = [r for r in records if int(r.get("record_id", 0)) <= 21]
compacted = prefix + [{"version":1,"record_id":"22","turn_id":"0","type":"checkpoint",
              "summary":"User is fixing a timerfd re-registration bug in reactor_epoll.c. "
                        "Registration was found at line 214; the second EPOLL_CTL_ADD failed with EEXIST "
                        "because the fd was never removed. register_timer was changed to use EPOLL_CTL_MOD, "
                        "a regression test was added, and TSan then exposed a data race on the polling flag "
                        "which was moved into the reactor loop's own signal handling. The fix is applied; "
                        "the suite is green locally.",
              "model":"gpt-4o-mini","source_first_record_id":"1","source_last_record_id":"21"}]
# keep records with record_id > 21 (turns 5-6), renumber from 23
keep = [r for r in records if int(r.get("record_id", 0)) > 21]
for i, r in enumerate(keep):
    r = dict(r)
    r["record_id"] = str(i + 23)
    compacted.append(r)
write_sesslog(os.path.join(sys.argv[1], "history-compacted.oilog"), compacted)
print("compacted: %d records" % len(compacted))
