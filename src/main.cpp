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

// ── Совместимость с MinGW для httplib ────────────────────────────
// Заголовки MinGW часто не объявляют API Windows 8 (например,
// GetAddrInfoExCancel), которые httplib считает доступными при
// _WIN32_WINNT >= 0x0602 — добавляем заглушку.
#ifdef _WIN32
// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on
inline int GetAddrInfoExCancel(HANDLE* x) {
    return 0;
}
#endif
// ─────────────────────────────────────────────────────────────────

#include <httplib.h>

#include <nlohmann/json.hpp>

#include "family.hpp"
#include "models.hpp"
#include "storage.hpp"
#include "tracker.hpp"

namespace {

/**
 * @brief Разбирает настройки запуска: путь к БД, путь к семье, порт.
 *
 * Приоритет источников: аргумент командной строки > переменная
 * окружения > значение по умолчанию.
 *
 * @param argc количество аргументов
 * @param argv массив аргументов командной строки
 * @param[out] db_path путь к файлу хранилища
 * @param[out] family_path путь к файлу семьи
 * @param[out] static_dir каталог со статикой Mini App
 * @param[out] port порт HTTP-сервера
 * @throws ValidationError если порт вне диапазона 1..65535
 */
void parseArgs(int argc, char* argv[], std::filesystem::path& db_path,
               std::filesystem::path& family_path, std::filesystem::path& static_dir, int& port) {
    // Приоритет: CLI-аргумент > переменная окружения > значение по умолчанию.
    if (const char* env_db = std::getenv("STORAGE_PATH"); env_db && *env_db) {
        db_path = env_db;
    } else {
        db_path = "./data/store.json";
    }
    if (const char* env_fam = std::getenv("FAMILY_STORAGE_PATH"); env_fam && *env_fam) {
        family_path = env_fam;
    } else {
        family_path = "";  // заполним относительно db_path ниже
    }
    if (const char* env_port = std::getenv("API_PORT"); env_port && *env_port) {
        port = std::atoi(env_port);
    } else {
        port = 8080;
    }
    if (const char* env_static = std::getenv("STATIC_DIR"); env_static && *env_static) {
        static_dir = env_static;
    } else {
        static_dir = "./static";
    }

    std::vector<std::string> args(argv, argv + argc);
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--db" && i + 1 < args.size()) {
            db_path = args[++i];
        } else if (args[i] == "--family" && i + 1 < args.size()) {
            family_path = args[++i];
        } else if (args[i] == "--static" && i + 1 < args.size()) {
            static_dir = args[++i];
        } else if (args[i] == "--port" && i + 1 < args.size()) {
            port = std::stoi(args[++i]);
        } else if (args[i] == "--help") {
            std::cout << "Usage: pillio [--db path] [--family path] "
                         "[--static dir] [--port N]\n";
            std::exit(0);
        }
    }

    if (family_path.empty()) {
        family_path = db_path.parent_path() / "family.json";
    }

    if (port < 1 || port > 65535) {
        throw pillio::ValidationError("Port must be 1..65535");
    }
}

/**
 * @brief Отправляет JSON-ответ с указанным HTTP-статусом.
 * @param res объект ответа httplib
 * @param body тело ответа в формате JSON
 * @param status HTTP-код состояния (по умолчанию 200)
 */
void jsonResponse(httplib::Response& res, const nlohmann::json& body, int status = 200) {
    res.set_content(body.dump(), "application/json");
    res.status = status;
}

/**
 * @brief Отправляет JSON-ответ об ошибке вида {"error": message}.
 * @param res объект ответа httplib
 * @param message текст ошибки
 * @param status HTTP-код состояния (по умолчанию 400)
 */
void errorResponse(httplib::Response& res, const std::string& message, int status = 400) {
    nlohmann::json body = {{"error", message}};
    jsonResponse(res, body, status);
}

