// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

#include "json.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace plaud_rec {
namespace screen {

class JsonParser {
 public:
  explicit JsonParser(const std::string& text) : s_(text) {}

  bool ParseDocument(Json& out, std::string& error) {
    SkipSpace();
    if (!ParseValue(out, 0)) {
      error = error_.empty() ? "invalid JSON" : error_;
      error += " at offset " + std::to_string(pos_);
      return false;
    }
    SkipSpace();
    if (pos_ != s_.size()) {
      error = "trailing characters after JSON value at offset " + std::to_string(pos_);
      return false;
    }
    return true;
  }

 private:
  static constexpr int kMaxDepth = 256;

  void SkipSpace() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
      pos_++;
    }
  }

  bool Fail(const char* message) {
    if (error_.empty()) error_ = message;
    return false;
  }

  bool Literal(const char* word) {
    const size_t n = std::strlen(word);
    if (s_.compare(pos_, n, word) != 0) return Fail("invalid literal");
    pos_ += n;
    return true;
  }

  bool ParseValue(Json& v, int depth) {
    if (depth > kMaxDepth) return Fail("JSON nesting too deep");
    if (pos_ >= s_.size()) return Fail("unexpected end of JSON");
    const char c = s_[pos_];
    if (c == '{') return ParseObject(v, depth);
    if (c == '[') return ParseArray(v, depth);
    if (c == '"') {
      v.type_ = Json::Type::String;
      return ParseString(v.string_);
    }
    if (c == 't') {
      v.type_ = Json::Type::Bool;
      v.bool_ = true;
      return Literal("true");
    }
    if (c == 'f') {
      v.type_ = Json::Type::Bool;
      v.bool_ = false;
      return Literal("false");
    }
    if (c == 'n') {
      v.type_ = Json::Type::Null;
      return Literal("null");
    }
    if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(v);
    return Fail("unexpected character");
  }

  bool ParseNumber(Json& v) {
    const size_t start = pos_;
    if (s_[pos_] == '-') pos_++;
    while (pos_ < s_.size() && std::strchr("0123456789.eE+-", s_[pos_]) != nullptr) pos_++;
    const std::string token = s_.substr(start, pos_ - start);
    char* end = nullptr;
    const double d = std::strtod(token.c_str(), &end);
    if (end == nullptr || *end != '\0' || token.empty()) return Fail("invalid number");
    v.type_ = Json::Type::Number;
    v.number_ = d;
    return true;
  }

  static void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  bool ParseHex4(uint32_t& cp) {
    if (pos_ + 4 > s_.size()) return Fail("truncated \\u escape");
    cp = 0;
    for (int i = 0; i < 4; i++) {
      const char h = s_[pos_++];
      cp <<= 4;
      if (h >= '0' && h <= '9') {
        cp |= static_cast<uint32_t>(h - '0');
      } else if (h >= 'a' && h <= 'f') {
        cp |= static_cast<uint32_t>(h - 'a' + 10);
      } else if (h >= 'A' && h <= 'F') {
        cp |= static_cast<uint32_t>(h - 'A' + 10);
      } else {
        return Fail("invalid \\u escape");
      }
    }
    return true;
  }

  bool ParseString(std::string& out) {
    pos_++;  // opening quote
    out.clear();
    while (pos_ < s_.size()) {
      const char c = s_[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= s_.size()) break;
      const char e = s_[pos_++];
      switch (e) {
        case '"':
        case '\\':
        case '/':
          out += e;
          break;
        case 'b':
          out += '\b';
          break;
        case 'f':
          out += '\f';
          break;
        case 'n':
          out += '\n';
          break;
        case 'r':
          out += '\r';
          break;
        case 't':
          out += '\t';
          break;
        case 'u': {
          uint32_t cp = 0;
          if (!ParseHex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
            const size_t save = pos_;
            pos_ += 2;
            uint32_t lo = 0;
            if (ParseHex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              pos_ = save;
              error_.clear();
              cp = 0xFFFD;
            }
          } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
          }
          AppendUtf8(out, cp);
          break;
        }
        default:
          return Fail("invalid string escape");
      }
    }
    return Fail("unterminated string");
  }

  bool ParseArray(Json& v, int depth) {
    pos_++;
    v.type_ = Json::Type::Array;
    SkipSpace();
    if (pos_ < s_.size() && s_[pos_] == ']') {
      pos_++;
      return true;
    }
    for (;;) {
      SkipSpace();
      v.array_.emplace_back();
      if (!ParseValue(v.array_.back(), depth + 1)) return false;
      SkipSpace();
      if (pos_ >= s_.size()) return Fail("unterminated array");
      if (s_[pos_] == ',') {
        pos_++;
        continue;
      }
      if (s_[pos_] == ']') {
        pos_++;
        return true;
      }
      return Fail("expected ',' or ']'");
    }
  }

  bool ParseObject(Json& v, int depth) {
    pos_++;
    v.type_ = Json::Type::Object;
    SkipSpace();
    if (pos_ < s_.size() && s_[pos_] == '}') {
      pos_++;
      return true;
    }
    for (;;) {
      SkipSpace();
      if (pos_ >= s_.size() || s_[pos_] != '"') return Fail("expected object key");
      std::string key;
      if (!ParseString(key)) return false;
      SkipSpace();
      if (pos_ >= s_.size() || s_[pos_] != ':') return Fail("expected ':'");
      pos_++;
      SkipSpace();
      v.object_.emplace_back(std::move(key), Json());
      if (!ParseValue(v.object_.back().second, depth + 1)) return false;
      SkipSpace();
      if (pos_ >= s_.size()) return Fail("unterminated object");
      if (s_[pos_] == ',') {
        pos_++;
        continue;
      }
      if (s_[pos_] == '}') {
        pos_++;
        return true;
      }
      return Fail("expected ',' or '}'");
    }
  }

  const std::string& s_;
  size_t pos_ = 0;
  std::string error_;
};

const Json* Json::Get(const std::string& key) const {
  if (type_ != Type::Object) return nullptr;
  for (const auto& kv : object_) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

double Json::NumberAt(const std::string& key, double fallback) const {
  const Json* v = Get(key);
  return v != nullptr ? v->Number(fallback) : fallback;
}

bool Json::BoolAt(const std::string& key, bool fallback) const {
  const Json* v = Get(key);
  return v != nullptr ? v->Bool(fallback) : fallback;
}

std::string Json::StringAt(const std::string& key) const {
  const Json* v = Get(key);
  return (v != nullptr && v->IsString()) ? v->String() : std::string();
}

bool Json::Parse(const std::string& text, Json& out, std::string& error) {
  JsonParser parser(text);
  Json v;
  if (!parser.ParseDocument(v, error)) return false;
  out = std::move(v);
  return true;
}

}  // namespace screen
}  // namespace plaud_rec
