#pragma once
#include <string>
#include <utility>
#include <vector>

// Minimal JSON reader for Spotify API responses (strings come out as UTF-8).
namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    // Missing keys / indices and wrong types give a shared null value, so chains like
    // v["item"]["album"]["images"][0]["url"].Str() never throw.
    const Value& operator[](const char* key) const;
    const Value& operator[](size_t index) const;
    size_t Size() const {
        return type == Type::Array ? array.size() : type == Type::Object ? object.size() : 0;
    }
    bool IsNull() const { return type == Type::Null; }
    std::string Str(const std::string& fallback = std::string()) const;
    double Num(double fallback = 0) const { return type == Type::Number ? number : fallback; }
    bool Bool(bool fallback = false) const { return type == Type::Bool ? boolean : fallback; }
};

bool Parse(const std::string& text, Value* out);

// JSON string literal (with quotes) for building request bodies.
std::string Quote(const std::string& utf8);

}  // namespace json
