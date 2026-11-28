#ifndef VASTNOVA_TYPES_H
#define VASTNOVA_TYPES_H

#include <cctype>
#include <memory>
#include <string>

namespace vastnova {

constexpr unsigned kMaxIntegerBits = 65535;

struct TypeInfo {
    enum Kind {
        Invalid,
        Void,
        Int,
        Float,
        Str,
        Array
    };

    Kind kind = Invalid;
    unsigned bits = 0;
    bool isUnsigned = false;
    std::shared_ptr<TypeInfo> elem;
    unsigned count = 0;

    static TypeInfo invalid() { return TypeInfo{}; }

    static TypeInfo voidType() {
        TypeInfo t;
        t.kind = Void;
        return t;
    }

    static TypeInfo intType(unsigned width, bool unsignedType) {
        TypeInfo t;
        t.kind = Int;
        t.bits = width;
        t.isUnsigned = unsignedType;
        return t;
    }

    static TypeInfo floatType(unsigned width) {
        TypeInfo t;
        t.kind = Float;
        t.bits = width;
        return t;
    }

    static TypeInfo strType() {
        TypeInfo t;
        t.kind = Str;
        return t;
    }

    static TypeInfo arrayType(const TypeInfo& element, unsigned length) {
        TypeInfo t;
        t.kind = Array;
        t.elem = std::make_shared<TypeInfo>(element);
        t.count = length;
        return t;
    }

    bool isInteger() const { return kind == Int; }
    bool isFloat() const { return kind == Float; }
    bool isNumeric() const { return kind == Int || kind == Float; }
    bool isArray() const { return kind == Array; }

    bool operator==(const TypeInfo& other) const {
        if (kind != other.kind) return false;
        if (kind == Int)
            return bits == other.bits && isUnsigned == other.isUnsigned;
        if (kind == Float)
            return bits == other.bits;
        if (kind == Array)
            return count == other.count && elem && other.elem && *elem == *other.elem;
        return true;
    }

    bool operator!=(const TypeInfo& other) const { return !(*this == other); }

    std::string name() const {
        switch (kind) {
            case Int:   return std::string(isUnsigned ? "u" : "i") + std::to_string(bits);
            case Float: return "f" + std::to_string(bits);
            case Str:   return "str";
            case Void:  return "void";
            case Array:
                return (elem ? elem->name() : std::string("void")) +
                       "[" + std::to_string(count) + "]";
            default:    return "<invalid>";
        }
    }
};

inline bool isNumericTypeWord(const std::string& s) {
    if (s.size() < 2) return false;
    if (s[0] != 'i' && s[0] != 'u' && s[0] != 'f') return false;
    for (size_t i = 1; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

inline bool parseTypeInfo(const std::string& text, TypeInfo& out, std::string& error) {
    size_t open = text.find('[');
    if (open != std::string::npos) {
        if (text.empty() || text.back() != ']') {
            error = "malformed array type '" + text + "'";
            return false;
        }
        size_t close = text.find(']', open);
        if (close != text.size() - 1) {
            error = "malformed array type '" + text + "'";
            return false;
        }
        std::string length = text.substr(open + 1, close - open - 1);
        std::string elementName = text.substr(0, open);
        if (elementName.find('[') != std::string::npos) {
            error = "nested arrays are not supported";
            return false;
        }
        if (length.empty()) {
            error = "array length must be a positive integer";
            return false;
        }
        unsigned long long lengthValue = 0;
        for (char c : length) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                error = "array length must be a positive integer";
                return false;
            }
            lengthValue = lengthValue * 10 + static_cast<unsigned>(c - '0');
            if (lengthValue > 0xFFFFFFFFull) {
                error = "array length is too large";
                return false;
            }
        }
        if (lengthValue == 0) {
            error = "array length must be greater than zero";
            return false;
        }
        TypeInfo element;
        if (!parseTypeInfo(elementName, element, error)) return false;
        if (element.kind == TypeInfo::Void) {
            error = "array element type cannot be void";
            return false;
        }
        out = TypeInfo::arrayType(element, static_cast<unsigned>(lengthValue));
        return true;
    }

    if (text == "str") {
        out = TypeInfo::strType();
        return true;
    }
    if (text == "void") {
        out = TypeInfo::voidType();
        return true;
    }
    if (!isNumericTypeWord(text)) {
        error = "unknown type '" + text + "'";
        return false;
    }

    char prefix = text[0];
    unsigned long long width = 0;
    for (size_t i = 1; i < text.size(); ++i) {
        width = width * 10 + static_cast<unsigned>(text[i] - '0');
        if (width > 1000000ull) break;
    }

    if (prefix == 'f') {
        if (width != 16 && width != 32 && width != 64 && width != 128) {
            error = "floating-point type '" + text +
                    "' is not supported; use f16, f32, f64, or f128";
            return false;
        }
        out = TypeInfo::floatType(static_cast<unsigned>(width));
        return true;
    }

    if (width == 0) {
        error = "integer width must be a positive integer";
        return false;
    }
    if (width > kMaxIntegerBits) {
        error = "integer width must be at most " + std::to_string(kMaxIntegerBits) + " bits";
        return false;
    }
    out = TypeInfo::intType(static_cast<unsigned>(width), prefix == 'u');
    return true;
}

} // namespace vastnova

#endif
