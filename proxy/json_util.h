// json_util.h - self-contained minimal JSON reader/writer.
//
// No third-party dependency and no C++ standard library containers: values
// live in a caller-owned fixed-size node pool, so this module stays STL-free
// and allocation-free. Children are an intrusive singly-linked list, which is
// why no arrays/vectors are needed.
//
// Scope: objects, arrays, strings, numbers, booleans, null. Deliberately NOT a
// full RFC 8259 parser: no surrogate pairs, no big-number precision. Protocol
// documents are tiny, so small caps are enough.
//
// Keep this file pure ASCII.
#pragma once

namespace ppjson {

// Fixed limits. Bump if a document ever needs more room.
const int kMaxStr   = 256;   // bytes of a string value
const int kMaxKey   = 32;    // bytes of an object key
const int kMaxNodes = 32;    // total value nodes for one whole document

enum Type { kNull = 0, kBool, kNum, kStr, kObj, kArr };

// A single value node. `firstChild`/`next` form the child list; when the node
// is an object member, `key` holds its name.
struct Node {
    Type   type;
    bool   b;
    double num;
    char   str[kMaxStr];
    char   key[kMaxKey];
    int    firstChild;   // head of child list (kObj / kArr), or -1
    int    next;         // next sibling, or -1
};

// The node pool. The caller owns one (usually on the stack).
struct Pool {
    Node nodes[kMaxNodes];
    int  used;

    Pool() : nodes(), used(0) {}
    int Alloc() { return (used < kMaxNodes) ? used++ : -1; }
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

private:
    Pool* pool_ = nullptr;
    int   idx_ = -1;
};

// Builders (for composing responses in the pool).
Value MakeObj(Pool& p);
void ObjSetStr(Pool& p, Value obj, const char* key, const char* val);
void ObjSetNum(Pool& p, Value obj, const char* key, double val);
void ObjSetBool(Pool& p, Value obj, const char* key, bool val);

}  // namespace ppjson