#include "json.h"

#include <stdexcept>
#include <cstdlib>

namespace {

void jerr(const std::string& m) { throw std::runtime_error("JSON: " + m); }

}  // namespace

JsonValue JsonParser::parse(const std::string& text) {
    JsonParser p;
    p.s_ = &text;
    p.i_ = 0;
    p.skipWs();
    if (p.i_ >= text.size()) jerr("empty input");
    JsonValue v = p.parseValue();
    p.skipWs();
    if (p.i_ < text.size()) jerr("trailing data after JSON value");
    return v;
}

void JsonParser::skipWs() {
    while (i_ < s_->size()) {
        char c = (*s_)[i_];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
        else break;
    }
}

JsonValue JsonParser::parseValue() {
    if (i_ >= s_->size()) jerr("unexpected end of input");
    char c = (*s_)[i_];
    switch (c) {
        case '{': return parseObject();
        case '[': return parseArray();
        case '"': return parseString();
        case 't': case 'f': case 'n': return parseLiteral();
        default:
            if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
            jerr("unexpected character");
    }
    return JsonValue();
}

JsonValue JsonParser::parseObject() {
    JsonValue v; v.type = JsonValue::Object;
    ++i_;  // consume '{'
    skipWs();
    if (i_ < s_->size() && (*s_)[i_] == '}') { ++i_; return v; }
    while (true) {
        skipWs();
        if (i_ >= s_->size() || (*s_)[i_] != '"') jerr("expected object key string");
        JsonValue key = parseString();
        skipWs();
        if (i_ >= s_->size() || (*s_)[i_] != ':') jerr("expected ':' after key");
        ++i_;
        skipWs();
        JsonValue val = parseValue();
        v.members.emplace_back(key.strVal, std::move(val));
        skipWs();
        if (i_ >= s_->size()) jerr("unterminated object");
        char c = (*s_)[i_];
        if (c == ',') { ++i_; continue; }
        if (c == '}') { ++i_; break; }
        jerr("expected ',' or '}' in object");
    }
    return v;
}

JsonValue JsonParser::parseArray() {
    JsonValue v; v.type = JsonValue::Array;
    ++i_;  // consume '['
    skipWs();
    if (i_ < s_->size() && (*s_)[i_] == ']') { ++i_; return v; }
    while (true) {
        skipWs();
        JsonValue val = parseValue();
        v.arr.push_back(std::move(val));
        skipWs();
        if (i_ >= s_->size()) jerr("unterminated array");
        char c = (*s_)[i_];
        if (c == ',') { ++i_; continue; }
        if (c == ']') { ++i_; break; }
        jerr("expected ',' or ']' in array");
    }
    return v;
}

void JsonParser::appendUtf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out.push_back((char)cp);
    } else if (cp <= 0x7FF) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

JsonValue JsonParser::parseString() {
    JsonValue v; v.type = JsonValue::String;
    ++i_;  // consume opening '"'
    std::string& out = v.strVal;
    while (i_ < s_->size()) {
        char c = (*s_)[i_++];
        if (c == '\\') {
            if (i_ >= s_->size()) jerr("bad escape at end of input");
            char e = (*s_)[i_++];
            switch (e) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    if (i_ + 4 > s_->size()) jerr("bad \\u escape");
                    unsigned int cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = (*s_)[i_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned int)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned int)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned int)(h - 'A' + 10);
                        else jerr("bad hex digit in \\u");
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // high surrogate; need a following \uXXXX low surrogate
                        if (i_ + 6 <= s_->size() && (*s_)[i_] == '\\' && (*s_)[i_ + 1] == 'u') {
                            i_ += 2;
                            unsigned int lo = 0;
                            for (int k = 0; k < 4; ++k) {
                                char h = (*s_)[i_++];
                                lo <<= 4;
                                if (h >= '0' && h <= '9') lo |= (unsigned int)(h - '0');
                                else if (h >= 'a' && h <= 'f') lo |= (unsigned int)(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') lo |= (unsigned int)(h - 'A' + 10);
                                else jerr("bad hex digit in \\u");
                            }
                            unsigned int full = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            appendUtf8(out, full);
                        } else {
                            jerr("bad surrogate pair");
                        }
                    } else {
                        appendUtf8(out, cp);
                    }
                    break;
                }
                default: jerr("bad escape character");
            }
        } else if (c == '"') {
            break;
        } else {
            out.push_back(c);
        }
    }
    return v;
}

JsonValue JsonParser::parseNumber() {
    JsonValue v; v.type = JsonValue::Number;
    size_t start = i_;
    if (i_ < s_->size() && (*s_)[i_] == '-') ++i_;
    while (i_ < s_->size()) {
        char c = (*s_)[i_];
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
            c == '+' || c == '-') ++i_;
        else break;
    }
    v.numStr = s_->substr(start, i_ - start);
    if (v.numStr.empty()) jerr("invalid number");
    return v;
}

JsonValue JsonParser::parseLiteral() {
    if (s_->compare(i_, 4, "true") == 0)  { i_ += 4; JsonValue v; v.type = JsonValue::Bool; v.boolVal = true;  return v; }
    if (s_->compare(i_, 5, "false") == 0) { i_ += 5; JsonValue v; v.type = JsonValue::Bool; v.boolVal = false; return v; }
    if (s_->compare(i_, 4, "null") == 0)  { i_ += 4; JsonValue v; v.type = JsonValue::Null; return v; }
    jerr("invalid literal (expected true/false/null)");
    return JsonValue();
}
