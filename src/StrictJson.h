#pragma once

// Minimal strict JSON reader shared by the runtime lock and the stored
// preflight verdict: both are documents this program wrote itself, so the
// reader accepts exactly RFC 8259 and nothing lenient, and a truncated or
// hand-edited file fails to parse instead of being searched for a substring.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace strict_json {

// Numbers keep their source text so integers are converted exactly.
struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind{Kind::Null};
    bool boolean{};
    std::string text; // Number: source text; String: decoded UTF-8
    std::vector<JsonValue> items; // Array items or Object values
    std::vector<std::string> keys; // Object keys, parallel to items

    const JsonValue* Member(std::string_view key) const
    {
        if (kind != Kind::Object) return nullptr;
        for (size_t index = 0; index < keys.size(); ++index) {
            if (keys[index] == key) return &items[index];
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    bool ParseDocument(JsonValue& out)
    {
        SkipWhitespace();
        if (!ParseValue(out, 0)) return false;
        SkipWhitespace();
        return pos_ == text_.size();
    }

private:
    static constexpr size_t kMaxDepth = 32;

    std::string_view text_;
    size_t pos_{};

    bool AtEnd() const { return pos_ >= text_.size(); }
    char Peek() const { return text_[pos_]; }

    void SkipWhitespace()
    {
        while (!AtEnd() && (Peek() == ' ' || Peek() == '\t' || Peek() == '\n' || Peek() == '\r')) ++pos_;
    }

    bool Consume(char expected)
    {
        if (AtEnd() || Peek() != expected) return false;
        ++pos_;
        return true;
    }

    bool ConsumeLiteral(std::string_view literal)
    {
        if (text_.substr(pos_, literal.size()) != literal) return false;
        pos_ += literal.size();
        return true;
    }

    bool ParseValue(JsonValue& out, size_t depth)
    {
        if (AtEnd() || depth > kMaxDepth) return false;
        switch (Peek()) {
            case '{': return ParseObject(out, depth);
            case '[': return ParseArray(out, depth);
            case '"':
                out.kind = JsonValue::Kind::String;
                return ParseString(out.text);
            case 't':
                out.kind = JsonValue::Kind::Bool;
                out.boolean = true;
                return ConsumeLiteral("true");
            case 'f':
                out.kind = JsonValue::Kind::Bool;
                out.boolean = false;
                return ConsumeLiteral("false");
            case 'n':
                out.kind = JsonValue::Kind::Null;
                return ConsumeLiteral("null");
            default: return ParseNumber(out);
        }
    }

    bool ParseObject(JsonValue& out, size_t depth)
    {
        out.kind = JsonValue::Kind::Object;
        if (!Consume('{')) return false;
        SkipWhitespace();
        if (Consume('}')) return true;
        for (;;) {
            SkipWhitespace();
            std::string key;
            if (AtEnd() || Peek() != '"' || !ParseString(key)) return false;
            SkipWhitespace();
            if (!Consume(':')) return false;
            SkipWhitespace();
            JsonValue value;
            if (!ParseValue(value, depth + 1)) return false;
            out.keys.push_back(std::move(key));
            out.items.push_back(std::move(value));
            SkipWhitespace();
            if (Consume(',')) continue;
            return Consume('}');
        }
    }

    bool ParseArray(JsonValue& out, size_t depth)
    {
        out.kind = JsonValue::Kind::Array;
        if (!Consume('[')) return false;
        SkipWhitespace();
        if (Consume(']')) return true;
        for (;;) {
            SkipWhitespace();
            JsonValue value;
            if (!ParseValue(value, depth + 1)) return false;
            out.items.push_back(std::move(value));
            SkipWhitespace();
            if (Consume(',')) continue;
            return Consume(']');
        }
    }

    bool ParseHex4(uint32_t& code)
    {
        if (pos_ + 4 > text_.size()) return false;
        code = 0;
        for (size_t index = 0; index < 4; ++index) {
            const char c = text_[pos_ + index];
            uint32_t digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(c - 'A' + 10);
            else return false;
            code = (code << 4) | digit;
        }
        pos_ += 4;
        return true;
    }

    static void AppendUtf8(std::string& out, uint32_t code)
    {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool ParseString(std::string& out)
    {
        if (!Consume('"')) return false;
        for (;;) {
            if (AtEnd()) return false;
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false; // raw control characters are invalid
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (AtEnd()) return false;
            const char escape = text_[pos_++];
            switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t code = 0;
                    if (!ParseHex4(code)) return false;
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        uint32_t low = 0;
                        if (!ConsumeLiteral("\\u") || !ParseHex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        return false; // lone low surrogate
                    }
                    AppendUtf8(out, code);
                    break;
                }
                default: return false;
            }
        }
    }

    bool ParseNumber(JsonValue& out)
    {
        out.kind = JsonValue::Kind::Number;
        const size_t start = pos_;
        Consume('-');
        if (AtEnd()) return false;
        if (Peek() == '0') {
            ++pos_;
        } else if (Peek() >= '1' && Peek() <= '9') {
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek()))) ++pos_;
        } else {
            return false;
        }
        if (Consume('.')) {
            const size_t fractionStart = pos_;
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek()))) ++pos_;
            if (pos_ == fractionStart) return false;
        }
        if (!AtEnd() && (Peek() == 'e' || Peek() == 'E')) {
            ++pos_;
            if (!Consume('+')) Consume('-');
            const size_t exponentStart = pos_;
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek()))) ++pos_;
            if (pos_ == exponentStart) return false;
        }
        out.text.assign(text_.substr(start, pos_ - start));
        return true;
    }
};

} // namespace strict_json
