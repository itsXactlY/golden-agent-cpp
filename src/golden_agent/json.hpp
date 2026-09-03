#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ga::json {

// Shortest round-trip decimal for a double (matches Python repr closely).
std::string format_double(double d);

struct Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;  // insertion-ordered

struct Value {
    enum class Kind { Null, Bool, Int, Double, String, Array, Object };
    Kind kind = Kind::Null;
    bool b = false;
    int64_t i = 0;
    double d = 0.0;
    std::string s;
    Array arr;
    Object obj;

    static Value make_null() { return {}; }
    static Value make_bool(bool x) { Value v; v.kind = Kind::Bool; v.b = x; return v; }
    static Value make_int(int64_t x) { Value v; v.kind = Kind::Int; v.i = x; return v; }
    static Value make_double(double x) { Value v; v.kind = Kind::Double; v.d = x; return v; }
    static Value make_string(std::string x) { Value v; v.kind = Kind::String; v.s = std::move(x); return v; }
    static Value make_array() { Value v; v.kind = Kind::Array; return v; }
    static Value make_object() { Value v; v.kind = Kind::Object; return v; }

    bool is_null() const { return kind == Kind::Null; }
    bool is_bool() const { return kind == Kind::Bool; }
    bool is_number() const { return kind == Kind::Int || kind == Kind::Double; }
    bool is_string() const { return kind == Kind::String; }
    bool is_array() const { return kind == Kind::Array; }
    bool is_object() const { return kind == Kind::Object; }

    // Python-dict-like lookups
    const Value* get(const std::string& key) const {
        if (!is_object()) return nullptr;
        for (const auto& [k, v] : obj)
            if (k == key) return &v;
        return nullptr;
    }
    bool has(const std::string& key) const { return get(key) != nullptr; }
    // returns "" for missing / non-string, mirroring .get(key) or ""
    std::string str_of(const std::string& key, const std::string& fallback = "") const {
        const Value* v = get(key);
        return v && v->is_string() ? v->s : fallback;
    }
    bool bool_of(const std::string& key, bool fallback = false) const {
        const Value* v = get(key);
        return v && v->is_bool() ? v->b : fallback;
    }
    double double_of(const std::string& key, double fallback = 0.0) const {
        const Value* v = get(key);
        if (!v) return fallback;
        if (v->is_number()) return v->kind == Kind::Int ? (double)v->i : v->d;
        return fallback;
    }
    std::string as_string() const {
        if (is_string()) return s;
        if (is_bool()) return b ? "true" : "false";
        if (is_null()) return "";
        return "";
    }
    // scalar-to-string like Python str()
    std::string scalar_repr() const {
        if (is_string()) return s;
        if (is_bool()) return b ? "True" : "False";
        if (is_null()) return "None";
        if (is_number()) return number_to_string();
        return "";
    }
    std::string number_to_string() const {
        if (kind == Kind::Int) return std::to_string(i);
        return format_double(d);
    }
};

// Shortest round-trip decimal for a double (matches Python repr closely).
std::string format_double(double d);

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

Value parse(const std::string& text);
// indent=0: compact; indent>0: python json.dumps(indent=...) layout.
std::string dumps(const Value& v, int indent = 0);

}  // namespace ga::json
