// main.cpp — HTTP-сервер Pillio (httplib).
// Запуск: ./pillio --db ./data/store.json [--port 8080]

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

// ── MinGW compatibility hacks for httplib ────────────────────────
// MinGW headers often lack Windows 8 APIs like CreateFile2 and GetAddrInfoExCancel,
// which modern httplib versions assume are available when _WIN32_WINNT >= 0x0602.
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
inline int GetAddrInfoExCancel(HANDLE* x) { return 0; }
#endif
// ─────────────────────────────────────────────────────────────────

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "analytics.hpp"
#include "checker.hpp"
#include "family.hpp"
#include "models.hpp"
#include "storage.hpp"
#include "tracker.hpp"

namespace {

void parseArgs(int argc, char* argv[],
               std::filesystem::path& db_path,
               int& port) {
    db_path = "./data/store.json";
    port = 8080;

    std::vector<std::string> args(argv, argv + argc);
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--db" && i + 1 < args.size()) {
            db_path = args[++i];
        } else if (args[i] == "--port" && i + 1 < args.size()) {
            port = std::stoi(args[++i]);
        } else if (args[i] == "--help") {
            std::cout << "Usage: pillio [--db path] [--port N]\n";
            std::exit(0);
        }
    }

    if (port < 1 || port > 65535) {
        throw pillio::ValidationError("Port must be 1..65535");
    }
}

void jsonResponse(httplib::Response& res,
                  const nlohmann::json& body,
                  int status = 200) {
    res.set_content(body.dump(), "application/json");
    res.status = status;
}

void errorResponse(httplib::Response& res,
                   const std::string& message,
                   int status = 400) {
    nlohmann::json body = {{"error", message}};
    jsonResponse(res, body, status);
}

// Кэш Storage-объектов по uid. Пустой uid → файл из --db (localhost-режим),
// uid=N → profiles/N.json (Telegram multi-tenant).
class ProfileManager {
public:
    explicit ProfileManager(std::filesystem::path default_db)
        : default_db_(std::move(default_db)),
          base_dir_(default_db_.parent_path()) {}

    pillio::Storage& get(const std::string& uid) {
        std::lock_guard<std::mutex> lock(mtx_);
        const std::string key = uid.empty() ? "__default__" : uid;
        auto it = stores_.find(key);
        if (it != stores_.end()) {
            return *it->second;
        }
        std::filesystem::path path =
            uid.empty() ? default_db_
                        : base_dir_ / "profiles" / (uid + ".json");
        auto store = std::make_unique<pillio::Storage>(path);
        auto& ref = *store;
        stores_.emplace(key, std::move(store));
        return ref;
    }

private:
    std::filesystem::path default_db_;
    std::filesystem::path base_dir_;
    std::map<std::string, std::unique_ptr<pillio::Storage>> stores_;
    std::mutex mtx_;
};

// uid из заголовка X-Pillio-Uid или ?uid= (пусто → default profile)
std::string getUid(const httplib::Request& req) {
    if (req.has_header("X-Pillio-Uid")) {
        return req.get_header_value("X-Pillio-Uid");
    }
    if (req.has_param("uid")) {
        return req.get_param_value("uid");
    }
    return "";
}

// Генерирует недостающие слоты расписания на дату для всех лекарств.
void ensureDailySlots(pillio::Storage& st, const std::string& date) {
    auto pills = st.getAllPills();
    for (const auto& pill : pills) {
        auto tp = pillio::parseTimePoint(date + "T12:00:00");
        auto slots = pillio::generateDailySlots(pill, tp);
        auto existing = st.getSchedulesForDate(date);
        for (auto& slot : slots) {
            bool dup = false;
            for (const auto& ex : existing) {
                if (ex.pill_id == slot.pill_id &&
                    ex.scheduled_time == slot.scheduled_time) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                st.addSchedule(slot);
                existing.push_back(slot);
            }
        }
    }
}

