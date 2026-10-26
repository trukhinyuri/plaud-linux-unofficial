// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Minimal JSON DOM for compositor IPC replies (Hyprland, sway).

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace plaud_rec {
namespace screen {

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Type type() const { return type_; }
  bool IsNull() const { return type_ == Type::Null; }
  bool IsNumber() const { return type_ == Type::Number; }
  bool IsString() const { return type_ == Type::String; }
  bool IsArray() const { return type_ == Type::Array; }
  bool IsObject() const { return type_ == Type::Object; }

  // Member lookup on objects; nullptr when absent or not an object.
  const Json* Get(const std::string& key) const;
  const std::vector<Json>& Items() const { return array_; }

  double Number(double fallback = 0.0) const { return type_ == Type::Number ? number_ : fallback; }
  bool Bool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
  const std::string& String() const { return string_; }

  // Convenience accessors for object members.
  double NumberAt(const std::string& key, double fallback = 0.0) const;
  bool BoolAt(const std::string& key, bool fallback = false) const;
  std::string StringAt(const std::string& key) const;

  static bool Parse(const std::string& text, Json& out, std::string& error);

 private:
  friend class JsonParser;
  Type type_ = Type::Null;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  std::vector<Json> array_;
  std::vector<std::pair<std::string, Json>> object_;
};

}  // namespace screen
}  // namespace plaud_rec
