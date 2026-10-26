// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
#include "json.h"

#include <cmath>
#include <cstdlib>

namespace am {
namespace {

const std::string kEmptyString;
const std::vector<JsonValue> kEmptyArray;
const std::map<std::string, JsonValue> kEmptyObject;
const JsonValue kNull;

constexpr int kMaxDepth = 256;

class Parser {
 public:
  explicit Parser(const std::string& text) : s_(text) {}

  bool parse(JsonValue& out, std::string* error) {
    skipWs();
    if (!parseValue(out, 0)) {
      if (error) *error = error_ + " at offset " + std::to_string(pos_);
      return false;
    }
    skipWs();
    if (pos_ != s_.size()) {
      if (error) *error = "trailing characters at offset " + std::to_string(pos_);
      return false;
    }
    return true;
  }

 private:
  bool fail(const char* msg) {
    if (error_.empty()) error_ = msg;
    return false;
  }

  void skipWs() {
    while (pos_ < s_.size()) {
      char c = s_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool literal(const char* word) {
    size_t n = 0;
    while (word[n] != '\0') ++n;
    if (s_.compare(pos_, n, word) != 0) return fail("invalid literal");
    pos_ += n;
    return true;
  }

  bool parseValue(JsonValue& out, int depth) {
    if (depth > kMaxDepth) return fail("nesting too deep");
    if (pos_ >= s_.size()) return fail("unexpected end of input");
    char c = s_[pos_];
    switch (c) {
      case '{':
        return parseObject(out, depth);
      case '[':
        return parseArray(out, depth);
      case '"': {
        std::string str;
        if (!parseString(str)) return false;
        out = JsonValue::makeString(std::move(str));
        return true;
      }
      case 't':
        if (!literal("true")) return false;
        out = JsonValue::makeBool(true);
        return true;
      case 'f':
        if (!literal("false")) return false;
        out = JsonValue::makeBool(false);
        return true;
      case 'n':
        if (!literal("null")) return false;
        out = JsonValue();
        return true;
      default:
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
        return fail("unexpected character");
    }
  }

  bool parseNumber(JsonValue& out) {
    size_t start = pos_;
    if (s_[pos_] == '-') ++pos_;
    if (pos_ >= s_.size()) return fail("bad number");
    if (s_[pos_] == '0') {
      ++pos_;
    } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
    } else {
      return fail("bad number");
    }
    if (pos_ < s_.size() && s_[pos_] == '.') {
      ++pos_;
      size_t digits = pos_;
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
      if (pos_ == digits) return fail("bad fraction");
    }
    if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
      size_t digits = pos_;
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
      if (pos_ == digits) return fail("bad exponent");
    }
    std::string num = s_.substr(start, pos_ - start);
    char* end = nullptr;
    double d = std::strtod(num.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(d)) return fail("bad number");
    out = JsonValue::makeNumber(d);
    return true;
  }

  static void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool parseHex4(uint32_t& v) {
    if (pos_ + 4 > s_.size()) return fail("truncated \\u escape");
    v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = s_[pos_++];
      v <<= 4;
      if (c >= '0' && c <= '9') {
        v |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        v |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        v |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return fail("bad \\u escape");
      }
    }
    return true;
  }

  bool parseString(std::string& out) {
    ++pos_;  // opening quote
    while (true) {
      if (pos_ >= s_.size()) return fail("unterminated string");
      char c = s_[pos_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= s_.size()) return fail("unterminated escape");
      char e = s_[pos_++];
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
          uint32_t cp = 0;
          if (!parseHex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            // High surrogate: a low surrogate must follow.
            if (pos_ + 2 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
              pos_ += 2;
              uint32_t lo = 0;
              if (!parseHex4(lo)) return false;
              if (lo < 0xDC00 || lo > 0xDFFF) return fail("bad surrogate pair");
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              return fail("lone surrogate");
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return fail("lone surrogate");
          }
          appendUtf8(out, cp);
          break;
        }
        default:
          return fail("bad escape");
      }
    }
  }

