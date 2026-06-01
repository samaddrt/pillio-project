// storage.cpp — JSON-файл как база данных.

#include "storage.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace pillio {

Storage::Storage(const std::filesystem::path& db_path) : db_path_(db_path) {
    try {
        auto parent = db_path_.parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent)) {
            std::filesystem::create_directories(parent);
        }
    } catch (const std::filesystem::filesystem_error& e) {
        throw StorageError(std::string("Cannot create DB directory: ") + e.what());
    }

    if (!std::filesystem::exists(db_path_)) {
        nlohmann::json initial = {{"pills", nlohmann::json::array()},
                                  {"schedules", nlohmann::json::array()}};
        save(initial);
    }
}

// ─── Private helpers ────────────────────────────────────────────

nlohmann::json Storage::load() const {
    std::ifstream ifs(db_path_);
    if (!ifs.is_open()) {
        throw StorageError("Cannot open DB file: " + db_path_.string());
    }
    try {
        nlohmann::json data;
        ifs >> data;
        return data;
    } catch (const nlohmann::json::parse_error& e) {
        throw StorageError(std::string("Corrupted DB file: ") + e.what());
    }
}

void Storage::save(const nlohmann::json& data) const {
    // Атомарная запись: временный файл + rename. Защищает от повреждения
    // при падении и от чтения наполовину записанного файла внешним бэкапом.
    std::filesystem::path tmp = db_path_;
    tmp += ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::trunc);
        if (!ofs.is_open()) {
            throw StorageError("Cannot write DB file: " + db_path_.string());
        }
        ofs << data.dump(2);
        if (ofs.fail()) {
            throw StorageError("Write failed for: " + db_path_.string());
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, db_path_, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        throw StorageError("Atomic rename failed for: " + db_path_.string());
    }
}

std::uint64_t Storage::nextPillId(const nlohmann::json& data) const {
    std::uint64_t max_id = 0;
    if (data.contains("pills")) {
        for (const auto& p : data["pills"]) {
            auto id = p.value("id", static_cast<std::uint64_t>(0));
            if (id > max_id) max_id = id;
        }
    }
    return max_id + 1;
}

// ─── Pill CRUD ──────────────────────────────────────────────────

std::vector<Pill> Storage::getAllPills() const {
    auto data = load();
    std::vector<Pill> result;
    if (data.contains("pills")) {
        result = data["pills"].get<std::vector<Pill>>();
    }
    return result;
}

Pill Storage::getPillById(std::uint64_t id) const {
    auto pills = getAllPills();
    auto it = std::find_if(pills.begin(), pills.end(),
                           [id](const Pill& p) { return p.id == id; });
    if (it == pills.end()) {
        throw NotFoundError("Pill not found: id=" + std::to_string(id));
    }
    return *it;
}

Pill Storage::addPill(Pill pill) {
    pill.validate();
    auto data = load();
    pill.id = nextPillId(data);
    data["pills"].push_back(pill);
    save(data);
    return pill;
}

void Storage::removePill(std::uint64_t id) {
    auto data = load();
    auto& pills = data["pills"];
    auto it = std::remove_if(pills.begin(), pills.end(),
                             [id](const nlohmann::json& j) {
                                 return j.value("id", static_cast<std::uint64_t>(0)) == id;
                             });
    if (it == pills.end()) {
        throw NotFoundError("Pill not found for removal: id=" + std::to_string(id));
    }
    pills.erase(it, pills.end());

    // Удаляем связанные записи расписания
    auto& scheds = data["schedules"];
    scheds.erase(
        std::remove_if(scheds.begin(), scheds.end(),
                       [id](const nlohmann::json& j) {
                           return j.value("pill_id", static_cast<std::uint64_t>(0)) == id;
                       }),
        scheds.end());

    save(data);
}

// ─── Schedule CRUD ──────────────────────────────────────────────

std::vector<Schedule> Storage::getAllSchedules() const {
    auto data = load();
    std::vector<Schedule> result;
    if (data.contains("schedules")) {
        result = data["schedules"].get<std::vector<Schedule>>();
    }
    return result;
}

std::vector<Schedule> Storage::getSchedulesForDate(const std::string& date_prefix) const {
    auto all = getAllSchedules();
    std::vector<Schedule> filtered;
    std::copy_if(all.begin(), all.end(), std::back_inserter(filtered),
                 [&date_prefix](const Schedule& s) {
                     return s.scheduled_time.rfind(date_prefix, 0) == 0;
                 });
    return filtered;
}

void Storage::addSchedule(Schedule entry) {
    entry.validate();
    auto data = load();
    data["schedules"].push_back(entry);
    save(data);
}

void Storage::markTaken(std::uint64_t pill_id,
                        const std::string& scheduled_time,
                        const std::string& taken_at) {
    auto data = load();
    bool found = false;
    for (auto& s : data["schedules"]) {
        if (s.value("pill_id", static_cast<std::uint64_t>(0)) == pill_id &&
            s.value("scheduled_time", std::string{}) == scheduled_time) {
            s["taken"] = true;
            s["taken_at"] = taken_at;
            found = true;
            break;
        }
    }
    if (!found) {
        throw NotFoundError("Schedule slot not found: pill_id=" +
                            std::to_string(pill_id) + " time=" + scheduled_time);
    }
    save(data);
}

}  // namespace pillio
