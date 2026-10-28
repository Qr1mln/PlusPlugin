// json_util.cpp - implementation of the shared minimal JSON reader/writer.
//
// Keep this file pure ASCII.
#include "json_util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ppjson {

namespace {

const Node* NodeOf(const Pool* pool, int idx) {
    return (pool != nullptr && idx >= 0 && idx < pool->cap) ? &pool->nodes[idx] : nullptr;
}

void CopyStr(char* dst, int cap, const char* src) {
    if (cap <= 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    std::strncpy(dst, src, static_cast<size_t>(cap - 1));
    dst[cap - 1] = '\0';
}

// Append a child index to a parent node's child list (source order).
void LinkChild(Pool& p, int parentIdx, int childIdx) {
    Node& parent = p.nodes[parentIdx];
    p.nodes[childIdx].next = -1;
    if (parent.firstChild < 0) {
        parent.firstChild = childIdx;
        return;
    }
    int last = parent.firstChild;
    while (p.nodes[last].next >= 0) {
        last = p.nodes[last].next;
    }
    p.nodes[last].next = childIdx;
}

int NewScalar(Pool& p, Value parent, const char* key, Type t) {
    if (!parent.Valid()) {
        return -1;
    }
    const int ci = p.Alloc();
    if (ci < 0) {
        return -1;
    }
    Node& c = p.nodes[ci];
    c.type = t;
    c.b = false;
    c.num = 0.0;
    c.str[0] = '\0';
    c.key[0] = '\0';
    c.firstChild = -1;
    c.next = -1;
    if (key != nullptr) {
        CopyStr(c.key, kMaxKey, key);
    }
    LinkChild(p, parent.Index(), ci);
    return ci;
}

}  // namespace

// ---------------------------------------------------------------------------
// Accessors.
// ---------------------------------------------------------------------------
Type Value::type() const {
    const Node* n = NodeOf(pool_, idx_);
    return (n != nullptr) ? n->type : kNull;
}

bool Value::AsBool(bool def) const {
    const Node* n = NodeOf(pool_, idx_);
    return (n != nullptr && n->type == kBool) ? n->b : def;
}

double Value::AsNum(double def) const {
    const Node* n = NodeOf(pool_, idx_);
    return (n != nullptr && n->type == kNum) ? n->num : def;
}

long long Value::AsInt(long long def) const {
    const Node* n = NodeOf(pool_, idx_);
    return (n != nullptr && n->type == kNum) ? static_cast<long long>(n->num) : def;
}

const char* Value::AsStr() const {
    const Node* n = NodeOf(pool_, idx_);
    return (n != nullptr && n->type == kStr) ? n->str : "";
}

Value Value::Find(const char* key) const {
    const Node* n = NodeOf(pool_, idx_);
    if (n == nullptr || n->type != kObj || key == nullptr) {
        return Value();
    }
    for (int c = n->firstChild; c >= 0; c = pool_->nodes[c].next) {
        if (std::strcmp(pool_->nodes[c].key, key) == 0) {
            return Value(pool_, c);
        }
    }
    return Value();
}

Value Value::At(int i) const {
    const Node* n = NodeOf(pool_, idx_);
    if (n == nullptr || n->type != kArr || i < 0) {
        return Value();
    }
    int c = n->firstChild;
    for (int k = 0; c >= 0 && k < i; ++k) {
        c = pool_->nodes[c].next;
    }
    return (c >= 0) ? Value(pool_, c) : Value();
}

int Value::Size() const {
    const Node* n = NodeOf(pool_, idx_);
    if (n == nullptr || n->type != kArr) {
        return 0;
    }
    int count = 0;
    for (int c = n->firstChild; c >= 0; c = pool_->nodes[c].next) {
        ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Builders.
// ---------------------------------------------------------------------------
Value MakeObj(Pool& p) {
    const int i = p.Alloc();
    if (i < 0) {
        return Value();
    }
    Node& n = p.nodes[i];
    n.type = kObj;
    n.b = false;
    n.num = 0.0;
    n.str[0] = '\0';
    n.key[0] = '\0';
    n.firstChild = -1;
    n.next = -1;
    return Value(&p, i);
}

Value MakeArr(Pool& p) {
    const int i = p.Alloc();
    if (i < 0) {
        return Value();
    }
    Node& n = p.nodes[i];
    n.type = kArr;
    n.b = false;
    n.num = 0.0;
    n.str[0] = '\0';
    n.key[0] = '\0';
    n.firstChild = -1;
    n.next = -1;
    return Value(&p, i);
}

void ObjSetStr(Pool& p, Value obj, const char* key, const char* val) {
    const int ci = NewScalar(p, obj, key, kStr);
    if (ci >= 0) {
        CopyStr(p.nodes[ci].str, kMaxStr, val);
    }
}

void ObjSetNum(Pool& p, Value obj, const char* key, double val) {
    const int ci = NewScalar(p, obj, key, kNum);
    if (ci >= 0) {
        p.nodes[ci].num = val;
    }
}

void ObjSetBool(Pool& p, Value obj, const char* key, bool val) {
    const int ci = NewScalar(p, obj, key, kBool);
    if (ci >= 0) {
        p.nodes[ci].b = val;
    }
}

void ObjSetValue(Pool& p, Value obj, const char* key, Value child) {
    if (!obj.Valid() || !child.Valid() || key == nullptr) {
        return;
    }
    CopyStr(p.nodes[child.Index()].key, kMaxKey, key);
    LinkChild(p, obj.Index(), child.Index());
}

void ArrAddStr(Pool& p, Value arr, const char* val) {
    const int ci = NewScalar(p, arr, nullptr, kStr);
    if (ci >= 0) {
        CopyStr(p.nodes[ci].str, kMaxStr, val);
    }
}

void ArrAddNum(Pool& p, Value arr, double val) {
    const int ci = NewScalar(p, arr, nullptr, kNum);
    if (ci >= 0) {
        p.nodes[ci].num = val;
    }
}

void ArrAddBool(Pool& p, Value arr, bool val) {
    const int ci = NewScalar(p, arr, nullptr, kBool);
    if (ci >= 0) {
        p.nodes[ci].b = val;
    }
}

void ArrAddValue(Pool& p, Value arr, Value child) {
    if (!arr.Valid() || !child.Valid()) {
        return;
    }
    p.nodes[child.Index()].key[0] = '\0';
    LinkChild(p, arr.Index(), child.Index());
}

// Convenience member builders: bind the pool carried by the handle.
Value Value::Obj(Pool& p) { return MakeObj(p); }
Value Value::Arr(Pool& p) { return MakeArr(p); }

Value Value::Str(Pool& p, const char* val) {
    const int i = p.Alloc();
    if (i < 0) {
        return Value();
    }
    Node& n = p.nodes[i];
    n.type = kStr;
    n.b = false;
    n.num = 0.0;
    n.firstChild = -1;
    n.next = -1;
    n.key[0] = '\0';
    CopyStr(n.str, kMaxStr, val);
    return Value(&p, i);
}

Value Value::Num(Pool& p, double val) {
    const int i = p.Alloc();
    if (i < 0) {
        return Value();
    }
    Node& n = p.nodes[i];
    n.type = kNum;
    n.b = false;
    n.num = val;
    n.str[0] = '\0';
    n.firstChild = -1;
    n.next = -1;
    n.key[0] = '\0';
    return Value(&p, i);
}

Value Value::Bool(Pool& p, bool val) {
    const int i = p.Alloc();
    if (i < 0) {
        return Value();
    }
    Node& n = p.nodes[i];
    n.type = kBool;
    n.b = val;
    n.num = 0.0;
    n.str[0] = '\0';
    n.firstChild = -1;
    n.next = -1;
    n.key[0] = '\0';
    return Value(&p, i);
}
void Value::SetStr(const char* key, const char* val) { if (pool_ != nullptr) ObjSetStr(*pool_, *this, key, val); }
void Value::SetNum(const char* key, double val) { if (pool_ != nullptr) ObjSetNum(*pool_, *this, key, val); }
void Value::SetBool(const char* key, bool val) { if (pool_ != nullptr) ObjSetBool(*pool_, *this, key, val); }
void Value::SetValue(const char* key, Value child) { if (pool_ != nullptr) ObjSetValue(*pool_, *this, key, child); }
void Value::AddStr(const char* val) { if (pool_ != nullptr) ArrAddStr(*pool_, *this, val); }
void Value::AddNum(double val) { if (pool_ != nullptr) ArrAddNum(*pool_, *this, val); }
void Value::AddBool(bool val) { if (pool_ != nullptr) ArrAddBool(*pool_, *this, val); }
void Value::AddValue(Value child) { if (pool_ != nullptr) ArrAddValue(*pool_, *this, child); }

// ---------------------------------------------------------------------------
// Serialization.
// ---------------------------------------------------------------------------
namespace {

struct Writer {
    char* out;
    int   cap;
    int   pos;
    bool  overflow;

    void Ch(char c) {
        if (pos + 1 >= cap) {
            overflow = true;
            return;
        }
        out[pos++] = c;
    }
    void Raw(const char* s) {
        while (*s != '\0') {
            Ch(*s++);
        }
    }
    void Str(const char* s) {
        Ch('"');
        for (; *s != '\0'; ++s) {
            const unsigned char c = static_cast<unsigned char>(*s);
            switch (c) {
            case '"':  Raw("\\\""); break;
            case '\\': Raw("\\\\"); break;
            case '\n': Raw("\\n");  break;
            case '\r': Raw("\\r");  break;
            case '\t': Raw("\\t");  break;
            case '\b': Raw("\\b");  break;
            case '\f': Raw("\\f");  break;
            default:
                if (c < 0x20) {
                    char esc[8] = {};
                    std::snprintf(esc, sizeof(esc), "\\u%04x", c);
                    Raw(esc);
                } else {
                    Ch(static_cast<char>(c));
                }
            }
        }
        Ch('"');
    }
};

void DumpNode(const Pool& p, int idx, Writer& w);

void DumpChildren(const Pool& p, int idx, Writer& w) {
    const Node& n = p.nodes[idx];
    if (n.type == kObj) {
        w.Ch('{');
        bool first = true;
        for (int c = n.firstChild; c >= 0; c = p.nodes[c].next) {
            if (!first) {
                w.Ch(',');
            }
            first = false;
            w.Str(p.nodes[c].key);
            w.Ch(':');
            DumpNode(p, c, w);
        }
        w.Ch('}');
    } else {
        w.Ch('[');
        bool first = true;
        for (int c = n.firstChild; c >= 0; c = p.nodes[c].next) {
            if (!first) {
                w.Ch(',');
            }
            first = false;
            DumpNode(p, c, w);
        }
        w.Ch(']');
    }
}

void DumpNode(const Pool& p, int idx, Writer& w) {
    const Node& n = p.nodes[idx];
    switch (n.type) {
    case kNull: w.Raw("null"); break;
    case kBool: w.Raw(n.b ? "true" : "false"); break;
    case kNum: {
        char buf[32] = {};
        if (n.num == static_cast<double>(static_cast<long long>(n.num))) {
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(n.num));
        } else {
            std::snprintf(buf, sizeof(buf), "%.6g", n.num);
        }
        w.Raw(buf);
        break;
    }
    case kStr: w.Str(n.str); break;
    case kObj:
    case kArr: DumpChildren(p, idx, w); break;
    }
}

}  // namespace

int Value::Dump(char* out, int cap) const {
    if (out == nullptr || cap <= 0 || !Valid()) {
        return -1;
    }
    Writer w{out, cap, 0, false};
    DumpNode(*pool_, idx_, w);
    if (w.overflow) {
        return -(w.pos + 1);
    }
    out[w.pos] = '\0';
    return w.pos;
}

// ---------------------------------------------------------------------------
// Parsing (recursive descent, allocating from the pool).
// ---------------------------------------------------------------------------
namespace {

struct Parser {
    const char* p;
    const char* end;
    Pool*       pool;
    const char* reason;

    void SkipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            ++p;
        }
    }
    char Peek() const { return (p < end) ? *p : '\0'; }
    bool Fail(const char* why) {
        if (reason == nullptr) {
            reason = why;
        }
        return false;
    }
};

bool ParseString(Parser& ps, char* dst, int cap) {
    if (ps.Peek() != '"') {
        return ps.Fail("expected string");
    }
    ++ps.p;
    int w = 0;
    while (ps.p < ps.end) {
        char c = *ps.p++;
        if (c == '"') {
            dst[w] = '\0';
            return true;
        }
        if (c == '\\' && ps.p < ps.end) {
            char e = *ps.p++;
            switch (e) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case 'u': {
                unsigned cp = 0;
                for (int i = 0; i < 4; ++i) {
                    if (ps.p >= ps.end) {
                        return ps.Fail("bad \\u escape");
                    }
                    char h = *ps.p++;
                    cp <<= 4;
                    if (h >= '0' && h <= '9')      cp |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                    else                           return ps.Fail("bad \\u digit");
                }
                if (cp < 0x80) {
                    if (w < cap - 1) dst[w++] = static_cast<char>(cp);
                } else if (cp < 0x800) {
                    if (w < cap - 2) { dst[w++] = static_cast<char>(0xC0 | (cp >> 6)); dst[w++] = static_cast<char>(0x80 | (cp & 0x3F)); }
                } else {
                    if (w < cap - 3) { dst[w++] = static_cast<char>(0xE0 | (cp >> 12)); dst[w++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); dst[w++] = static_cast<char>(0x80 | (cp & 0x3F)); }
                }
                continue;
            }
            default: c = e; break;
            }
        }
        if (w < cap - 1) {
            dst[w++] = c;
        }
    }
    return ps.Fail("unterminated string");
}

