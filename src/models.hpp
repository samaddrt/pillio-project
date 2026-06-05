#pragma once
/**
 * @file models.hpp
 * @brief Доменные модели: Pill, Schedule + JSON-сериализация.
 */

#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace pillio {

/// @brief Ошибка валидации входных данных (некорректные поля).
class ValidationError : public std::runtime_error {
   public:
    explicit ValidationError(const std::string& msg) : std::runtime_error(msg) {}
};

/// @brief Ошибка ввода-вывода хранилища (чтение/запись файла).
class StorageError : public std::runtime_error {
   public:
    explicit StorageError(const std::string& msg) : std::runtime_error(msg) {}
};

/// @brief Запрошенная сущность не найдена.
class NotFoundError : public std::runtime_error {
   public:
    explicit NotFoundError(const std::string& msg) : std::runtime_error(msg) {}
};

// ── Pill — описание одного лекарства ──────────────────────────────

struct Pill {
    std::uint64_t id{0};                ///< Уникальный идентификатор
    std::string name;                   ///< Название лекарства (UTF-8)
    double dosage{0.0};                 ///< Числовое значение дозировки
    std::string unit{"мг"};             ///< Единица измерения (мг, мл, шт.)
    int interval_hours{8};              ///< Интервал между приёмами (часы)
    int start_hour{8};                  ///< Час первого приёма (0–23)
    int start_minute{0};                ///< Минута первого приёма (0–59)
    std::string meal_relation{"none"};  ///< Связь с едой: none/before/during/after
    int course_days{0};                 ///< Длительность курса (0 = бессрочно)

    /// Бросает ValidationError при некорректных значениях.
    void validate() const {
        if (name.empty()) {
            throw ValidationError("Pill name must not be empty");
        }
        if (dosage <= 0.0) {
            throw ValidationError("Dosage must be positive, got " + std::to_string(dosage));
        }
        if (interval_hours <= 0 || interval_hours > 24) {
            throw ValidationError("Interval must be 1..24 h, got " +
                                  std::to_string(interval_hours));
        }
        if (start_hour < 0 || start_hour > 23) {
            throw ValidationError("start_hour must be 0..23, got " + std::to_string(start_hour));
        }
        if (start_minute < 0 || start_minute > 59) {
            throw ValidationError("start_minute must be 0..59, got " +
                                  std::to_string(start_minute));
        }
        if (meal_relation != "none" && meal_relation != "before" && meal_relation != "during" &&
            meal_relation != "after") {
            throw ValidationError("meal_relation must be none/before/during/after");
        }
        if (course_days < 0) {
            throw ValidationError("course_days must be >= 0");
        }
    }
};

/// @brief JSON-сериализация Pill (отсутствующие поля берутся по умолчанию).
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Pill, id, name, dosage, unit, interval_hours,
                                                start_hour, start_minute, meal_relation,
                                                course_days)

// ── Schedule — отметка о конкретном приёме ────────────────────────

struct Schedule {
    std::uint64_t pill_id{0};    ///< ID связанного Pill
    std::string scheduled_time;  ///< Запланированное время (ISO 8601 строка)
    bool taken{false};           ///< Было ли лекарство принято
    std::string taken_at;        ///< Фактическое время приёма (или пусто)

    /// @brief Бросает ValidationError при некорректных полях.
    void validate() const {
        if (pill_id == 0) {
            throw ValidationError("Schedule must reference a valid pill_id");
        }
        if (scheduled_time.empty()) {
            throw ValidationError("scheduled_time must not be empty");
        }
    }
};

/// @brief JSON-сериализация Schedule.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Schedule, pill_id, scheduled_time, taken, taken_at)

}  // namespace pillio
