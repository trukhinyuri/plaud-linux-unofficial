// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Minimal JSON reader used for compositor IPC replies (Hyprland, Sway),
// PipeWire metadata values ({"name":"..."}) and the window-list override file.
// Read-only DOM, strict RFC 8259 syntax, UTF-8 passthrough, \uXXXX decoded.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace am {

class JsonValue {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  JsonValue() = default;
  static JsonValue makeBool(bool b);
  static JsonValue makeNumber(double d);
  static JsonValue makeString(std::string s);
  static JsonValue makeArray(std::vector<JsonValue> items);
  static JsonValue makeObject(std::map<std::string, JsonValue> members);

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }
  bool isBool() const { return type_ == Type::Bool; }
  bool isNumber() const { return type_ == Type::Number; }
  bool isString() const { return type_ == Type::String; }
  bool isArray() const { return type_ == Type::Array; }
  bool isObject() const { return type_ == Type::Object; }

  bool asBool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
  double asNumber(double fallback = 0) const { return type_ == Type::Number ? number_ : fallback; }
  const std::string& asString() const;
  const std::vector<JsonValue>& items() const;
  const std::map<std::string, JsonValue>& members() const;

  // Object member lookup; returns a shared Null value when missing.
  const JsonValue& operator[](const std::string& key) const;
  std::string stringOr(const std::string& key, const std::string& fallback = std::string()) const;
  int64_t intOr(const std::string& key, int64_t fallback) const;

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double number_ = 0;
  std::shared_ptr<const std::string> string_;
  std::shared_ptr<const std::vector<JsonValue>> array_;
  std::shared_ptr<const std::map<std::string, JsonValue>> object_;
};

// Parses `text`. On syntax error returns false and fills `error`.
bool parseJson(const std::string& text, JsonValue& out, std::string* error = nullptr);

}  // namespace am