bool ParseNumber(Parser& ps, double& out) {
    const char* start = ps.p;
    if (ps.p < ps.end && (*ps.p == '-' || *ps.p == '+')) {
        ++ps.p;
    }
    while (ps.p < ps.end && ((*ps.p >= '0' && *ps.p <= '9') || *ps.p == '.' ||
                             *ps.p == 'e' || *ps.p == 'E' ||
                             *ps.p == '+' || *ps.p == '-')) {
        ++ps.p;
    }
    if (ps.p == start) {
        return ps.Fail("bad number");
    }
    char buf[40] = {};
    const int len = static_cast<int>(ps.p - start);
    const int n = (len < static_cast<int>(sizeof(buf)) - 1) ? len : static_cast<int>(sizeof(buf)) - 1;
    std::memcpy(buf, start, static_cast<size_t>(n));
    buf[n] = '\0';
    out = std::strtod(buf, nullptr);
    return true;
}

bool ParseValue(Parser& ps, int& out);

bool ParseObject(Parser& ps, int parent) {
    ps.SkipWs();
    if (ps.Peek() == '}') {
        ++ps.p;
        return true;
    }
    while (true) {
        ps.SkipWs();
        char key[kMaxKey] = {};
        if (!ParseString(ps, key, kMaxKey)) {
            return false;
        }
        ps.SkipWs();
        if (ps.Peek() != ':') {
            return ps.Fail("expected ':'");
        }
        ++ps.p;
        int child = -1;
        if (!ParseValue(ps, child)) {
            return false;
        }
        CopyStr(ps.pool->nodes[child].key, kMaxKey, key);
        LinkChild(*ps.pool, parent, child);
        ps.SkipWs();
        char sep = ps.Peek();
        if (sep == ',') {
            ++ps.p;
            continue;
        }
        if (sep == '}') {
            ++ps.p;
            return true;
        }
        return ps.Fail("expected ',' or '}'");
    }
}