  bool parseArray(JsonValue& out, int depth) {
    ++pos_;
    std::vector<JsonValue> items;
    skipWs();
    if (pos_ < s_.size() && s_[pos_] == ']') {
      ++pos_;
      out = JsonValue::makeArray(std::move(items));
      return true;
    }
    while (true) {
      skipWs();
      JsonValue v;
      if (!parseValue(v, depth + 1)) return false;
      items.push_back(std::move(v));
      skipWs();
      if (pos_ >= s_.size()) return fail("unterminated array");
      char c = s_[pos_++];
      if (c == ']') break;
      if (c != ',') return fail("expected ',' or ']'");
    }
    out = JsonValue::makeArray(std::move(items));
    return true;
  }

  bool parseObject(JsonValue& out, int depth) {
    ++pos_;
    std::map<std::string, JsonValue> members;
    skipWs();
    if (pos_ < s_.size() && s_[pos_] == '}') {
      ++pos_;
      out = JsonValue::makeObject(std::move(members));
      return true;
    }
    while (true) {
      skipWs();
      if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected object key");
      std::string key;
      if (!parseString(key)) return false;
      skipWs();
      if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected ':'");
      ++pos_;
      skipWs();
      JsonValue v;
      if (!parseValue(v, depth + 1)) return false;
      members[std::move(key)] = std::move(v);
      skipWs();
      if (pos_ >= s_.size()) return fail("unterminated object");
      char c = s_[pos_++];
      if (c == '}') break;
      if (c != ',') return fail("expected ',' or '}'");
    }
    out = JsonValue::makeObject(std::move(members));
    return true;
  }

  const std::string& s_;
  size_t pos_ = 0;
  std::string error_;
};

}  // namespace

JsonValue JsonValue::makeBool(bool b) {
  JsonValue v;
  v.type_ = Type::Bool;
  v.bool_ = b;
  return v;
}

JsonValue JsonValue::makeNumber(double d) {
  JsonValue v;
  v.type_ = Type::Number;
  v.number_ = d;
  return v;
}

JsonValue JsonValue::makeString(std::string s) {
  JsonValue v;
  v.type_ = Type::String;
  v.string_ = std::make_shared<const std::string>(std::move(s));
  return v;
}

JsonValue JsonValue::makeArray(std::vector<JsonValue> items) {
  JsonValue v;
  v.type_ = Type::Array;
  v.array_ = std::make_shared<const std::vector<JsonValue>>(std::move(items));
  return v;
}

JsonValue JsonValue::makeObject(std::map<std::string, JsonValue> members) {
  JsonValue v;
  v.type_ = Type::Object;
  v.object_ = std::make_shared<const std::map<std::string, JsonValue>>(std::move(members));
  return v;
}

const std::string& JsonValue::asString() const {
  return type_ == Type::String ? *string_ : kEmptyString;
}

const std::vector<JsonValue>& JsonValue::items() const {
  return type_ == Type::Array ? *array_ : kEmptyArray;
}

const std::map<std::string, JsonValue>& JsonValue::members() const {
  return type_ == Type::Object ? *object_ : kEmptyObject;
}

const JsonValue& JsonValue::operator[](const std::string& key) const {
  if (type_ != Type::Object) return kNull;
  auto it = object_->find(key);
  return it == object_->end() ? kNull : it->second;
}

std::string JsonValue::stringOr(const std::string& key, const std::string& fallback) const {
  const JsonValue& v = (*this)[key];
  return v.isString() ? v.asString() : fallback;
}

int64_t JsonValue::intOr(const std::string& key, int64_t fallback) const {
  const JsonValue& v = (*this)[key];
  if (v.isNumber()) return static_cast<int64_t>(v.asNumber());
  if (v.isString()) {
    const std::string& s = v.asString();
    char* end = nullptr;
    long long n = std::strtoll(s.c_str(), &end, 10);
    if (!s.empty() && end != nullptr && *end == '\0') return n;
  }
  return fallback;
}

bool parseJson(const std::string& text, JsonValue& out, std::string* error) {
  Parser p(text);
  return p.parse(out, error);
}

}  // namespace am