/// Считает серию дней подряд со 100% адгезией (смотрит назад до 365 дней).
int computeStreak(pillio::Storage& st) {
    auto now = pillio::Clock::now();
    int streak = 0;
    for (int d = 1; d <= 365; ++d) {
        auto day_str = pillio::formatTimePoint(
            now - std::chrono::hours(24 * d)).substr(0, 10);
        auto day_scheds = st.getSchedulesForDate(day_str);
        if (day_scheds.empty()) break;
        bool ok = std::all_of(day_scheds.begin(), day_scheds.end(),
                              [](const pillio::Schedule& s) { return s.taken; });
        if (ok) ++streak; else break;
    }
    return streak;
}

/// Дневной статус: taken/total/progress/streak/pending — для семейного доступа.
nlohmann::json buildDailyStatus(pillio::Storage& st) {
    auto now = pillio::Clock::now();
    auto today = pillio::formatTimePoint(now).substr(0, 10);
    ensureDailySlots(st, today);

    auto scheds = st.getSchedulesForDate(today);
    int total = static_cast<int>(scheds.size());
    int taken = 0;
    nlohmann::json pending = nlohmann::json::array();
    nlohmann::json items = nlohmann::json::array();

    for (const auto& s : scheds) {
        std::string name = "?";
        double dosage = 0;
        std::string unit, meal = "none";
        try {
            auto p = st.getPillById(s.pill_id);
            name = p.name;
            dosage = p.dosage;
            unit = p.unit;
            meal = p.meal_relation;
        } catch (...) {
        }
        nlohmann::json item = {{"pill_name", name},
                               {"dosage", dosage},
                               {"unit", unit},
                               {"scheduled_time", s.scheduled_time},
                               {"taken", s.taken},
                               {"meal_relation", meal}};
        items.push_back(item);
        if (s.taken) {
            ++taken;
        } else {
            pending.push_back(item);
        }
    }

    double progress = total > 0 ? static_cast<double>(taken) / total : 0.0;
    return {{"taken", taken},
            {"total", total},
            {"progress", progress},
            {"streak", computeStreak(st)},
            {"pending", pending},
            {"items", items}};
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        std::filesystem::path db_path;
        int port{};
        parseArgs(argc, argv, db_path, port);

        // Менеджер хранилищ по пользователям (multi-tenant) + семейный модуль
        ProfileManager profiles(db_path);
        pillio::FamilyStore fam(db_path.parent_path() / "family.json");

        httplib::Server svr;

        // ── CORS middleware ─────────────────────────────────────
        svr.set_pre_routing_handler(
            [](const httplib::Request& /*req*/, httplib::Response& res) {
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_header("Access-Control-Allow-Methods",
                               "GET, POST, DELETE, OPTIONS");
                res.set_header("Access-Control-Allow-Headers",
                               "Content-Type, X-Pillio-Uid");
                return httplib::Server::HandlerResponse::Unhandled;
            });

        // OPTIONS preflight
        svr.Options(".*", [](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods",
                           "GET, POST, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers",
                           "Content-Type, X-Pillio-Uid");
            res.status = 204;
        });

        // ── No-cache для статики ────────────────────────────────
        // Запрещаем браузеру кэшировать ответы, чтобы при обновлении
        // index.html всегда отдавалась свежая версия (а не старая из кэша).
        svr.set_post_routing_handler(
            [](const httplib::Request& /*req*/, httplib::Response& res) {
                res.set_header("Cache-Control",
                               "no-cache, no-store, must-revalidate");
                res.set_header("Pragma", "no-cache");
                res.set_header("Expires", "0");
            });

        // ── GET /api/pills ──────────────────────────────────────
        svr.Get("/api/pills", [&profiles](const httplib::Request& req,
                                         httplib::Response& res) {
            try {
                auto* storage = &profiles.get(getUid(req));
                auto pills = storage->getAllPills();
                nlohmann::json j = pills;
                jsonResponse(res, j);
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── POST /api/pills ─────────────────────────────────────
        svr.Post("/api/pills", [&profiles](const httplib::Request& req,
                                          httplib::Response& res) {
            try {
                auto* storage = &profiles.get(getUid(req));
                auto body = nlohmann::json::parse(req.body);
                pillio::Pill pill = body.get<pillio::Pill>();
                auto saved = storage->addPill(std::move(pill));

                auto today = pillio::formatTimePoint(pillio::Clock::now()).substr(0, 10);
                ensureDailySlots(*storage, today);

                nlohmann::json j = saved;
                jsonResponse(res, j, 201);
            } catch (const pillio::ValidationError& e) {
                errorResponse(res, e.what(), 422);
            } catch (const nlohmann::json::exception& e) {
                errorResponse(res, std::string("Invalid JSON: ") + e.what(), 400);
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── DELETE /api/pills/:id ───────────────────────────────
        svr.Delete(R"(/api/pills/(\d+))",
                   [&profiles](const httplib::Request& req,
                              httplib::Response& res) {
            try {
                auto* storage = &profiles.get(getUid(req));
                auto id = static_cast<std::uint64_t>(
                    std::stoull(req.matches[1].str()));
                storage->removePill(id);
                jsonResponse(res, {{"ok", true}});
            } catch (const pillio::NotFoundError& e) {
                errorResponse(res, e.what(), 404);
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── GET /api/schedule?date=YYYY-MM-DD ───────────────────
        svr.Get("/api/schedule", [&profiles](const httplib::Request& req,
                                            httplib::Response& res) {
            try {
                auto* storage = &profiles.get(getUid(req));
                auto date = req.get_param_value("date");
                if (date.empty()) {
                    // Текущая дата
                    auto now = pillio::Clock::now();
                    date = pillio::formatTimePoint(now).substr(0, 10);
                }

                ensureDailySlots(*storage, date);
                auto schedules = storage->getSchedulesForDate(date);
                // Обогащаем данными pill
                nlohmann::json result = nlohmann::json::array();
                for (const auto& s : schedules) {
                    nlohmann::json entry = s;
                    try {
                        auto pill = storage->getPillById(s.pill_id);
                        entry["pill_name"] = pill.name;
                        entry["pill_dosage"] = pill.dosage;
                        entry["pill_unit"] = pill.unit;
                        entry["meal_relation"] = pill.meal_relation;
                        entry["course_days"] = pill.course_days;
                    } catch (...) {
                        entry["pill_name"] = "Unknown";
                        entry["pill_dosage"] = 0;
                        entry["pill_unit"] = "";
                        entry["meal_relation"] = "none";
                        entry["course_days"] = 0;
                    }
                    result.push_back(std::move(entry));
                }

                nlohmann::json body = {{"date", date},
                                       {"schedules", result},
                                       {"progress",
                                        pillio::dailyProgress(schedules)}};
                jsonResponse(res, body);
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── POST /api/take ──────────────────────────────────────
        svr.Post("/api/take", [&profiles](const httplib::Request& req,
                                         httplib::Response& res) {
            try {
                auto* storage = &profiles.get(getUid(req));
                auto body = nlohmann::json::parse(req.body);
                auto pill_id = body.at("pill_id").get<std::uint64_t>();
                auto sched_time = body.at("scheduled_time").get<std::string>();
                auto taken_at = pillio::formatTimePoint(pillio::Clock::now());

                storage->markTaken(pill_id, sched_time, taken_at);

                nlohmann::json resp = {{"ok", true}, {"taken_at", taken_at}};
                jsonResponse(res, resp);
            } catch (const pillio::NotFoundError& e) {
                errorResponse(res, e.what(), 404);
            } catch (const nlohmann::json::exception& e) {
                errorResponse(res, std::string("Invalid JSON: ") + e.what(), 400);
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── GET /api/next?pill_id=N ─────────────────────────────
        svr.Get("/api/next", [&profiles](const httplib::Request& req,
                                        httplib::Response& res) {
            try {
                auto* storage = &profiles.get(getUid(req));
                auto pid_str = req.get_param_value("pill_id");
                if (pid_str.empty()) {
                    errorResponse(res, "pill_id is required", 400);
                    return;
                }
                auto pill_id = static_cast<std::uint64_t>(
                    std::stoull(pid_str));
                auto pill = storage->getPillById(pill_id);
                auto history = storage->getAllSchedules();
                auto now = pillio::Clock::now();
                auto next = pillio::calculateNextIntake(pill, history, now);

                nlohmann::json body = {
                    {"pill_id", pill_id},
                    {"next_intake", pillio::formatTimePoint(next)}};
                jsonResponse(res, body);
            } catch (const pillio::NotFoundError& e) {
                errorResponse(res, e.what(), 404);
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── GET /api/stats ──────────────────────────────────────────
        svr.Get("/api/stats", [&profiles](const httplib::Request& req,
                                        httplib::Response& res) {
            try {
                auto* storage = &profiles.get(getUid(req));
                auto pills = storage->getAllPills();
                int streak = computeStreak(*storage);

                auto now = pillio::Clock::now();
                auto today = pillio::formatTimePoint(now).substr(0, 10);
                auto today_scheds = storage->getSchedulesForDate(today);
                int total = static_cast<int>(today_scheds.size());
                int taken = 0;
                for (const auto& s : today_scheds) { if (s.taken) ++taken; }

                nlohmann::json body = {
                    {"streak", streak},
                    {"total_pills", static_cast<int>(pills.size())},
                    {"today_total", total},
                    {"today_taken", taken},
                    {"progress", total > 0 ? static_cast<double>(taken) / total : 0.0}
                };
                jsonResponse(res, body);
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── POST /api/mood ──────────────────────────────────────────
        svr.Post("/api/mood", [&db_path](const httplib::Request& req,
                                         httplib::Response& res) {
            try {
                auto body = nlohmann::json::parse(req.body);
                auto date = body.at("date").get<std::string>();
                auto mood = body.at("mood").get<int>();
                if (mood < 1 || mood > 5) {
                    errorResponse(res, "Mood must be 1..5", 422);
                    return;
                }
                // Store mood in a separate JSON file
                auto mood_path = db_path.parent_path() / "moods.json";
                nlohmann::json moods = nlohmann::json::object();
                if (std::filesystem::exists(mood_path)) {
                    std::ifstream ifs(mood_path);
                    if (ifs.is_open()) {
                        try { ifs >> moods; } catch (...) {}
                    }
                }
                moods[date] = mood;
                std::ofstream ofs(mood_path);
                ofs << moods.dump(2);

                jsonResponse(res, {{"ok", true}, {"date", date}, {"mood", mood}});
            } catch (const nlohmann::json::exception& e) {
                errorResponse(res, std::string("Invalid JSON: ") + e.what(), 400);
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── GET /api/mood?date=YYYY-MM-DD ───────────────────────────
        svr.Get("/api/mood", [&db_path](const httplib::Request& req,
                                        httplib::Response& res) {
            try {
                auto date = req.get_param_value("date");
                auto mood_path = db_path.parent_path() / "moods.json";
                nlohmann::json moods = nlohmann::json::object();
                if (std::filesystem::exists(mood_path)) {
                    std::ifstream ifs(mood_path);
                    if (ifs.is_open()) {
                        try { ifs >> moods; } catch (...) {}
                    }
                }
                int mood_val = moods.value(date, 0);
                jsonResponse(res, {{"date", date}, {"mood", mood_val}});
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── POST /api/bot/register ──────────────────────────────────
        svr.Post("/api/bot/register", [&db_path](const httplib::Request& req,
                                                  httplib::Response& res) {
            try {
                auto body = nlohmann::json::parse(req.body);
                auto chat_id = body.at("chat_id").get<std::int64_t>();
                auto bot_path = db_path.parent_path() / "bot.json";
                nlohmann::json bot_data = nlohmann::json::object();
                if (std::filesystem::exists(bot_path)) {
                    std::ifstream ifs(bot_path);
                    if (ifs.is_open()) {
                        try { ifs >> bot_data; } catch (...) {}
                    }
                }
                bot_data["chat_id"] = chat_id;
                bot_data["registered_at"] = pillio::formatTimePoint(
                    pillio::Clock::now());
                std::ofstream ofs(bot_path);
                ofs << bot_data.dump(2);
                jsonResponse(res, {{"ok", true}, {"chat_id", chat_id}});
            } catch (const nlohmann::json::exception& e) {
                errorResponse(res, std::string("Invalid JSON: ") + e.what(), 400);
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── GET /api/bot/reminders ──────────────────────────────────
        svr.Get("/api/bot/reminders", [&profiles](const httplib::Request& req,
                                                  httplib::Response& res) {
            try {
                auto* storage = &profiles.get(getUid(req));
                auto now = pillio::Clock::now();
                auto today = pillio::formatTimePoint(now).substr(0, 10);
                auto scheds = storage->getSchedulesForDate(today);
                auto pills = storage->getAllPills();
                nlohmann::json pending = nlohmann::json::array();
                for (const auto& s : scheds) {
                    if (!s.taken) {
                        nlohmann::json entry = s;
                        for (const auto& p : pills) {
                            if (p.id == s.pill_id) {
                                entry["pill_name"] = p.name;
                                entry["pill_dosage"] = p.dosage;
                                entry["pill_unit"] = p.unit;
                                entry["meal_relation"] = p.meal_relation;
                                break;
                            }
                        }
                        pending.push_back(std::move(entry));
                    }
                }
                jsonResponse(res, {{"reminders", pending}});
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── GET /api/analytics ───────────────────────────────────────
        svr.Get("/api/analytics", [&profiles](const httplib::Request& req,
                                              httplib::Response& res) {
            try {
                auto* storage = &profiles.get(getUid(req));
                auto all = storage->getAllSchedules();
                auto now = pillio::Clock::now();
                auto report = pillio::computeAnalytics(all, now);
                nlohmann::json j = report;
                jsonResponse(res, j);
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── GET /api/interactions ────────────────────────────────────
        svr.Get("/api/interactions", [&profiles](const httplib::Request& req,
                                                httplib::Response& res) {
            try {
                auto* storage = &profiles.get(getUid(req));
                auto pills = storage->getAllPills();
                std::vector<std::string> names;
                names.reserve(pills.size());
                for (const auto& p : pills) {
                    names.push_back(p.name);
                }
                auto interactions = pillio::checkInteractions(names);
                nlohmann::json j = interactions;
                jsonResponse(res, {{"interactions", j},
                                   {"db_size", pillio::interactionDatabaseSize()}});
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── GET /api/export/csv ──────────────────────────────────────
        svr.Get("/api/export/csv", [&profiles](const httplib::Request& req,
                                              httplib::Response& res) {
            try {
                auto* storage = &profiles.get(getUid(req));
                auto pills = storage->getAllPills();
                auto schedules = storage->getAllSchedules();
                std::ostringstream csv;
                csv << "pill_id,pill_name,dosage,unit,scheduled_time,taken,taken_at\n";
                for (const auto& s : schedules) {
                    std::string pill_name = "unknown";
                    double dosage = 0;
                    std::string unit;
                    for (const auto& p : pills) {
                        if (p.id == s.pill_id) {
                            pill_name = p.name;
                            dosage = p.dosage;
                            unit = p.unit;
                            break;
                        }
                    }
                    csv << s.pill_id << ",\""
                        << pill_name << "\","
                        << dosage << ",\""
                        << unit << "\",\""
                        << s.scheduled_time << "\","
                        << (s.taken ? "yes" : "no") << ",\""
                        << s.taken_at << "\"\n";
                }
                res.set_content(csv.str(), "text/csv");
                res.set_header("Content-Disposition",
                               "attachment; filename=pillio_export.csv");
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ════════════════════════════════════════════════════════════
        // Семейный доступ (family sharing)
        // ════════════════════════════════════════════════════════════

        // ── GET /api/family/me?uid=&name= ────────────────────────────
        // Возвращает (создавая при необходимости) мой профиль с share-кодом
        // и текущим дневным статусом — «то, что увидят близкие».
        svr.Get("/api/family/me", [&fam, &profiles](const httplib::Request& req,
                                                    httplib::Response& res) {
            try {
                auto uid_str = getUid(req);
                if (uid_str.empty()) {
                    errorResponse(res, "uid is required", 400);
                    return;
                }
                std::int64_t uid = std::stoll(uid_str);
                std::string name = req.get_param_value("name");
                if (name.empty()) name = "Профиль";

                auto prof = fam.ensureProfile(uid, name);
                auto& st = profiles.get(uid_str);
                nlohmann::json body = {{"chat_id", prof.chat_id},
                                       {"name", prof.name},
                                       {"share_code", prof.share_code},
                                       {"status", buildDailyStatus(st)}};
                jsonResponse(res, body);
            } catch (const pillio::ValidationError& e) {
                errorResponse(res, e.what(), 422);
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── POST /api/family/follow ──────────────────────────────────
        // Подписаться на чужой профиль по share-коду.
        // body: {uid, name, code, relation}
        svr.Post("/api/family/follow", [&fam](const httplib::Request& req,
                                              httplib::Response& res) {
            try {
                auto body = nlohmann::json::parse(req.body);
                auto uid = body.at("uid").get<std::int64_t>();
                auto myname = body.value("name", std::string{"Профиль"});
                auto code = body.at("code").get<std::string>();
                auto relation = body.value("relation", std::string{"близкий"});

                fam.ensureProfile(uid, myname);
                auto target = fam.profileByCode(code);
                if (!target) {
                    errorResponse(res, "Код не найден", 404);
                    return;
                }
                if (target->chat_id == uid) {
                    errorResponse(res, "Нельзя подписаться на себя", 422);
                    return;
                }
                fam.link(uid, target->chat_id, relation);

                nlohmann::json out = {
                    {"ok", true},
                    {"target", {{"chat_id", target->chat_id},
                                {"name", target->name},
                                {"relation", relation}}}};
                jsonResponse(res, out, 201);
            } catch (const pillio::ValidationError& e) {
                errorResponse(res, e.what(), 422);
            } catch (const nlohmann::json::exception& e) {
                errorResponse(res, std::string("Invalid JSON: ") + e.what(), 400);
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── DELETE /api/family/follow?uid=&target= ───────────────────
        svr.Delete("/api/family/follow", [&fam](const httplib::Request& req,
                                                httplib::Response& res) {
            try {
                auto uid_str = getUid(req);
                auto target_str = req.get_param_value("target");
                if (uid_str.empty() || target_str.empty()) {
                    errorResponse(res, "uid and target are required", 400);
                    return;
                }
                fam.unlink(std::stoll(uid_str), std::stoll(target_str));
                jsonResponse(res, {{"ok", true}});
            } catch (const pillio::NotFoundError& e) {
                errorResponse(res, e.what(), 404);
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── GET /api/family/following?uid= ───────────────────────────
        // Список профилей, за которыми я слежу, с их дневным статусом.
        svr.Get("/api/family/following", [&fam, &profiles](
                                             const httplib::Request& req,
                                             httplib::Response& res) {
            try {
                auto uid_str = getUid(req);
                if (uid_str.empty()) {
                    errorResponse(res, "uid is required", 400);
                    return;
                }
                std::int64_t uid = std::stoll(uid_str);
                auto links = fam.following(uid);
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& l : links) {
                    auto tp = fam.profileByChatId(l.target);
                    auto& st = profiles.get(std::to_string(l.target));
                    arr.push_back({{"chat_id", l.target},
                                   {"name", tp ? tp->name : std::string{"?"}},
                                   {"relation", l.relation},
                                   {"status", buildDailyStatus(st)}});
                }
                jsonResponse(res, {{"following", arr}});
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── GET /api/family/members?uid= ────────────────────────────
        // Все связанные пользователи (following ∪ followers, дедупликация)
        // с полным дневным статусом. Единый список «Моя семья».
        svr.Get("/api/family/members", [&fam, &profiles](
                                            const httplib::Request& req,
                                            httplib::Response& res) {
            try {
                auto uid_str = getUid(req);
                if (uid_str.empty()) {
                    errorResponse(res, "uid is required", 400);
                    return;
                }
                std::int64_t uid = std::stoll(uid_str);

                // Собираем все связанные chat_id (both directions)
                auto fwd = fam.following(uid);   // я → они
                auto rev = fam.followers(uid);    // они → я

                std::map<std::int64_t, std::string> seen;  // chat_id → relation
                for (const auto& l : fwd) seen[l.target] = l.relation;
                for (const auto& l : rev) {
                    if (seen.find(l.follower) == seen.end()) {
                        seen[l.follower] = l.relation;
                    }
                }

                nlohmann::json arr = nlohmann::json::array();
                for (const auto& [cid, rel] : seen) {
                    auto tp = fam.profileByChatId(cid);
                    auto& st = profiles.get(std::to_string(cid));
                    arr.push_back({{"chat_id", cid},
                                   {"name", tp ? tp->name : std::string{"?"}},
                                   {"relation", rel},
                                   {"status", buildDailyStatus(st)}});
                }
                jsonResponse(res, {{"members", arr}});
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── GET /api/family/followers?uid= ───────────────────────────
        // Список тех, кто видит мой статус.
        svr.Get("/api/family/followers", [&fam](const httplib::Request& req,
                                                 httplib::Response& res) {
            try {
                auto uid_str = getUid(req);
                if (uid_str.empty()) {
                    errorResponse(res, "uid is required", 400);
                    return;
                }
                std::int64_t uid = std::stoll(uid_str);
                auto links = fam.followers(uid);
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& l : links) {
                    auto fp = fam.profileByChatId(l.follower);
                    arr.push_back({{"chat_id", l.follower},
                                   {"name", fp ? fp->name : std::string{"?"}},
                                   {"relation", l.relation}});
                }
                jsonResponse(res, {{"followers", arr}});
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        // ── GET /api/family/digest?uid= ──────────────────────────────
        // Для Telegram-бота: просроченные приёмы пользователя uid и список
        // близких (chat_id), которым нужно отправить уведомление.
        svr.Get("/api/family/digest", [&fam, &profiles](
                                          const httplib::Request& req,
                                          httplib::Response& res) {
            try {
                auto uid_str = getUid(req);
                if (uid_str.empty()) {
                    errorResponse(res, "uid is required", 400);
                    return;
                }
                std::int64_t uid = std::stoll(uid_str);
                auto prof = fam.profileByChatId(uid);
                auto& st = profiles.get(uid_str);
                auto status = buildDailyStatus(st);

                // Просроченные = ожидающие с временем приёма <= текущего.
                auto now = pillio::Clock::now();
                auto now_hm = pillio::formatTimePoint(now).substr(11, 5);
                nlohmann::json overdue = nlohmann::json::array();
                for (const auto& item : status["pending"]) {
                    auto t = item.value("scheduled_time", std::string{});
                    if (t.size() >= 16 && t.substr(11, 5) <= now_hm) {
                        overdue.push_back(item);
                    }
                }

                nlohmann::json notify = nlohmann::json::array();
                for (const auto& l : fam.followers(uid)) {
                    notify.push_back({{"chat_id", l.follower},
                                      {"relation", l.relation}});
                }

                nlohmann::json body = {
                    {"profile", {{"chat_id", uid},
                                 {"name", prof ? prof->name : std::string{"?"}}}},
                    {"overdue", overdue},
                    {"notify", notify}};
                jsonResponse(res, body);
            } catch (const std::exception& e) {
                errorResponse(res, e.what(), 500);
            }
        });

        auto exe_dir = std::filesystem::current_path();
        auto static_dir = exe_dir / "static";
        if (std::filesystem::exists(static_dir)) {
            svr.set_mount_point("/", static_dir.string());
        }

        std::cout << "Pillio server starting on http://localhost:"
                  << port << "\n";
        std::cout << "Database: " << db_path.string() << "\n";

        if (!svr.listen("0.0.0.0", port)) {
            std::cerr << "Failed to start server on port " << port << "\n";
            return 1;
        }

    } catch (const pillio::ValidationError& e) {
        std::cerr << "[ValidationError] " << e.what() << "\n";
        return 1;
    } catch (const pillio::StorageError& e) {
        std::cerr << "[StorageError] " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "[Error] " << e.what() << "\n";
        return 1;
    }
    return 0;
}