bool ParseArray(Parser& ps, int parent) {
    ps.SkipWs();
    if (ps.Peek() == ']') {
        ++ps.p;
        return true;
    }
    while (true) {
        int child = -1;
        if (!ParseValue(ps, child)) {
            return false;
        }
        ps.pool->nodes[child].key[0] = '\0';
        LinkChild(*ps.pool, parent, child);
        ps.SkipWs();
        char sep = ps.Peek();
        if (sep == ',') {
            ++ps.p;
            continue;
        }
        if (sep == ']') {
            ++ps.p;
            return true;
        }
        return ps.Fail("expected ',' or ']'");
    }
}

bool ParseValue(Parser& ps, int& out) {
    ps.SkipWs();
    if (ps.p >= ps.end) {
        return ps.Fail("unexpected end");
    }
    char c = ps.Peek();
    const int idx = ps.pool->Alloc();
    if (idx < 0) {
        return ps.Fail("node pool exhausted");
    }
    Node& n = ps.pool->nodes[idx];
    n.type = kNull;
    n.b = false;
    n.num = 0.0;
    n.str[0] = '\0';
    n.key[0] = '\0';
    n.firstChild = -1;
    n.next = -1;

    if (c == '{') {
        ++ps.p;
        n.type = kObj;
        if (!ParseObject(ps, idx)) {
            return false;
        }
        out = idx;
        return true;
    }
    if (c == '[') {
        ++ps.p;
        n.type = kArr;
        if (!ParseArray(ps, idx)) {
            return false;
        }
        out = idx;
        return true;
    }
    if (c == '"') {
        n.type = kStr;
        if (!ParseString(ps, n.str, kMaxStr)) {
            return false;
        }
        out = idx;
        return true;
    }
    if (c == 't' && (ps.end - ps.p) >= 4 && std::strncmp(ps.p, "true", 4) == 0) {
        ps.p += 4;
        n.type = kBool;
        n.b = true;
        out = idx;
        return true;
    }
    if (c == 'f' && (ps.end - ps.p) >= 5 && std::strncmp(ps.p, "false", 5) == 0) {
        ps.p += 5;
        n.type = kBool;
        n.b = false;
        out = idx;
        return true;
    }
    if (c == 'n' && (ps.end - ps.p) >= 4 && std::strncmp(ps.p, "null", 4) == 0) {
        ps.p += 4;
        n.type = kNull;
        out = idx;
        return true;
    }
    double num = 0.0;
    if (ParseNumber(ps, num)) {
        n.type = kNum;
        n.num = num;
        out = idx;
        return true;
    }
    return ps.Fail("unexpected token");
}

}  // namespace

bool Value::Parse(const char* text, Pool& pool, Value& out, char* err, int errCap) {
    if (text == nullptr) {
        if (err != nullptr) {
            CopyStr(err, errCap, "null input");
        }
        return false;
    }
    Parser ps{text, text + std::strlen(text), &pool, nullptr};
    int root = -1;
    const bool ok = ParseValue(ps, root);
    if (ok) {
        ps.SkipWs();
        if (ps.p < ps.end) {
            ps.Fail("trailing garbage");
        }
    }
    if (err != nullptr) {
        CopyStr(err, errCap, ok ? "" : (ps.reason != nullptr ? ps.reason : "parse error"));
    }
    if (!ok) {
        return false;
    }
    out = Value(&pool, root);
    return true;
}

}  // namespace ppjson