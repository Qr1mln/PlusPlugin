// json_util.cpp - implementation of the minimal JSON reader/writer.
//
// Keep this file pure ASCII.
#include "json_util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ppjson {

// ---------------------------------------------------------------------------
// Factories.
// ---------------------------------------------------------------------------
Value Value::Bool(bool b) {
    Value v;
    v.type_ = kBool;
    v.bool_ = b;
    return v;
}

Value Value::Num(double n) {
    Value v;
    v.type_ = kNum;
    v.num_ = n;
    return v;
}

Value Value::Str(const std::string& s) {
    Value v;
    v.type_ = kStr;
    v.str_ = s;
    return v;
}

Value Value::Obj() {
    Value v;
    v.type_ = kObj;
    return v;
}

Value Value::Arr() {
    Value v;
    v.type_ = kArr;
    return v;
}

// ---------------------------------------------------------------------------
// Accessors.
// ---------------------------------------------------------------------------
const std::string& Value::AsStr() const {
    static const std::string kEmpty;
    return type_ == kStr ? str_ : kEmpty;
}

void Value::Set(const std::string& key, const Value& v) {
    type_ = kObj;
    for (auto& kv : obj_) {
        if (kv.first == key) {
            kv.second = v;
            return;
        }
    }
    obj_.emplace_back(key, v);
}

const Value* Value::Find(const std::string& key) const {
    if (type_ != kObj) {
        return nullptr;
    }
    for (const auto& kv : obj_) {
        if (kv.first == key) {
            return &kv.second;
        }
    }
    return nullptr;
}

void Value::Push(const Value& v) {
    type_ = kArr;
    arr_.push_back(v);
}

const Value& Value::At(std::size_t i) const {
    static const Value kNull;
    return (type_ == kArr && i < arr_.size()) ? arr_[i] : kNull;
}

std::size_t Value::Size() const {
    return type_ == kArr ? arr_.size() : 0;
}

// ---------------------------------------------------------------------------
// Serialization.
// ---------------------------------------------------------------------------
namespace {

void AppendString(const std::string& s, std::string& out) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8] = {};
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                out += buf;
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

}  // namespace

void Value::AppendTo(std::string& out) const {
    switch (type_) {
    case kNull:
        out += "null";
        break;
    case kBool:
        out += bool_ ? "true" : "false";
        break;
    case kNum: {
        char buf[32] = {};
        if (num_ == static_cast<double>(static_cast<long long>(num_))) {
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(num_));
        } else {
            std::snprintf(buf, sizeof(buf), "%.6g", num_);
        }
        out += buf;
        break;
    }
    case kStr:
        AppendString(str_, out);
        break;
    case kArr: {
        out.push_back('[');
        for (std::size_t i = 0; i < arr_.size(); ++i) {
            if (i != 0) {
                out.push_back(',');
            }
            arr_[i].AppendTo(out);
        }
        out.push_back(']');
        break;
    }
    case kObj: {
        out.push_back('{');
        bool first = true;
        for (const auto& kv : obj_) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            AppendString(kv.first, out);
            out.push_back(':');
            kv.second.AppendTo(out);
        }
        out.push_back('}');
        break;
    }
    }
}

std::string Value::Dump() const {
    std::string out;
    AppendTo(out);
    return out;
}

// ---------------------------------------------------------------------------
// Parsing (recursive descent).
// ---------------------------------------------------------------------------
namespace {

struct Parser {
    const char* p;
    const char* end;

    void SkipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            ++p;
        }
    }
    bool Eof() const { return p >= end; }
    char Peek() const { return (p < end) ? *p : '\0'; }
};

bool ParseValue(Parser& ps, Value& out, std::string* err);

