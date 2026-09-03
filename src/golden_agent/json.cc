#include "golden_agent/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace ga::json {

// --------------------------------------------------------------------------
// double formatting: shortest decimal that round-trips (Python-repr style)
// --------------------------------------------------------------------------
std::string format_double(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    if (d == 0.0) return "0.0";
    char buf[64];
    for (int prec = 1; prec <= 17; prec++) {
        std::snprintf(buf, sizeof(buf), "%.*f", prec, d);
        if (std::strtod(buf, nullptr) == d) {
            // trim trailing zeros but keep at least one decimal digit
            std::string s = buf;
            if (s.find('.') != std::string::npos) {
                size_t last = s.find_last_not_of('0');
                if (last == s.size() - 1) last++;  // keep "x.0"
                s = s.substr(0, last);
            }
            return s;
        }
    }
    std::snprintf(buf, sizeof(buf), "%.17f", d);
    return buf;
}

// --------------------------------------------------------------------------
// parser
// --------------------------------------------------------------------------
namespace {

struct Parser {
    const std::string& t;
    size_t pos = 0;

    explicit Parser(const std::string& text) : t(text) {}

    [[noreturn]] void fail(const std::string& msg) {
        throw ParseError(msg);
    }

    void skip_ws() {
        while (pos < t.size() && (t[pos] == ' ' || t[pos] == '\t' || t[pos] == '\n' || t[pos] == '\r'))
            pos++;
    }

    char peek() {
        if (pos >= t.size()) fail("unexpected end of input");
        return t[pos];
    }

    void expect(char c) {
        if (peek() != c) fail(std::string("expected '") + c + "'");
        pos++;
    }

    Value parse_value() {
        skip_ws();
        char c = peek();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return Value::make_string(parse_string());
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') { expect_lit("null"); return Value::make_null(); }
        return parse_number();
    }

    void expect_lit(const char* lit) {
        size_t n = std::strlen(lit);
        if (t.compare(pos, n, lit) != 0) fail("invalid literal");
        pos += n;
    }

