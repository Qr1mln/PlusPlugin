// json_util.h - shared minimal JSON reader/writer (no third-party, no STL).
//
// Values live in a caller-owned node pool: the caller supplies the node array
// and its capacity, so proxy can use a small stack pool and a heavier consumer
// can use a large heap pool (HeapAlloc). No std::string / std::vector here.
//
// Scope: objects, arrays, strings, numbers, booleans, null; parse + compact
// dump. Not a full RFC 8259 parser (no surrogate pairs, no big-number
// precision). Children are an intrusive singly-linked list, hence no arrays.
//
// Keep this file pure ASCII.
#pragma once

namespace ppjson {

// Per-node limits. Keys are short; string values may be longer (paths, text).
const int kMaxStr = 512;
const int kMaxKey = 64;

enum Type { kNull = 0, kBool, kNum, kStr, kObj, kArr };

// One value node. `firstChild`/`next` form the child list; `key` holds the
// member name when the node is an object member.
struct Node {
    Type   type;
    bool   b;
    double num;
    char   str[kMaxStr];
    char   key[kMaxKey];
    int    firstChild;   // head of child list (kObj / kArr), or -1
    int    next;         // next sibling, or -1
};

// Node pool backed by caller memory.
struct Pool {
    Node* nodes;
    int   cap;
    int   used;

    Pool(Node* pool, int capacity) : nodes(pool), cap(capacity), used(0) {}
    int Alloc() { return (used < cap) ? used++ : -1; }
};

// A value handle: pool + node index. Copyable and cheap.
class Value {
public:
    Value() = default;
    Value(Pool* pool, int index) : pool_(pool), idx_(index) {}

    bool Valid() const { return pool_ != nullptr && idx_ >= 0; }
    int Index() const { return idx_; }

    Type type() const;
    bool IsNull() const { return type() == kNull; }
    bool IsObj() const { return type() == kObj; }
    bool IsArr() const { return type() == kArr; }
    bool IsStr() const { return type() == kStr; }

    bool AsBool(bool def = false) const;
    double AsNum(double def = 0.0) const;
    long long AsInt(long long def = 0) const;
    const char* AsStr() const;

    Value Find(const char* key) const;   // object member, or an invalid Value
    Value At(int i) const;               // array item, or an invalid Value
    int Size() const;                    // array length

    // Parse text into pool. Returns false and writes a short reason into err.
    static bool Parse(const char* text, Pool& pool, Value& out, char* err, int errCap);

    // Serialize into out (NUL-terminated). Returns length (>= 0), or
    // -(need + 1) when out is too small.
    int Dump(char* out, int cap) const;

    // Convenience member builders (use this value's own pool). Handy for code
    // that composes a response in place, e.g. out.SetNum("x", rc.left).
    static Value Obj(Pool& p);
    static Value Arr(Pool& p);
    void SetStr(const char* key, const char* val);
    void SetNum(const char* key, double val);
    void SetBool(const char* key, bool val);
    void SetValue(const char* key, Value child);
    void AddStr(const char* val);
    void AddNum(double val);
    void AddBool(bool val);
    void AddValue(Value child);

private:
    Pool* pool_ = nullptr;
    int   idx_ = -1;
};

// Builders (for composing responses in the pool). Children are appended in
// source order; passing an invalid child/obj is a no-op.
Value MakeObj(Pool& p);
Value MakeArr(Pool& p);
void ObjSetStr(Pool& p, Value obj, const char* key, const char* val);
void ObjSetNum(Pool& p, Value obj, const char* key, double val);
void ObjSetBool(Pool& p, Value obj, const char* key, bool val);
void ObjSetValue(Pool& p, Value obj, const char* key, Value child);   // nested
void ArrAddStr(Pool& p, Value arr, const char* val);
void ArrAddNum(Pool& p, Value arr, double val);
void ArrAddBool(Pool& p, Value arr, bool val);
void ArrAddValue(Pool& p, Value arr, Value child);                    // nested

}  // namespace ppjson