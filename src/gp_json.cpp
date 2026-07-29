#include "gp_json.h"
#include <sstream>
#include <cmath>
#include <cctype>
#include <algorithm>

namespace gp {

// ===== Parser =====
class JsonParser {
    const char* p;
    const char* end;
public:
    JsonParser(const std::string& s) : p(s.c_str()), end(s.c_str() + s.size()) {}

    void skipWs() {
        while (p < end) {
            char c = *p;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p++;
            else break;
        }
    }

    bool parse(JsonValue& out) {
        skipWs();
        if (p >= end) return false;
        char c = *p;
        if (c == '{') return parseObject(out);
        if (c == '[') return parseArray(out);
        if (c == '"') return parseString(out);
        if (c == 't' || c == 'f') return parseBool(out);
        if (c == 'n') return parseNull(out);
        return parseNumber(out);
    }

    bool parseObject(JsonValue& out) {
        out.type = JsonValue::Type::Object;
        out.objVal.clear();
        p++; // skip {
        skipWs();
        if (p < end && *p == '}') { p++; return true; }
        while (p < end) {
            skipWs();
            if (*p != '"') return false;
            JsonValue key;
            if (!parseString(key)) return false;
            skipWs();
            if (p >= end || *p != ':') return false;
            p++; // skip :
            JsonValue val;
            if (!parse(val)) return false;
            out.objVal[key.strVal] = std::move(val);
            skipWs();
            if (p >= end) return false;
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; return true; }
            return false;
        }
        return false;
    }

    bool parseArray(JsonValue& out) {
        out.type = JsonValue::Type::Array;
        out.arrVal.clear();
        p++; // skip [
        skipWs();
        if (p < end && *p == ']') { p++; return true; }
        while (p < end) {
            JsonValue val;
            if (!parse(val)) return false;
            out.arrVal.push_back(std::move(val));
            skipWs();
            if (p >= end) return false;
            if (*p == ',') { p++; continue; }
            if (*p == ']') { p++; return true; }
            return false;
        }
        return false;
    }

    bool parseString(JsonValue& out) {
        out.type = JsonValue::Type::String;
        out.strVal.clear();
        p++; // skip opening "
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= end) return false;
                char esc = *p++;
                switch (esc) {
                    case '"': out.strVal += '"'; break;
                    case '\\': out.strVal += '\\'; break;
                    case '/': out.strVal += '/'; break;
                    case 'n': out.strVal += '\n'; break;
                    case 't': out.strVal += '\t'; break;
                    case 'r': out.strVal += '\r'; break;
                    case 'b': out.strVal += '\b'; break;
                    case 'f': out.strVal += '\f'; break;
                    case 'u': {
                        if (p + 4 > end) return false;
                        unsigned int code = 0;
                        for (int i = 0; i < 4; i++) {
                            char h = *p++;
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= (h - '0');
                            else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
                            else return false;
                        }
                        // Basic UTF-8 encoding (BMP only, no surrogate pairs)
                        if (code < 0x80) {
                            out.strVal += (char)code;
                        } else if (code < 0x800) {
                            out.strVal += (char)(0xC0 | (code >> 6));
                            out.strVal += (char)(0x80 | (code & 0x3F));
                        } else {
                            out.strVal += (char)(0xE0 | (code >> 12));
                            out.strVal += (char)(0x80 | ((code >> 6) & 0x3F));
                            out.strVal += (char)(0x80 | (code & 0x3F));
                        }
                        break;
                    }
                    default: out.strVal += esc; break;
                }
            } else {
                out.strVal += c;
            }
        }
        return false;
    }

    bool parseNumber(JsonValue& out) {
        out.type = JsonValue::Type::Number;
        std::string s;
        while (p < end) {
            char c = *p;
            if (c >= '0' && c <= '9' || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
                s += c; p++;
            } else break;
        }
        if (s.empty()) return false;
        try { out.numVal = std::stod(s); } catch (...) { return false; }
        return true;
    }

    bool parseBool(JsonValue& out) {
        out.type = JsonValue::Type::Bool;
        if (end - p >= 4 && strncmp(p, "true", 4) == 0) {
            out.boolVal = true; p += 4; return true;
        }
        if (end - p >= 5 && strncmp(p, "false", 5) == 0) {
            out.boolVal = false; p += 5; return true;
        }
        return false;
    }

    bool parseNull(JsonValue& out) {
        out.type = JsonValue::Type::Null;
        if (end - p >= 4 && strncmp(p, "null", 4) == 0) { p += 4; return true; }
        return false;
    }
};

JsonValue JsonValue::parse(const std::string& json) {
    JsonParser parser(json);
    JsonValue result;
    if (!parser.parse(result)) {
        result.type = Type::Null;
    }
    return result;
}

// ===== Serializer =====
static void escapeString(const std::string& s, std::string& out) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

std::string JsonValue::dump(bool pretty, int indent) const {
    std::string out;
    std::string pad(pretty ? std::string(indent * 2, ' ') : "");
    std::string nl(pretty ? "\n" : "");

    switch (type) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += boolVal ? "true" : "false"; break;
        case Type::Number: {
            if (numVal == (double)(long long)numVal && std::abs(numVal) < 1e15) {
                out += std::to_string((long long)numVal);
            } else {
                std::ostringstream ss;
                ss << numVal;
                out += ss.str();
            }
            break;
        }
        case Type::String: escapeString(strVal, out); break;
        case Type::Array: {
            out += "[" + nl;
            for (size_t i = 0; i < arrVal.size(); i++) {
                if (pretty) out += std::string((indent + 1) * 2, ' ');
                out += arrVal[i].dump(pretty, indent + 1);
                if (i + 1 < arrVal.size()) out += ",";
                out += nl;
            }
            if (pretty) out += pad;
            out += "]";
            break;
        }
        case Type::Object: {
            out += "{" + nl;
            bool first = true;
            for (auto& [k, v] : objVal) {
                if (!first) out += "," + nl;
                first = false;
                if (pretty) out += std::string((indent + 1) * 2, ' ');
                escapeString(k, out);
                out += pretty ? ": " : ":";
                out += v.dump(pretty, indent + 1);
            }
            out += nl;
            if (pretty) out += pad;
            out += "}";
            break;
        }
    }
    return out;
}

} // namespace gp
