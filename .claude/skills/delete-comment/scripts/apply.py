import json, re, sys, os, collections

API_PREFIX = "include/oi/"
API_DOC = "docs/API.md"
KEYWORDS = {"if", "for", "while", "return", "sizeof", "switch", "do", "else", "case",
            "CHECK", "CHECK_EQ", "CHECK_STREQ", "RUN", "memset", "memcpy", "free"}
GROUPS = [("reactor", "reactor"), ("llm_", None), ("cli_history", "cli_history"),
          ("cli_session_metadata", "cli_session_metadata"), ("cli_markdown", "cli_markdown"),
          ("cli_render", "cli_render"), ("json", "json"), ("tool_", "tool"),
          ("cli_utf8", "cli_utf8")]


def module_of(path):
    base = os.path.splitext(os.path.basename(path))[0]
    if base in ("test", "mock_api"):
        return "test_harness"
    for prefix in ("test_", "fuzz_"):
        if base.startswith(prefix):
            base = base[len(prefix):]
    base = {"http": "llm_http", "sse": "llm_sse"}.get(base, base)
    for prefix, name in GROUPS:
        if base.startswith(prefix) and name:
            return name
    return base


def symbol_of(lines, l1, l2, nxt):
    for pat in (r"^TEST\((\w+)\)", r"#define\s+(\w+)", r"^(?:struct|enum)\s+(\w+)\s*\{"):
        m = re.search(pat, nxt)
        if m:
            return m.group(1)
    m = re.match(r"^[\w\s\*]*?\b(\w+)\s*\(", nxt)
    if m and m.group(1) not in KEYWORDS:
        return m.group(1)
    m = re.match(r"^(?:const\s+)?[\w\s\*]+?\b(\w+)\s*(?:\[[^\]]*\])?\s*[;=,]", nxt)
    if m and m.group(1) not in KEYWORDS:
        field = m.group(1)
    else:
        field = None
    for i in range(l1 - 2, -1, -1):
        line = lines[i]
        m = re.match(r"^TEST\((\w+)\)", line) or re.match(r"^(?:static\s+)?[A-Za-z][\w\s\*]*?\b(\w+)\s*\(", line)
        if m and not line.rstrip().endswith(";") and m.group(1) not in KEYWORDS:
            return m.group(1) if field is None else m.group(1) + "." + field
        m = re.match(r"^(?:typedef\s+)?(?:struct|enum)\s+(\w+)", line)
        if m:
            return m.group(1) + ("." + field if field else "")
        if line.startswith("}"):
            break
    return field or "file scope"


def declared(lines, l2, trail, nxt):
    if trail:
        code = nxt.split('/*')[0].strip()
        return re.search(r'(\w+)\s*(?:\[[^\]]*\])?\s*[,;]?\s*$', code).group(1)
    i = l2
    while not lines[i].strip():
        i += 1
    head = lines[i].strip()
    m = re.match(r'typedef\s+[\w\s\*]*\(\s*\*\s*(\w+)\)', head)
    if m:
        return m.group(1)
    m = re.match(r'typedef\s+(?:struct|enum)\s+(\w+)\s+\w+\s*;', head)
    if m:
        return m.group(1)
    if re.match(r'(typedef\s+)?(struct|enum)\b[^;]*\{', head):
        name = re.match(r'(?:typedef\s+)?(?:struct|enum)\s+(\w+)', head)
        if name:
            return name.group(1)
        j = i + 1
        while not re.match(r'\s*\}', lines[j]):
            j += 1
        tail = re.match(r'\s*\}\s*(\w+)?', lines[j]).group(1)
        if tail:
            return tail
        first = next(l for l in lines[i + 1:] if l.strip() and not l.strip().startswith(('/*', '*')))
        return re.match(r'\s*(\w+)', first).group(1) + ' (enum)'
    m = re.match(r'#define\s+(\w+)', head)
    if m:
        return m.group(1)
    m = re.match(r'[\w\s\*]*?\b(\w+)\s*\(', head)
    return m.group(1)


