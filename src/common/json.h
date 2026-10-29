#pragma once

#include <string>
#include <vector>
#include <utility>

// A minimal, dependency-free JSON value + recursive-descent parser. It supports
// the subset needed by the extractors: objects, arrays, strings (with escapes),
// numbers, booleans and null. On malformed input it throws std::runtime_error.

struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;

    bool boolVal = false;
    std::string numStr;   // raw numeric text (preserves precision)
    std::string strVal;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> members;  // object key/value pairs

    bool isObject() const { return type == Object; }
    bool isArray()  const { return type == Array; }
    bool isString() const { return type == String; }
    bool isNumber() const { return type == Number; }
    bool isBool()   const { return type == Bool; }
    bool isNull()   const { return type == Null; }

    // Object lookup by key (returns nullptr if not an object / not found).
    const JsonValue* find(const std::string& key) const {
        if (type != Object) return nullptr;
        for (const auto& m : members)
            if (m.first == key) return &m.second;
        return nullptr;
    }
    // Array element lookup (returns nullptr if not an array / out of range).
    const JsonValue* at(size_t i) const {
        if (type != Array) return nullptr;
        if (i >= arr.size()) return nullptr;
        return &arr[i];
    }

    long long asLong() const {
        try { return std::stoll(numStr); } catch (...) { return 0; }
    }
    double asDouble() const {
        try { return std::stod(numStr); } catch (...) { return 0.0; }
    }
    std::string asString() const {
        if (type == String) return strVal;
        if (type == Number) return numStr;
        return "";
    }
};

class JsonParser {
public:
    static JsonValue parse(const std::string& text);

private:
    const std::string* s_ = nullptr;
    size_t i_ = 0;

    JsonValue parseValue();
    void skipWs();
    JsonValue parseObject();
    JsonValue parseArray();
    JsonValue parseString();
    JsonValue parseNumber();
    JsonValue parseLiteral();
    static void appendUtf8(std::string& out, unsigned int cp);
};
