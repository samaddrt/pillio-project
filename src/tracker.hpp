#pragma once
/**
 * @file tracker.hpp
 * @brief Ядро расчёта расписания: слоты, прогресс, следующий приём.
 */

#include <chrono>
#include <ctime>
#include <string>
#include <vector>

#include "models.hpp"

namespace pillio {

/// Псевдоним для системных часов
using Clock = std::chrono::system_clock;
/// Псевдоним для точки времени
using TimePoint = Clock::time_point;

/**
 * @brief Форматирует TimePoint в строку ISO 8601 (локальное время).
 * @param tp точка времени
 * @return строка вида "2025-06-15T08:00:00"
 */
std::string formatTimePoint(const TimePoint& tp);

/**
 * @brief Разбирает строку ISO 8601 в TimePoint.
 * @param iso строка вида "2025-06-15T08:00:00"
 * @return соответствующая точка времени
 * @throws ValidationError если строка не может быть разобрана.
 */
TimePoint parseTimePoint(const std::string& iso);

/**
 * @brief Вычисляет время следующего приёма лекарства.
 *
 * Алгоритм учитывает:
 * - интервал между приёмами (pill.interval_hours);
 * - время начала (pill.start_hour, pill.start_minute);
 * - историю приёмов (history) — пропускает уже принятые слоты;
 * - «сегодняшний» диапазон (00:00 .. 23:59) относительно @p now.
 *
 * @param pill описание лекарства
 * @param history вектор записей расписания (уже существующих)
 * @param now текущий момент времени
 * @return время следующего запланированного приёма
 * @throws ValidationError если pill не проходит валидацию.
 */
TimePoint calculateNextIntake(const Pill& pill, const std::vector<Schedule>& history,
                              const TimePoint& now);

/**
 * @brief Генерирует все слоты приёма на заданный день для одного лекарства.
 *
 * @param pill описание лекарства
 * @param day дата, для которой нужно сгенерировать слоты
 * @return вектор Schedule-записей (taken == false, taken_at пуст)
 */
std::vector<Schedule> generateDailySlots(const Pill& pill, const TimePoint& day);

/**
 * @brief Считает прогресс за день (доля принятых лекарств, 0.0 – 1.0).
 *
 * @param schedules все записи расписания за день
 * @return значение от 0.0 (ничего не принято) до 1.0 (всё принято)
 */
double dailyProgress(const std::vector<Schedule>& schedules);

}  // namespace pillio