/**
 * @brief Оборачивает обработчик единой обработкой исключений → HTTP-код.
 *
 * Убирает дублирование try/catch в каждом эндпоинте: тип исключения
 * однозначно отображается на код ответа (422/404/400/500).
 *
 * @param fn обработчик запроса
 * @return обработчик httplib с перехватом исключений
 */
template <typename F>
auto guarded(F fn) {
    return [fn](const httplib::Request& req, httplib::Response& res) {
        try {
            fn(req, res);
        } catch (const pillio::ValidationError& e) {
            errorResponse(res, e.what(), 422);
        } catch (const pillio::NotFoundError& e) {
            errorResponse(res, e.what(), 404);
        } catch (const nlohmann::json::exception& e) {
            errorResponse(res, std::string("Некорректный JSON: ") + e.what(), 400);
        } catch (const std::exception& e) {
            errorResponse(res, e.what(), 500);
        }
    };
}

/// @brief Безопасно читает JSON-объект из файла (при ошибке — пустой объект).
nlohmann::json loadJsonFile(const std::filesystem::path& path) {
    nlohmann::json data = nlohmann::json::object();
    if (std::filesystem::exists(path)) {
        std::ifstream ifs(path);
        if (ifs.is_open()) {
            try {
                ifs >> data;
            } catch (...) {
                data = nlohmann::json::object();
            }
        }
    }
    if (!data.is_object()) data = nlohmann::json::object();
    return data;
}

/**
 * @brief Атомарно записывает JSON в файл (временный файл + rename).
 *
 * Гарантирует, что внешний бэкап никогда не прочитает наполовину
 * записанный файл, и защищает от повреждения при падении процесса.
 *
 * @param path путь к целевому файлу
 * @param data данные для записи
 * @throws StorageError при ошибке записи или переименования
 */
void saveJsonFileAtomic(const std::filesystem::path& path, const nlohmann::json& data) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::trunc);
        if (!ofs.is_open()) {
            throw pillio::StorageError("Cannot write file: " + path.string());
        }
        ofs << data.dump(2);
        if (ofs.fail()) {
            throw pillio::StorageError("Write failed for: " + path.string());
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        throw pillio::StorageError("Atomic rename failed for " + path.string());
    }
}

/**
 * @brief Кэш хранилищ по пользователям (мультиарендность).
 *
 * Пустой uid → файл из --db (локальный режим); uid=N → profiles/N.json
 * (режим Telegram). Доступ потокобезопасен (httplib многопоточный).
 */
class ProfileManager {
   public:
    /**
     * @brief Создаёт менеджер с путём к хранилищу по умолчанию.
     * @param default_db путь к БД для пустого uid
     */
    explicit ProfileManager(std::filesystem::path default_db)
        : default_db_(std::move(default_db)), base_dir_(default_db_.parent_path()) {}

