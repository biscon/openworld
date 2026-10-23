#pragma once
#include <cmath>
#include <stdexcept>
#include <string>
inline void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
inline void near(double actual, double expected, double tolerance, const char *message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
        throw std::runtime_error(std::string(message) + ": " + std::to_string(actual) +
                                 " != " + std::to_string(expected));
}
template <class F> void rejects(F &&operation, const char *message) {
    try {
        operation();
    } catch (const std::invalid_argument &) {
        return;
    }
    throw std::runtime_error(message);
}