bool ParseString(Parser& ps, std::string& out, std::string* err) {
    if (ps.Peek() != '"') {
        if (err) *err = "expected string";
        return false;
    }
    ++ps.p;
    out.clear();
    while (ps.p < ps.end) {
        char c = *ps.p++;
        if (c == '"') {
            return true;
        }
        if (c == '\\') {
            if (ps.p >= ps.end) {
                break;
            }
            char e = *ps.p++;
            switch (e) {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/');  break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'u': {
                // Minimal \uXXXX handling: BMP only, no surrogate pairs.
                unsigned cp = 0;
                for (int i = 0; i < 4; ++i) {
                    if (ps.p >= ps.end) {
                        if (err) *err = "bad \\u escape";
                        return false;
                    }
                    char h = *ps.p++;
                    cp <<= 4;
                    if (h >= '0' && h <= '9')      cp |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                    else {
                        if (err) *err = "bad \\u digit";
                        return false;
                    }
                }
                if (cp < 0x80) {
                    out.push_back(static_cast<char>(cp));
                } else if (cp < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default:
                out.push_back(e);
                break;
            }
        } else {
            out.push_back(c);
        }
    }
    if (err) *err = "unterminated string";
    return false;
}

bool ParseNumber(Parser& ps, double& out, std::string* err) {
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
        if (err) *err = "bad number";
        return false;
    }
    const std::string s(start, ps.p - start);
    out = std::strtod(s.c_str(), nullptr);
    return true;
}

bool ParseValue(Parser& ps, Value& out, std::string* err) {
    ps.SkipWs();
    if (ps.Eof()) {
        if (err) *err = "unexpected end";
        return false;
    }
    char c = ps.Peek();

    if (c == '{') {
        ++ps.p;
        out = Value::Obj();
        ps.SkipWs();
        if (ps.Peek() == '}') {
            ++ps.p;
            return true;
        }
        while (true) {
            ps.SkipWs();
            std::string key;
            if (!ParseString(ps, key, err)) {
                return false;
            }
            ps.SkipWs();
            if (ps.Peek() != ':') {
                if (err) *err = "expected ':'";
                return false;
            }
            ++ps.p;
            Value v;
            if (!ParseValue(ps, v, err)) {
                return false;
            }
            out.Set(key, v);
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
            if (err) *err = "expected ',' or '}'";
            return false;
        }
    }

    if (c == '[') {
        ++ps.p;
        out = Value::Arr();
        ps.SkipWs();
        if (ps.Peek() == ']') {
            ++ps.p;
            return true;
        }
        while (true) {
            Value v;
            if (!ParseValue(ps, v, err)) {
                return false;
            }
            out.Push(v);
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
            if (err) *err = "expected ',' or ']'";
            return false;
        }
    }

    if (c == '"') {
        std::string s;
        if (!ParseString(ps, s, err)) {
            return false;
        }
        out = Value::Str(s);
        return true;
    }

    if (c == 't' && (ps.end - ps.p) >= 4 && std::strncmp(ps.p, "true", 4) == 0) {
        ps.p += 4;
        out = Value::Bool(true);
        return true;
    }
    if (c == 'f' && (ps.end - ps.p) >= 5 && std::strncmp(ps.p, "false", 5) == 0) {
        ps.p += 5;
        out = Value::Bool(false);
        return true;
    }
    if (c == 'n' && (ps.end - ps.p) >= 4 && std::strncmp(ps.p, "null", 4) == 0) {
        ps.p += 4;
        out = Value();
        return true;
    }

    double num = 0.0;
    if (ParseNumber(ps, num, err)) {
        out = Value::Num(num);
        return true;
    }
    if (err) *err = "unexpected token";
    return false;
}

}  // namespace

bool Value::Parse(const std::string& text, Value& out, std::string* err) {
    Parser ps{text.c_str(), text.c_str() + text.size()};
    if (!ParseValue(ps, out, err)) {
        return false;
    }
    ps.SkipWs();
    if (!ps.Eof()) {
        if (err) *err = "trailing garbage";
        return false;
    }
    return true;
}

}  // namespace ppjson