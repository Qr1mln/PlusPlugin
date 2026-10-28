// json_util.h - self-contained minimal JSON reader/writer (no third-party dependency).
//
// Scope: just enough for the version protocol - objects, arrays, strings,
// numbers, booleans and null. Deliberately NOT a full RFC 8259 parser:
// no surrogate-pair handling and no big-number precision guarantees, because
// the protocol fields are short and simple. Keeping it local also keeps the
// proxy free of any external JSON header.
//
// Keep this file pure ASCII.
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ppjson {

// A lightweight JSON value: one struct with a type tag and per-type fields,
// which keeps the implementation small and avoids templates / variants.
class Value {
public:
    enum Type { kNull, kBool, kNum, kStr, kArr, kObj };

    Value() = default;

    // Factories.
    static Value Bool(bool b);
    static Value Num(double n);
    static Value Str(const std::string& s);
    static Value Obj();
    static Value Arr();

    Type type() const { return type_; }
    bool IsNull() const { return type_ == kNull; }
    bool IsObj() const { return type_ == kObj; }
    bool IsArr() const { return type_ == kArr; }
    bool IsStr() const { return type_ == kStr; }

    bool AsBool(bool def = false) const { return type_ == kBool ? bool_ : def; }
    double AsNum(double def = 0.0) const { return type_ == kNum ? num_ : def; }
    long long AsInt(long long def = 0) const {
        return type_ == kNum ? static_cast<long long>(num_) : def;
    }
    const std::string& AsStr() const;

    // Object access. Set() turns the value into an object if it was not one.
    void Set(const std::string& key, const Value& v);
    const Value* Find(const std::string& key) const;

    // Array access.
    void Push(const Value& v);
    const Value& At(std::size_t i) const;
    std::size_t Size() const;

    // Parse text into out. On failure returns false and fills err (if not null).
    static bool Parse(const std::string& text, Value& out, std::string* err);

    // Compact serialization.
    std::string Dump() const;

private:
    void AppendTo(std::string& out) const;

    Type type_ = kNull;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<Value> arr_;
    std::vector<std::pair<std::string, Value>> obj_;
};

}  // namespace ppjson