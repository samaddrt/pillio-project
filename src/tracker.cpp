// tracker.cpp — генерация слотов, расчёт времени следующего приёма.

#include "tracker.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace pillio {

// ISO 8601: "2025-06-15T08:00:00"
std::string formatTimePoint(const TimePoint& tp) {
    auto time_c = Clock::to_time_t(tp);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &time_c);
#else
    localtime_r(&time_c, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
    return oss.str();
}

TimePoint parseTimePoint(const std::string& iso) {
    std::tm tm_buf{};
    std::istringstream iss(iso);
    iss >> std::get_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
    if (iss.fail()) {
        throw ValidationError("Cannot parse ISO time: " + iso);
    }
    tm_buf.tm_isdst = -1;  // автоопределение летнего времени
    auto time_c = std::mktime(&tm_buf);
    if (time_c == static_cast<std::time_t>(-1)) {
        throw ValidationError("mktime failed for: " + iso);
    }
    return Clock::from_time_t(time_c);
}

// Возвращает начало суток (00:00:00) для заданной точки времени.
// Используется только внутри этого файла (генерация слотов).
static TimePoint startOfDay(const TimePoint& tp) {
    auto time_c = Clock::to_time_t(tp);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &time_c);
#else
    localtime_r(&time_c, &tm_buf);
#endif
    tm_buf.tm_hour = 0;
    tm_buf.tm_min = 0;
    tm_buf.tm_sec = 0;
    return Clock::from_time_t(std::mktime(&tm_buf));
}

std::vector<Schedule> generateDailySlots(const Pill& pill, const TimePoint& day) {
    pill.validate();

    auto midnight = startOfDay(day);
    std::vector<Schedule> slots;

    auto first =
        midnight + std::chrono::hours(pill.start_hour) + std::chrono::minutes(pill.start_minute);

    auto end_of_day = midnight + std::chrono::hours(24);

    for (auto slot_time = first; slot_time < end_of_day;
         slot_time += std::chrono::hours(pill.interval_hours)) {
        Schedule entry;
        entry.pill_id = pill.id;
        entry.scheduled_time = formatTimePoint(slot_time);
        entry.taken = false;
        entry.taken_at = "";
        slots.push_back(std::move(entry));
    }

    return slots;
}

// Находит ближайший непринятый слот (сегодня или завтра).
TimePoint calculateNextIntake(const Pill& pill, const std::vector<Schedule>& history,
                              const TimePoint& now) {
    pill.validate();

    auto isSlotTaken = [&history](std::uint64_t pid, const std::string& slot_iso) -> bool {
        return std::any_of(history.begin(), history.end(), [&](const Schedule& s) {
            return s.pill_id == pid && s.scheduled_time == slot_iso && s.taken;
        });
    };

    for (int day_offset = 0; day_offset <= 1; ++day_offset) {
        auto day = now + std::chrono::hours(24 * day_offset);
        auto slots = generateDailySlots(pill, day);

        for (const auto& slot : slots) {
            auto slot_tp = parseTimePoint(slot.scheduled_time);

            if (day_offset == 0 && slot_tp <= now) {
                continue;
            }

            if (isSlotTaken(pill.id, slot.scheduled_time)) {
                continue;
            }

            return slot_tp;
        }
    }

    // Fallback: послезавтра
    auto day_after = now + std::chrono::hours(48);
    auto fallback_slots = generateDailySlots(pill, day_after);
    if (!fallback_slots.empty()) {
        return parseTimePoint(fallback_slots.front().scheduled_time);
    }

    throw ValidationError("Cannot compute next intake for: " + pill.name);
}

double dailyProgress(const std::vector<Schedule>& schedules) {
    if (schedules.empty()) {
        return 0.0;
    }
    auto taken_count = std::count_if(schedules.begin(), schedules.end(),
                                     [](const Schedule& s) { return s.taken; });
    return static_cast<double>(taken_count) / static_cast<double>(schedules.size());
}

}  // namespace pillio
