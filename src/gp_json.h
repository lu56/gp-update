#pragma once
#include <string>
#include <vector>
#include <map>
#include <variant>
#include <memory>

namespace gp {

// Minimal JSON value type
class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolVal = false;
    double numVal = 0;
    std::string strVal;
    std::vector<JsonValue> arrVal;
    std::map<std::string, JsonValue> objVal;

    bool isNull() const { return type == Type::Null; }
    bool isBool() const { return type == Type::Bool; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isArray() const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    bool asBool(bool def = false) const {
        if (type == Type::Bool) return boolVal;
        if (type == Type::Number) return numVal != 0;
        return def;
    }
    int asInt(int def = 0) const {
        if (type == Type::Number) return (int)numVal;
        if (type == Type::String) { try { return std::stoi(strVal); } catch (...) {} }
        return def;
    }
    long long asInt64(long long def = 0) const {
        if (type == Type::Number) return (long long)numVal;
        return def;
    }
    double asDouble(double def = 0) const {
        if (type == Type::Number) return numVal;
        return def;
    }
    const std::string& asString(const std::string& def = "") const {
        if (type == Type::String) return strVal;
        return def;
    }

    const JsonValue* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        auto it = objVal.find(key);
        if (it == objVal.end()) return nullptr;
        return &it->second;
    }
    bool has(const std::string& key) const { return find(key) != nullptr; }

    // Parse JSON string
    static JsonValue parse(const std::string& json);
    // Serialize to JSON string
    std::string dump(bool pretty = false, int indent = 0) const;
};

} // namespace gp
