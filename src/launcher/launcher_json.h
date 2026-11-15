// A minimal JSON reader for the launcher (settings_schema.json, GitHub's release JSON). Objects, arrays, strings with
// escapes (\uXXXX -> UTF-8, surrogate pairs joined), numbers, true / false / null. Malformed input is REFUSED with the
// byte offset - never a partial tree. Pure and header-only - tested by src_beta/src/tests/launcher_schema_test.cpp.
#pragma once

#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dlvr::launcher::json {

class Value {
public:
    enum class Kind { Null, Bool, Num, Str, Arr, Obj };

    Kind kind = Kind::Null;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;   // order kept

    bool IsNull() const { return kind == Kind::Null; }
    bool Bool() const { return kind == Kind::Bool && b; }
    double Num() const { return kind == Kind::Num ? n : 0.0; }
    const std::string& Str() const { static const std::string e; return kind == Kind::Str ? s : e; }
    size_t Size() const { return kind == Kind::Arr ? arr.size() : (kind == Kind::Obj ? obj.size() : 0); }
    const Value& At(size_t i) const { return (kind == Kind::Arr && i < arr.size()) ? arr[i] : Null(); }
    const Value& Get(const std::string& k) const
    {
        if (kind == Kind::Obj) {
            for (const auto& kv : obj) {
                if (kv.first == k) { return kv.second; }
            }
        }
        return Null();
    }
    static const Value& Null() { static const Value v; return v; }
};

namespace detail {

struct Reader {
    const std::string& t;
    size_t p = 0;
    std::string err;

    void Ws() { while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\r' || t[p] == '\n')) { ++p; } }
    bool Fail(const char* what)
    {
        if (err.empty()) { err = std::string(what) + " at offset " + std::to_string(p); }
        return false;
    }
    bool Lit(const char* w)
    {
        size_t i = 0;
        for (; w[i]; ++i) {
            if (p + i >= t.size() || t[p + i] != w[i]) { return Fail("bad literal"); }
        }
        p += i;
        return true;
    }
    static void Utf8(std::string* o, unsigned cp)
    {
        if (cp < 0x80) { o->push_back(static_cast<char>(cp)); }
        else if (cp < 0x800) { o->push_back(static_cast<char>(0xC0 | (cp >> 6))); o->push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) {
            o->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            o->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            o->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            o->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            o->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            o->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            o->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    bool Hex4(unsigned* v)
    {
        if (p + 4 > t.size()) { return Fail("short \\u escape"); }
        *v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = t[p++];
            *v <<= 4;
            if (c >= '0' && c <= '9') { *v |= static_cast<unsigned>(c - '0'); }
            else if (c >= 'a' && c <= 'f') { *v |= static_cast<unsigned>(c - 'a' + 10); }
            else if (c >= 'A' && c <= 'F') { *v |= static_cast<unsigned>(c - 'A' + 10); }
            else { return Fail("bad \\u escape"); }
        }
        return true;
    }
    bool Str(std::string* o)
    {
        if (p >= t.size() || t[p] != '"') { return Fail("expected a string"); }
        ++p;
        while (p < t.size()) {
            const char c = t[p++];
            if (c == '"') { return true; }
            if (static_cast<unsigned char>(c) < 0x20) { return Fail("control character in a string"); }
            if (c != '\\') { o->push_back(c); continue; }
            if (p >= t.size()) { break; }
            const char e = t[p++];
            switch (e) {
                case '"': o->push_back('"'); break;
                case '\\': o->push_back('\\'); break;
                case '/': o->push_back('/'); break;
                case 'b': o->push_back('\b'); break;
                case 'f': o->push_back('\f'); break;
                case 'n': o->push_back('\n'); break;
                case 'r': o->push_back('\r'); break;
                case 't': o->push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!Hex4(&cp)) { return false; }
                    if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= t.size() && t[p] == '\\' && t[p + 1] == 'u') {
                        p += 2;
                        unsigned lo = 0;
                        if (!Hex4(&lo)) { return false; }
                        if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); }
                    }
                    Utf8(o, cp);
                    break;
                }
                default: return Fail("bad escape");
            }
        }
        return Fail("unterminated string");
    }
    bool Val(Value* v, int depth)
    {
        if (depth > 64) { return Fail("nested too deep"); }
        Ws();
        if (p >= t.size()) { return Fail("unexpected end"); }
        const char c = t[p];
        if (c == '{') {
            ++p;
            v->kind = Value::Kind::Obj;
            Ws();
            if (p < t.size() && t[p] == '}') { ++p; return true; }
            while (true) {
                Ws();
                std::string k;
                if (!Str(&k)) { return false; }
                Ws();
                if (p >= t.size() || t[p] != ':') { return Fail("expected ':'"); }
                ++p;
                Value child;
                if (!Val(&child, depth + 1)) { return false; }
                v->obj.emplace_back(std::move(k), std::move(child));
                Ws();
                if (p < t.size() && t[p] == ',') { ++p; continue; }
                if (p < t.size() && t[p] == '}') { ++p; return true; }
                return Fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p;
            v->kind = Value::Kind::Arr;
            Ws();
            if (p < t.size() && t[p] == ']') { ++p; return true; }
            while (true) {
                Value child;
                if (!Val(&child, depth + 1)) { return false; }
                v->arr.push_back(std::move(child));
                Ws();
                if (p < t.size() && t[p] == ',') { ++p; continue; }
                if (p < t.size() && t[p] == ']') { ++p; return true; }
                return Fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v->kind = Value::Kind::Str; return Str(&v->s); }
        if (c == 't') { v->kind = Value::Kind::Bool; v->b = true; return Lit("true"); }
        if (c == 'f') { v->kind = Value::Kind::Bool; v->b = false; return Lit("false"); }
        if (c == 'n') { v->kind = Value::Kind::Null; return Lit("null"); }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const size_t s = p;
            if (t[p] == '-') { ++p; }
            while (p < t.size() && ((t[p] >= '0' && t[p] <= '9') || t[p] == '.' || t[p] == 'e' || t[p] == 'E' ||
                                    t[p] == '+' || t[p] == '-')) { ++p; }
            const std::string num = t.substr(s, p - s);
            char* end = nullptr;
            v->kind = Value::Kind::Num;
            v->n = std::strtod(num.c_str(), &end);
            if (!end || *end != '\0' || num == "-") { p = s; return Fail("bad number"); }
            return true;
        }
        return Fail("unexpected character");
    }
};

}  // namespace detail

// false = refused, *why = what and the byte offset. Trailing non-space after the value is refused.
inline bool Parse(const std::string& text, Value* out, std::string* why)
{
    detail::Reader r{text};
    Value v;
    if (!r.Val(&v, 0)) { *why = r.err; return false; }
    r.Ws();
    if (r.p != text.size()) { *why = "trailing characters at offset " + std::to_string(r.p); return false; }
    *out = std::move(v);
    return true;
}

}  // namespace dlvr::launcher::json