def add_api_entry(path, symbol, text):
    doc = open(API_DOC).read()
    heading = "## `oi/%s`" % path[len(API_PREFIX):]
    entry = "### `%s`\n\n%s\n" % (symbol, text)
    if heading not in doc:
        doc = doc.rstrip() + "\n\n" + heading + "\n"
    start = doc.index(heading) + len(heading)
    nxt = doc.find("\n## ", start)
    end = len(doc) if nxt < 0 else nxt
    doc = doc[:end].rstrip() + "\n\n" + entry + doc[end:]
    open(API_DOC, "w").write(doc.rstrip() + "\n")


def prose(text):
    body = []
    for raw in text.split("\n"):
        s = re.sub(r"^\s*(/\*+|\*/|\*(?!/)|//)\s?", "", raw)
        s = s.replace("*/", "").rstrip()
        body.append(s)
    paras, cur = [], []
    for s in body:
        if not s.strip():
            if cur:
                paras.append(cur)
            cur = []
        elif cur and re.match(r"^\s*(- |OI_[A-Z_]+\s)", s):
            paras.append(cur)
            cur = [s.strip()]
        else:
            cur.append(s.strip())
    if cur:
        paras.append(cur)
    return "\n\n".join(" ".join(p) for p in paras).strip()


def main(classified, adr_dir, first_number, date):
    comments = json.load(open(classified))
    by_file = collections.defaultdict(list)
    for c in comments:
        by_file[c["file"]].append(c)
    records = collections.defaultdict(lambda: collections.OrderedDict())
    for path, cs in by_file.items():
        lines = open(path, encoding="utf-8").read().split("\n")
        for c in cs:
            c["keep"] = False
            if c["cat"] == "D" and path.startswith(API_PREFIX):
                add_api_entry(path, declared(lines, c["l2"], c["trail"], c["next"]), prose(c["text"]))
            elif c["cat"] == "D":
                key = (symbol_of(lines, c["l1"], c["l2"], c["next"]), path)
                records[module_of(path)].setdefault(key, []).append(prose(c["text"]))
    written = []
    for n, module in enumerate(sorted(records), start=first_number):
        files = sorted({p for (_, p) in records[module]})
        out = [f"# {module}: decision records from code comments", "", f"Date: {date}", "",
               "These records were code comments. The project does not allow code comments, "
               "so the records are here. Each heading names the symbol and the file that the "
               "record applies to. The record text is the original comment text.", "",
               "Source files: " + ", ".join(f"`{p}`" for p in files) + ".", ""]
        for (symbol, path), texts in records[module].items():
            out += [f"## `{symbol}` ({path})", ""]
            for t in texts:
                out += [t, ""]
        name = os.path.join(adr_dir, f"{n:04d}-{module.replace('_', '-')}-comment-records.md")
        open(name, "w").write("\n".join(out).rstrip() + "\n")
        written.append(name)
    for path, cs in by_file.items():
        delete_from(path, [c for c in cs if not c["keep"]])
    json.dump(comments, open(classified, "w"))
    print("\n".join(written))


def delete_from(path, cs):
    s = open(path, encoding="utf-8").read()
    starts = [0]
    for i, ch in enumerate(s):
        if ch == "\n":
            starts.append(i + 1)
    spans, used = [], set()
    for c in cs:
        a = s.index(c["text"], starts[c["l1"] - 1])
        while a in used:
            a = s.index(c["text"], a + 1)
        used.add(a)
        b = a + len(c["text"])
        ls = s.rfind("\n", 0, a) + 1
        le = s.find("\n", b)
        le = len(s) if le < 0 else le
        if s[ls:a].strip() == "" and s[b:le].strip() == "":
            spans.append((ls, min(le + 1, len(s))))
        elif s[b:le].strip() == "":
            k = a
            while k > ls and s[k - 1] in " \t":
                k -= 1
            spans.append((k, b))
        else:
            k = b
            if a > 0 and s[a - 1] in " \t(" and k < len(s) and s[k] == " ":
                k += 1
            spans.append((a, k))
    for a, b in sorted(spans, reverse=True):
        s = s[:a] + s[b:]
    s = re.sub(r"\n{3,}", "\n\n", s)
    s = re.sub(r"\{\n\n", "{\n", s)
    s = re.sub(r"\n\n(\s*\})", r"\n\1", s)
    open(path, "w", encoding="utf-8").write(s)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4])
