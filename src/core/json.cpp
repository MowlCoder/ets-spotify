#include "core/json.hpp"

#include <cstdlib>
#include <cstring>
#include <format>

#include "core/immortal.hpp"

namespace json {
namespace {
const Value& Null() {
    static const Value& kNull = core::Immortal<Value>();
    return kNull;
}

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    bool ParseDocument(Value* out) {
        if (!ParseValue(out, 0)) { return false; }
        SkipSpace();
        return pos_ == s_.size();
    }

private:
    static constexpr int kMaxDepth = 64;

    void SkipSpace() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool Literal(const char* word) {
        const size_t n = strlen(word);
        if (s_.compare(pos_, n, word) != 0) { return false; }
        pos_ += n;
        return true;
    }

    bool ParseValue(Value* out, int depth) {
        if (depth > kMaxDepth) { return false; }
        SkipSpace();
        if (pos_ >= s_.size()) { return false; }
        const char c = s_[pos_];
        if (c == '{') { return ParseObject(out, depth); }
        if (c == '[') { return ParseArray(out, depth); }
        if (c == '"') {
            out->type = Value::Type::String;
            return ParseString(&out->string);
        }
        if (c == 't' && Literal("true")) {
            out->type = Value::Type::Bool;
            out->boolean = true;
            return true;
        }
        if (c == 'f' && Literal("false")) {
            out->type = Value::Type::Bool;
            out->boolean = false;
            return true;
        }
        if (c == 'n' && Literal("null")) {
            out->type = Value::Type::Null;
            return true;
        }
        return ParseNumber(out);
    }

    bool ParseObject(Value* out, int depth) {
        out->type = Value::Type::Object;
        ++pos_;  // {
        SkipSpace();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }

        for (;;) {
            SkipSpace();
            if (pos_ >= s_.size() || s_[pos_] != '"') { return false; }
            std::string key;
            if (!ParseString(&key)) { return false; }
            SkipSpace();
            if (pos_ >= s_.size() || s_[pos_] != ':') { return false; }
            ++pos_;
            Value value;
            if (!ParseValue(&value, depth + 1)) { return false; }
            out->object.emplace_back(std::move(key), std::move(value));
            SkipSpace();
            if (pos_ >= s_.size()) { return false; }
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool ParseArray(Value* out, int depth) {
        out->type = Value::Type::Array;
        ++pos_;  // [
        SkipSpace();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }

        for (;;) {
            Value value;
            if (!ParseValue(&value, depth + 1)) { return false; }
            out->array.push_back(std::move(value));
            SkipSpace();
            if (pos_ >= s_.size()) { return false; }
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool Hex4(unsigned* out) {
        if (pos_ + 4 > s_.size()) { return false; }
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_];
            ++pos_;
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= c - '0';
            } else if (c >= 'a' && c <= 'f') {
                v |= c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                v |= c - 'A' + 10;
            } else {
                return false;
            }
        }
        *out = v;
        return true;
    }

    static void AppendUtf8(std::string* out, unsigned cp) {
        if (cp < 0x80) {
            out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool ParseString(std::string* out) {
        ++pos_;  // opening quote
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            ++pos_;
            if (c == '"') { return true; }
            if (c != '\\') {
                out->push_back(c);
                continue;
            }
            if (pos_ >= s_.size()) { return false; }
            const char e = s_[pos_];
            ++pos_;
            switch (e) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!Hex4(&cp)) { return false; }
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' &&
                        s_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        unsigned low = 0;
                        if (!Hex4(&low)) { return false; }
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool ParseNumber(Value* out) {
        const char* begin = s_.c_str() + pos_;
        char* end = nullptr;
        const double v = strtod(begin, &end);
        if (end == begin) { return false; }
        pos_ += static_cast<size_t>(end - begin);
        out->type = Value::Type::Number;
        out->number = v;
        return true;
    }

    const std::string& s_;
    size_t pos_ = 0;
};
}  // namespace

const Value& Value::operator[](const char* key) const {
    if (type != Type::Object) { return Null(); }
    for (const auto& [k, v] : object) {
        if (k == key) { return v; }
    }
    return Null();
}

const Value& Value::operator[](size_t index) const {
    if (type != Type::Array || index >= array.size()) { return Null(); }
    return array[index];
}

std::string Value::Str(const std::string& fallback) const {
    return type == Type::String ? string : fallback;
}

bool Parse(const std::string& text, Value* out) {
    *out = Value();
    Parser parser(text);
    return parser.ParseDocument(out);
}

std::string Quote(const std::string& utf8) {
    std::string out = "\"";
    for (const char c : utf8) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}",
                                       static_cast<unsigned>(static_cast<unsigned char>(c)));
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace json