    Value parse_bool() {
        if (peek() == 't') { expect_lit("true"); return Value::make_bool(true); }
        expect_lit("false");
        return Value::make_bool(false);
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            char c = peek();
            pos++;
            if (c == '"') break;
            if (c == '\\') {
                char e = peek();
                pos++;
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (pos + 4 > t.size()) fail("bad \\u escape");
                        unsigned cp = (unsigned)std::stoul(t.substr(pos, 4), nullptr, 16);
                        pos += 4;
                        if (cp >= 0xd800 && cp <= 0xdbff && pos + 4 <= t.size() &&
                            t[pos] == '\\' && t[pos + 1] == 'u') {
                            unsigned lo = (unsigned)std::stoul(t.substr(pos + 2, 4), nullptr, 16);
                            if (lo >= 0xdc00 && lo <= 0xdfff) {
                                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                                pos += 6;
                            }
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: fail("bad escape");
                }
            } else if ((unsigned char)c < 0x20) {
                fail("control character in string");
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    static void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out.push_back((char)cp);
        else if (cp < 0x800) {
            out.push_back((char)(0xc0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3f)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xe0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back((char)(0x80 | (cp & 0x3f)));
        } else {
            out.push_back((char)(0xf0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back((char)(0x80 | (cp & 0x3f)));
        }
    }

    Value parse_number() {
        size_t start = pos;
        if (peek() == '-') pos++;
        if (pos >= t.size() || !isdigit((unsigned char)t[pos])) fail("bad number");
        bool is_int = true;
        if (t[pos] == '0') pos++;
        else while (pos < t.size() && isdigit((unsigned char)t[pos])) pos++;
        if (pos < t.size() && t[pos] == '.') {
            is_int = false;
            pos++;
            while (pos < t.size() && isdigit((unsigned char)t[pos])) pos++;
        }
        if (pos < t.size() && (t[pos] == 'e' || t[pos] == 'E')) {
            is_int = false;
            pos++;
            if (pos < t.size() && (t[pos] == '+' || t[pos] == '-')) pos++;
            while (pos < t.size() && isdigit((unsigned char)t[pos])) pos++;
        }
        std::string lit = t.substr(start, pos - start);
        if (is_int) {
            try {
                return Value::make_int((int64_t)std::stoll(lit));
            } catch (...) {
                // overflow -> treat as float
            }
        }
        return Value::make_double(std::stod(lit));
    }

    Value parse_array() {
        expect('[');
        Value v = Value::make_array();
        skip_ws();
        if (peek() == ']') { pos++; return v; }
        while (true) {
            v.arr.push_back(parse_value());
            skip_ws();
            if (peek() == ',') { pos++; continue; }
            expect(']');
            break;
        }
        return v;
    }

    Value parse_object() {
        expect('{');
        Value v = Value::make_object();
        skip_ws();
        if (peek() == '}') { pos++; return v; }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            expect(':');
            v.obj.emplace_back(std::move(key), parse_value());
            skip_ws();
            if (peek() == ',') { pos++; continue; }
            expect('}');
            break;
        }
        return v;
    }
};

}  // namespace

Value parse(const std::string& text) {
    Parser p(text);
    Value v = p.parse_value();
    p.skip_ws();
    if (p.pos != text.size()) throw ParseError("trailing characters");
    return v;
}

// --------------------------------------------------------------------------
// serializer (ensure_ascii, Python-dumps compatible)
// --------------------------------------------------------------------------
namespace {

void dump_string(std::string& out, const std::string& s) {
    out.push_back('"');
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        unsigned cp;
        size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if (c >= 0xc0 && c < 0xe0) { cp = c & 0x1f; len = 2; }
        else if (c >= 0xe0 && c < 0xf0) { cp = c & 0x0f; len = 3; }
        else { cp = c & 0x07; len = 4; }
        for (size_t j = 1; j < len && i + j < s.size(); j++)
            cp = (cp << 6) | (s[i + j] & 0x3f);
        i += len;

        auto emit_hex = [&](unsigned v) {
            static const char* H = "0123456789abcdef";
            out.push_back('\\');
            out.push_back('u');
            out.push_back(H[(v >> 12) & 0xf]);
            out.push_back(H[(v >> 8) & 0xf]);
            out.push_back(H[(v >> 4) & 0xf]);
            out.push_back(H[v & 0xf]);
        };
        switch (cp) {
            case '"': out += "\\\""; continue;
            case '\\': out += "\\\\"; continue;
            case '\b': out += "\\b"; continue;
            case '\f': out += "\\f"; continue;
            case '\n': out += "\\n"; continue;
            case '\r': out += "\\r"; continue;
            case '\t': out += "\\t"; continue;
            default:
                if (cp < 0x20) {
                    out.push_back('\\');
                    out.push_back('u');
                    out.push_back('0');
                    out.push_back('0');
                    out.push_back("0123456789abcdef"[cp >> 4]);
                    out.push_back("0123456789abcdef"[cp & 0xf]);
                    continue;
                }
                if (cp < 0x80) { out.push_back((char)cp); continue; }
                // ensure_ascii: encode as UTF-16 code units
                if (cp <= 0xffff) emit_hex((unsigned)cp);
                else {
                    unsigned v = cp - 0x10000;
                    emit_hex(0xd800 + (v >> 10));
                    emit_hex(0xdc00 + (v & 0x3ff));
                }
                continue;
        }
    }
    out.push_back('"');
}

// Python json.dumps(v, indent=n): empty containers stay compact; non-empty
// ones break one element per line, indented n*(depth+1) spaces.
void dump(const Value& v, std::string& out, int indent, int depth) {
    switch (v.kind) {
        case Value::Kind::Null: out += "null"; break;
        case Value::Kind::Bool: out += v.b ? "true" : "false"; break;
        case Value::Kind::Int: out += std::to_string(v.i); break;
        case Value::Kind::Double: out += format_double(v.d); break;
        case Value::Kind::String: dump_string(out, v.s); break;
        case Value::Kind::Array: {
            if (v.arr.empty() || indent <= 0) {
                out.push_back('[');
                for (size_t i = 0; i < v.arr.size(); i++) {
                    if (i) out.push_back(',');
                    dump(v.arr[i], out, indent, depth);
                }
                out.push_back(']');
                break;
            }
            out.push_back('[');
            for (size_t i = 0; i < v.arr.size(); i++) {
                if (i) out.push_back(',');
                out.push_back('\n');
                for (int j = 0; j <= indent * (depth + 1); j++) out.push_back(' ');
                dump(v.arr[i], out, indent, depth + 1);
            }
            out.push_back('\n');
            for (int j = 0; j < indent * depth; j++) out.push_back(' ');
            out.push_back(']');
            break;
        }
        case Value::Kind::Object: {
            if (v.obj.empty() || indent <= 0) {
                out.push_back('{');
                for (size_t i = 0; i < v.obj.size(); i++) {
                    if (i) out.push_back(',');
                    dump_string(out, v.obj[i].first);
                    out.push_back(':');
                    dump(v.obj[i].second, out, indent, depth);
                }
                out.push_back('}');
                break;
            }
            out.push_back('{');
            for (size_t i = 0; i < v.obj.size(); i++) {
                if (i) out.push_back(',');
                out.push_back('\n');
                for (int j = 0; j <= indent * (depth + 1); j++) out.push_back(' ');
                dump_string(out, v.obj[i].first);
                out.push_back(':');
                out.push_back(' ');
                dump(v.obj[i].second, out, indent, depth + 1);
            }
            out.push_back('\n');
            for (int j = 0; j < indent * depth; j++) out.push_back(' ');
            out.push_back('}');
            break;
        }
    }
}

}  // namespace

std::string dumps(const Value& v, int indent) {
    std::string out;
    dump(v, out, indent, 0);
    return out;
}

}  // namespace ga::json
