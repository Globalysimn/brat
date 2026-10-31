#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

struct Json {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;
    using Data = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    Data data = nullptr;

    Json() = default;
    Json(std::nullptr_t) : data(nullptr) {}
    Json(bool v) : data(v) {}
    Json(int v) : data(static_cast<double>(v)) {}
    Json(double v) : data(v) {}
    Json(const char *v) : data(std::string(v)) {}
    Json(std::string v) : data(std::move(v)) {}
    Json(Array v) : data(std::move(v)) {}
    Json(Object v) : data(std::move(v)) {}

    const Json &get(const std::string &key) const {
        static const Json empty;
        const auto *obj = std::get_if<Object>(&data);
        if (!obj) return empty;
        auto it = obj->find(key);
        return it == obj->end() ? empty : it->second;
    }

    std::string string() const {
        const auto *v = std::get_if<std::string>(&data);
        return v ? *v : "";
    }

    int integer(int fallback = 0) const {
        const auto *v = std::get_if<double>(&data);
        return v ? static_cast<int>(*v) : fallback;
    }

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(data); }
    const Array &array() const {
        static const Array empty;
        const auto *v = std::get_if<Array>(&data);
        return v ? *v : empty;
    }
};

Json json_parse(const std::string &text);
std::string json_write(const Json &value);