    /**
     * @brief Возвращает (создавая при первом обращении) хранилище для uid.
     * @param uid идентификатор пользователя (пустой → хранилище по умолчанию)
     * @return ссылка на хранилище данного пользователя
     */
    pillio::Storage& get(const std::string& uid) {
        std::lock_guard<std::mutex> lock(mtx_);
        const std::string key = uid.empty() ? "__default__" : uid;
        auto it = stores_.find(key);
        if (it != stores_.end()) {
            return *it->second;
        }
        std::filesystem::path path =
            uid.empty() ? default_db_ : base_dir_ / "profiles" / (uid + ".json");
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

/**
 * @brief Извлекает uid из заголовка X-Pillio-Uid или параметра ?uid=.
 * @param req входящий HTTP-запрос
 * @return идентификатор пользователя или пустая строка
 */
std::string getUid(const httplib::Request& req) {
    if (req.has_header("X-Pillio-Uid")) {
        return req.get_header_value("X-Pillio-Uid");
    }
    if (req.has_param("uid")) {
        return req.get_param_value("uid");
    }
    return "";
}

/// @brief Возвращает uid из запроса или бросает ошибку, если он не задан.
std::string requireUid(const httplib::Request& req) {
    auto uid = getUid(req);
    if (uid.empty()) throw pillio::ValidationError("uid is required");
    return uid;
}

/**
 * @brief Генерирует недостающие слоты расписания на дату для всех лекарств.
 * @param st хранилище пользователя
 * @param date дата в формате "YYYY-MM-DD"
 */
void ensureDailySlots(pillio::Storage& st, const std::string& date) {
    auto pills = st.getAllPills();
    for (const auto& pill : pills) {
        auto tp = pillio::parseTimePoint(date + "T12:00:00");
        auto slots = pillio::generateDailySlots(pill, tp);
        auto existing = st.getSchedulesForDate(date);
        for (auto& slot : slots) {
            bool dup = false;
            for (const auto& ex : existing) {
                if (ex.pill_id == slot.pill_id && ex.scheduled_time == slot.scheduled_time) {
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

/**
 * @brief Считает серию дней подряд со 100% адгезией (назад до 365 дней).
 * @param st хранилище пользователя
 * @return число последовательных «идеальных» дней
 */
int computeStreak(pillio::Storage& st) {
    auto now = pillio::Clock::now();
    int streak = 0;
    for (int d = 1; d <= 365; ++d) {
        auto day_str = pillio::formatTimePoint(now - std::chrono::hours(24 * d)).substr(0, 10);
        auto day_scheds = st.getSchedulesForDate(day_str);
        if (day_scheds.empty()) break;
        bool ok = std::all_of(day_scheds.begin(), day_scheds.end(),
                              [](const pillio::Schedule& s) { return s.taken; });
        if (ok)
            ++streak;
        else
            break;
    }
    return streak;
}

/**
 * @brief Формирует дневной статус (принято/всего/прогресс/серия/ожидающие).
 * @param st хранилище пользователя
 * @return JSON-объект со сводкой за сегодня (для семейного доступа)
 */
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
        nlohmann::json item = {{"pill_name", name}, {"dosage", dosage},
                               {"unit", unit},      {"scheduled_time", s.scheduled_time},
                               {"taken", s.taken},  {"meal_relation", meal}};
        items.push_back(item);
        if (s.taken) {
            ++taken;
        } else {
            pending.push_back(item);
        }
    }

    double progress = total > 0 ? static_cast<double>(taken) / total : 0.0;
    return {{"taken", taken},       {"total", total},
            {"progress", progress}, {"streak", computeStreak(st)},
            {"pending", pending},   {"items", items}};
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        std::filesystem::path db_path;
        std::filesystem::path family_path;
        std::filesystem::path static_dir;
        int port{};
        parseArgs(argc, argv, db_path, family_path, static_dir, port);

        // Менеджер хранилищ по пользователям (multi-tenant) + семейный модуль
        ProfileManager profiles(db_path);
        pillio::FamilyStore fam(family_path);

        httplib::Server svr;

        // ── Заголовки CORS (для всех ответов) ───────────────────
        auto applyCors = [](httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type, X-Pillio-Uid");
        };
        svr.set_pre_routing_handler(
            [&applyCors](const httplib::Request& /*req*/, httplib::Response& res) {
                applyCors(res);
                return httplib::Server::HandlerResponse::Unhandled;
            });
        svr.Options(".*", [&applyCors](const httplib::Request&, httplib::Response& res) {
            applyCors(res);
            res.status = 204;
        });

        // ── No-cache для статики ────────────────────────────────
        // Запрещаем браузеру кэшировать ответы, чтобы при обновлении
        // index.html всегда отдавалась свежая версия (а не старая из кэша).
        svr.set_post_routing_handler([](const httplib::Request& /*req*/, httplib::Response& res) {
            res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
            res.set_header("Pragma", "no-cache");
            res.set_header("Expires", "0");
        });

        // ── GET /api/pills ──────────────────────────────────────
        svr.Get("/api/pills",
                guarded([&profiles](const httplib::Request& req, httplib::Response& res) {
                    auto* storage = &profiles.get(getUid(req));
                    auto pills = storage->getAllPills();
                    nlohmann::json j = pills;
                    jsonResponse(res, j);
                }));

        // ── POST /api/pills ─────────────────────────────────────
        svr.Post("/api/pills",
                 guarded([&profiles](const httplib::Request& req, httplib::Response& res) {
                     auto* storage = &profiles.get(getUid(req));
                     auto body = nlohmann::json::parse(req.body);
                     pillio::Pill pill = body.get<pillio::Pill>();
                     auto saved = storage->addPill(std::move(pill));

                     auto today = pillio::formatTimePoint(pillio::Clock::now()).substr(0, 10);
                     ensureDailySlots(*storage, today);

                     nlohmann::json j = saved;
                     jsonResponse(res, j, 201);
                 }));

        // ── DELETE /api/pills/:id ───────────────────────────────
        svr.Delete(R"(/api/pills/(\d+))",
                   guarded([&profiles](const httplib::Request& req, httplib::Response& res) {
                       auto* storage = &profiles.get(getUid(req));
                       auto id = static_cast<std::uint64_t>(std::stoull(req.matches[1].str()));
                       storage->removePill(id);
                       jsonResponse(res, {{"ok", true}});
                   }));

        // ── GET /api/schedule?date=YYYY-MM-DD ───────────────────
        svr.Get("/api/schedule",
                guarded([&profiles](const httplib::Request& req, httplib::Response& res) {
                    auto* storage = &profiles.get(getUid(req));
                    auto date = req.get_param_value("date");
                    if (date.empty()) {
                        date = pillio::formatTimePoint(pillio::Clock::now()).substr(0, 10);
                    }

                    ensureDailySlots(*storage, date);
                    auto schedules = storage->getSchedulesForDate(date);
                    // Обогащаем каждый слот сведениями о лекарстве
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
                                           {"progress", pillio::dailyProgress(schedules)}};
                    jsonResponse(res, body);
                }));

        // ── POST /api/take ──────────────────────────────────────
        svr.Post("/api/take",
                 guarded([&profiles](const httplib::Request& req, httplib::Response& res) {
                     auto* storage = &profiles.get(getUid(req));
                     auto body = nlohmann::json::parse(req.body);
                     auto pill_id = body.at("pill_id").get<std::uint64_t>();
                     auto sched_time = body.at("scheduled_time").get<std::string>();
                     auto taken_at = pillio::formatTimePoint(pillio::Clock::now());

                     storage->markTaken(pill_id, sched_time, taken_at);
                     jsonResponse(res, {{"ok", true}, {"taken_at", taken_at}});
                 }));

        // ── GET /api/next?pill_id=N ─────────────────────────────
        svr.Get(
            "/api/next", guarded([&profiles](const httplib::Request& req, httplib::Response& res) {
                auto* storage = &profiles.get(getUid(req));
                auto pid_str = req.get_param_value("pill_id");
                if (pid_str.empty()) {
                    errorResponse(res, "pill_id is required", 400);
                    return;
                }
                auto pill_id = static_cast<std::uint64_t>(std::stoull(pid_str));
                auto pill = storage->getPillById(pill_id);
                auto next = pillio::calculateNextIntake(pill, storage->getAllSchedules(),
                                                        pillio::Clock::now());
                jsonResponse(
                    res, {{"pill_id", pill_id}, {"next_intake", pillio::formatTimePoint(next)}});
            }));

        // ── GET /api/stats ──────────────────────────────────────────
        svr.Get("/api/stats",
                guarded([&profiles](const httplib::Request& req, httplib::Response& res) {
                    auto* storage = &profiles.get(getUid(req));
                    auto pills = storage->getAllPills();
                    int streak = computeStreak(*storage);

                    auto today = pillio::formatTimePoint(pillio::Clock::now()).substr(0, 10);
                    auto today_scheds = storage->getSchedulesForDate(today);
                    int total = static_cast<int>(today_scheds.size());
                    int taken = 0;
                    for (const auto& s : today_scheds) {
                        if (s.taken) ++taken;
                    }

                    jsonResponse(
                        res, {{"streak", streak},
                              {"total_pills", static_cast<int>(pills.size())},
                              {"today_total", total},
                              {"today_taken", taken},
                              {"progress", total > 0 ? static_cast<double>(taken) / total : 0.0}});
                }));

        // ── POST /api/bot/register ──────────────────────────────────
        // Сохраняет соответствие telegram_id -> chat_id в bot.json
        svr.Post(
            "/api/bot/register",
            guarded([&db_path](const httplib::Request& req, httplib::Response& res) {
                auto body = nlohmann::json::parse(req.body);
                auto telegram_id = body.at("telegram_id").get<std::string>();
                auto bot_path = db_path.parent_path() / "bot.json";
                nlohmann::json bot_data = loadJsonFile(bot_path);
                // chat_id сохраняем как есть, чтобы не потерять тип (число или строка)
                bot_data[telegram_id] =
                    body.contains("chat_id") ? body["chat_id"] : nlohmann::json("");
                bot_data["_meta"] = {{"updated_at", pillio::formatTimePoint(pillio::Clock::now())}};
                saveJsonFileAtomic(bot_path, bot_data);
                jsonResponse(res, {{"ok", true}});
            }));

        // ── GET /api/bot/reminders ──────────────────────────────────
        svr.Get("/api/bot/reminders",
                guarded([&profiles](const httplib::Request& req, httplib::Response& res) {
                    auto* storage = &profiles.get(getUid(req));
                    auto today = pillio::formatTimePoint(pillio::Clock::now()).substr(0, 10);
                    auto scheds = storage->getSchedulesForDate(today);
                    auto pills = storage->getAllPills();
                    nlohmann::json pending = nlohmann::json::array();
                    for (const auto& s : scheds) {
                        if (s.taken) continue;
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
                    jsonResponse(res, {{"reminders", pending}});
                }));

        // ── GET /api/bot/users ─────────────────────────────────────────
        // Возвращает всех зарегистрированных пользователей: {users: {uid: chat_id, ...}}
        svr.Get("/api/bot/users",
                guarded([&db_path](const httplib::Request& /*req*/, httplib::Response& res) {
                    auto bot_data = loadJsonFile(db_path.parent_path() / "bot.json");
                    nlohmann::json users = nlohmann::json::object();
                    for (auto& [key, val] : bot_data.items()) {
                        if (key != "_meta") users[key] = val;
                    }
                    jsonResponse(res, {{"users", users}});
                }));

        // ── GET /api/export/csv ──────────────────────────────────────
        svr.Get("/api/export/csv",
                guarded([&profiles](const httplib::Request& req, httplib::Response& res) {
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
                        csv << s.pill_id << ",\"" << pill_name << "\"," << dosage << ",\"" << unit
                            << "\",\"" << s.scheduled_time << "\"," << (s.taken ? "yes" : "no")
                            << ",\"" << s.taken_at << "\"\n";
                    }
                    res.set_content(csv.str(), "text/csv");
                    res.set_header("Content-Disposition", "attachment; filename=pillio_export.csv");
                }));

        // ════════════════════════════════════════════════════════════
        // Семейный доступ (family sharing)
        // ════════════════════════════════════════════════════════════

        // ── GET /api/family/me?uid=&name= ────────────────────────────
        // Возвращает (создавая при необходимости) мой профиль с share-кодом
        // и текущим дневным статусом — «то, что увидят близкие».
        svr.Get("/api/family/me",
                guarded([&fam, &profiles](const httplib::Request& req, httplib::Response& res) {
                    auto uid_str = requireUid(req);
                    std::int64_t uid = std::stoll(uid_str);
                    std::string name = req.get_param_value("name");
                    if (name.empty()) name = "Профиль";
                    std::string username = req.get_param_value("username");

                    auto prof = fam.ensureProfile(uid, name, username);
                    auto& st = profiles.get(uid_str);
                    jsonResponse(res, {{"chat_id", prof.chat_id},
                                       {"name", prof.name},
                                       {"share_code", prof.share_code},
                                       {"username", prof.username},
                                       {"status", buildDailyStatus(st)}});
                }));

        // ── POST /api/family/follow ──────────────────────────────────
        // Подписаться на чужой профиль по share-коду.
        // body: {uid, name, code, relation}
        svr.Post("/api/family/follow",
                 guarded([&fam](const httplib::Request& req, httplib::Response& res) {
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
                     jsonResponse(res,
                                  {{"ok", true},
                                   {"target",
                                    {{"chat_id", target->chat_id},
                                     {"name", target->name},
                                     {"relation", relation}}}},
                                  201);
                 }));

        // ── DELETE /api/family/follow?uid=&target= ───────────────────
        svr.Delete("/api/family/follow",
                   guarded([&fam](const httplib::Request& req, httplib::Response& res) {
                       auto uid_str = getUid(req);
                       auto target_str = req.get_param_value("target");
                       if (uid_str.empty() || target_str.empty()) {
                           errorResponse(res, "uid and target are required", 400);
                           return;
                       }
                       fam.unlink(std::stoll(uid_str), std::stoll(target_str));
                       jsonResponse(res, {{"ok", true}});
                   }));

        // ── GET /api/family/following?uid= ───────────────────────────
        // Список профилей, за которыми я слежу, с их дневным статусом.
        // name = как я их назвал (relation), profile_name = их настоящее имя
        svr.Get("/api/family/following",
                guarded([&fam, &profiles](const httplib::Request& req, httplib::Response& res) {
                    auto uid_str = requireUid(req);
                    std::int64_t uid = std::stoll(uid_str);
                    nlohmann::json arr = nlohmann::json::array();
                    for (const auto& l : fam.following(uid)) {
                        auto tp = fam.profileByChatId(l.target);
                        auto& st = profiles.get(std::to_string(l.target));
                        std::string pname = tp ? tp->name : std::string{"?"};
                        // name = моя метка, иначе настоящее имя профиля
                        arr.push_back({{"chat_id", l.target},
                                       {"name", l.relation.empty() ? pname : l.relation},
                                       {"profile_name", pname},
                                       {"relation", l.relation},
                                       {"status", buildDailyStatus(st)}});
                    }
                    jsonResponse(res, {{"following", arr}});
                }));

        // ── GET /api/family/members?uid= ────────────────────────────
        // Все связанные пользователи (following ∪ followers, дедупликация)
        // с полным дневным статусом. Единый список «Моя семья».
        // name = как я их назвал (relation), profile_name = их настоящее имя
        svr.Get("/api/family/members",
                guarded([&fam, &profiles](const httplib::Request& req, httplib::Response& res) {
                    auto uid_str = requireUid(req);
                    std::int64_t uid = std::stoll(uid_str);

                    // Объединяем обе стороны: за кем слежу я и кто следит за мной
                    std::map<std::int64_t, std::string> seen;  // chat_id → метка
                    for (const auto& l : fam.following(uid)) seen[l.target] = l.relation;
                    for (const auto& l : fam.followers(uid)) {
                        if (seen.find(l.follower) == seen.end()) seen[l.follower] = l.relation;
                    }

                    nlohmann::json arr = nlohmann::json::array();
                    for (const auto& [cid, rel] : seen) {
                        auto tp = fam.profileByChatId(cid);
                        auto& st = profiles.get(std::to_string(cid));
                        std::string pname = tp ? tp->name : std::string{"?"};
                        arr.push_back({{"chat_id", cid},
                                       {"name", rel.empty() ? pname : rel},
                                       {"profile_name", pname},
                                       {"relation", rel},
                                       {"status", buildDailyStatus(st)}});
                    }
                    jsonResponse(res, {{"members", arr}});
                }));

        // ── GET /api/family/followers?uid= ───────────────────────────
        // Список тех, кто видит мой статус.
        svr.Get("/api/family/followers",
                guarded([&fam](const httplib::Request& req, httplib::Response& res) {
                    auto uid_str = requireUid(req);
                    std::int64_t uid = std::stoll(uid_str);
                    nlohmann::json arr = nlohmann::json::array();
                    for (const auto& l : fam.followers(uid)) {
                        auto fp = fam.profileByChatId(l.follower);
                        arr.push_back({{"chat_id", l.follower},
                                       {"name", fp ? fp->name : std::string{"?"}},
                                       {"relation", l.relation}});
                    }
                    jsonResponse(res, {{"followers", arr}});
                }));

        // ── GET /api/family/digest?uid= ──────────────────────────────
        // Для Telegram-бота: просроченные приёмы пользователя uid и список
        // близких (chat_id), которым нужно отправить уведомление.
        svr.Get("/api/family/digest",
                guarded([&fam, &profiles](const httplib::Request& req, httplib::Response& res) {
                    auto uid_str = requireUid(req);
                    std::int64_t uid = std::stoll(uid_str);
                    auto prof = fam.profileByChatId(uid);
                    auto status = buildDailyStatus(profiles.get(uid_str));

                    // Просрочено = ожидающие, чьё время приёма уже наступило
                    auto now_hm = pillio::formatTimePoint(pillio::Clock::now()).substr(11, 5);
                    nlohmann::json overdue = nlohmann::json::array();
                    for (const auto& item : status["pending"]) {
                        auto t = item.value("scheduled_time", std::string{});
                        if (t.size() >= 16 && t.substr(11, 5) <= now_hm) overdue.push_back(item);
                    }

                    nlohmann::json notify = nlohmann::json::array();
                    for (const auto& l : fam.followers(uid)) {
                        notify.push_back({{"chat_id", l.follower}, {"relation", l.relation}});
                    }

                    jsonResponse(
                        res, {{"profile",
                               {{"chat_id", uid}, {"name", prof ? prof->name : std::string{"?"}}}},
                              {"overdue", overdue},
                              {"notify", notify}});
                }));

        // ── POST /api/family/request ─────────────────────────────────
        // Запрос на добавление в семью по @username.
        // body: {uid, name, username, target_username, relation}
        svr.Post("/api/family/request",
                 guarded([&fam](const httplib::Request& req, httplib::Response& res) {
                     auto body = nlohmann::json::parse(req.body);
                     auto uid = body.at("uid").get<std::int64_t>();
                     auto myname = body.value("name", std::string{"Пользователь"});
                     auto myuser = body.value("username", std::string{});
                     auto target_username = body.value("target_username", std::string{});
                     auto relation = body.value("relation", std::string{"близкий"});
                     if (target_username.empty()) {
                         errorResponse(res, "Укажите @username", 422);
                         return;
                     }

                     // Регистрируем свой профиль (с username — чтобы и нас находили)
                     fam.ensureProfile(uid, myname, myuser);

                     auto target = fam.profileByUsername(target_username);
                     if (!target) {
                         errorResponse(res,
                                       "Пользователь не найден. Попросите его открыть приложение "
                                       "или написать боту /start.",
                                       404);
                         return;
                     }
                     if (target->chat_id == uid) {
                         errorResponse(res, "Нельзя добавить самого себя", 422);
                         return;
                     }

                     auto r = fam.addRequest(uid, myname, target->chat_id, relation);
                     jsonResponse(
                         res, {{"ok", true}, {"request_id", r.id}, {"target_name", target->name}},
                         201);
                 }));

        // ── GET /api/family/requests?uid= ────────────────────────────
        // Входящие запросы для пользователя uid.
        svr.Get("/api/family/requests",
                guarded([&fam](const httplib::Request& req, httplib::Response& res) {
                    auto uid_str = requireUid(req);
                    std::int64_t uid = std::stoll(uid_str);
                    nlohmann::json arr = nlohmann::json::array();
                    for (const auto& r : fam.incomingRequests(uid)) {
                        arr.push_back({{"id", r.id},
                                       {"from", r.from},
                                       {"from_name", r.from_name},
                                       {"relation", r.relation}});
                    }
                    jsonResponse(res, {{"requests", arr}});
                }));

        // ── GET /api/family/requests/outbox ──────────────────────────
        // Для бота: запросы, о которых ещё не уведомлён получатель.
        svr.Get("/api/family/requests/outbox",
                guarded([&fam](const httplib::Request& /*req*/, httplib::Response& res) {
                    nlohmann::json arr = nlohmann::json::array();
                    for (const auto& r : fam.unnotifiedRequests()) {
                        arr.push_back({{"id", r.id},
                                       {"from", r.from},
                                       {"from_name", r.from_name},
                                       {"to", r.to},
                                       {"relation", r.relation}});
                    }
                    jsonResponse(res, {{"requests", arr}});
                }));

        // ── POST /api/family/request/notified  body:{request_id} ─────
        svr.Post("/api/family/request/notified",
                 guarded([&fam](const httplib::Request& req, httplib::Response& res) {
                     auto body = nlohmann::json::parse(req.body);
                     fam.markNotified(body.at("request_id").get<std::string>());
                     jsonResponse(res, {{"ok", true}});
                 }));

        // ── POST /api/family/request/accept  body:{request_id, name} ─
        // Подтверждение получателем: создаётся связь from → to.
        svr.Post("/api/family/request/accept",
                 guarded([&fam](const httplib::Request& req, httplib::Response& res) {
                     auto body = nlohmann::json::parse(req.body);
                     auto id = body.at("request_id").get<std::string>();
                     auto r = fam.requestById(id);
                     if (!r) {
                         errorResponse(res, "Запрос не найден", 404);
                         return;
                     }
                     // Получатель подтверждает свой профиль (имя, если передано)
                     auto myname = body.value("name", std::string{});
                     if (!myname.empty()) fam.ensureProfile(r->to, myname);
                     fam.link(r->from, r->to, r->relation);
                     fam.removeRequest(id);
                     jsonResponse(res, {{"ok", true},
                                        {"from", r->from},
                                        {"from_name", r->from_name},
                                        {"relation", r->relation}});
                 }));

        // ── POST /api/family/request/decline  body:{request_id} ──────
        svr.Post("/api/family/request/decline",
                 guarded([&fam](const httplib::Request& req, httplib::Response& res) {
                     auto body = nlohmann::json::parse(req.body);
                     auto id = body.at("request_id").get<std::string>();
                     auto r = fam.requestById(id);
                     fam.removeRequest(id);
                     nlohmann::json out = {{"ok", true}};
                     if (r) {
                         out["from"] = r->from;
                         out["from_name"] = r->from_name;
                     }
                     jsonResponse(res, out);
                 }));

        // ── POST /api/family/relabel  body:{uid, target, relation} ───
        // Задать/изменить, как Я называю человека из своей семьи. Работает
        // и для тех, кого я добавил сам, и для тех, чей запрос я принял
        // (там обратная связь изначально создаётся без метки).
        svr.Post("/api/family/relabel",
                 guarded([&fam](const httplib::Request& req, httplib::Response& res) {
                     auto body = nlohmann::json::parse(req.body);
                     auto uid = body.at("uid").get<std::int64_t>();
                     auto target = body.at("target").get<std::int64_t>();
                     auto relation = body.value("relation", std::string{});

                     // Связь должна существовать в любом направлении
                     bool linked = false;
                     for (const auto& l : fam.following(uid)) {
                         if (l.target == target) {
                             linked = true;
                             break;
                         }
                     }
                     if (!linked) {
                         for (const auto& l : fam.followers(uid)) {
                             if (l.follower == target) {
                                 linked = true;
                                 break;
                             }
                         }
                     }
                     if (!linked) {
                         errorResponse(res, "Этого человека нет в вашей семье", 404);
                         return;
                     }

                     // link() обновит метку связи uid → target, не трогая обратную
                     fam.link(uid, target, relation);
                     jsonResponse(res, {{"ok", true}, {"target", target}, {"relation", relation}});
                 }));

        if (std::filesystem::exists(static_dir)) {
            svr.set_mount_point("/", static_dir.string());
        }

        std::cout << "Pillio server starting on http://localhost:" << port << "\n";
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
