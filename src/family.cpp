// family.cpp — хранилище семейных связей (family.json).

#include "family.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <random>

#include "tracker.hpp"  // formatTimePoint, Clock

namespace pillio {

namespace {

std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Нормализует @username: убирает ведущий @ и приводит к нижнему регистру.
std::string normUsername(std::string s) {
    if (!s.empty() && s.front() == '@') s.erase(s.begin());
    return toLower(std::move(s));
}

// Генерирует случайный идентификатор запроса (12 hex-символов).
std::string makeRequestId() {
    static const char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);
    std::string id;
    for (int i = 0; i < 12; ++i) id += hex[dist(gen)];
    return id;
}

}  // namespace

FamilyStore::FamilyStore(std::filesystem::path path) : path_(std::move(path)) {
    try {
        auto parent = path_.parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent)) {
            std::filesystem::create_directories(parent);
        }
    } catch (const std::filesystem::filesystem_error& e) {
        throw StorageError(std::string("Cannot create family dir: ") + e.what());
    }

    if (!std::filesystem::exists(path_)) {
        nlohmann::json initial = {{"profiles", nlohmann::json::array()},
                                  {"links", nlohmann::json::array()},
                                  {"requests", nlohmann::json::array()}};
        save(initial);
    }
}

nlohmann::json FamilyStore::load() const {
    std::ifstream ifs(path_);
    if (!ifs.is_open()) {
        throw StorageError("Cannot open family file: " + path_.string());
    }
    try {
        nlohmann::json data;
        ifs >> data;
        if (!data.contains("profiles")) data["profiles"] = nlohmann::json::array();
        if (!data.contains("links")) data["links"] = nlohmann::json::array();
        if (!data.contains("requests")) data["requests"] = nlohmann::json::array();
        return data;
    } catch (const nlohmann::json::parse_error& e) {
        throw StorageError(std::string("Corrupted family file: ") + e.what());
    }
}

void FamilyStore::save(const nlohmann::json& data) const {
    std::ofstream ofs(path_);
    if (!ofs.is_open()) {
        throw StorageError("Cannot write family file: " + path_.string());
    }
    ofs << data.dump(2);
    if (ofs.fail()) {
        throw StorageError("Write failed for: " + path_.string());
    }
}

std::string FamilyStore::generateCode(const nlohmann::json& data) const {
    // Без 0/O/1/I — чтобы не путать при ручном вводе
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    static const std::size_t alpha_len = sizeof(alphabet) - 1;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> dist(0, alpha_len - 1);

    auto isTaken = [&data](const std::string& code) {
        for (const auto& p : data["profiles"]) {
            if (p.value("share_code", std::string{}) == code) return true;
        }
        return false;
    };

    std::string code;
    do {
        code.clear();
        for (int i = 0; i < 6; ++i) code += alphabet[dist(gen)];
    } while (isTaken(code));
    return code;
}

// Профили

Profile FamilyStore::ensureProfile(std::int64_t chat_id, const std::string& name,
                                   const std::string& username) {
    auto uname = normUsername(username);

    Profile candidate;
    candidate.chat_id = chat_id;
    candidate.name = name;
    candidate.username = uname;
    candidate.validate();

    auto data = load();
    for (auto& p : data["profiles"]) {
        if (p.value("chat_id", static_cast<std::int64_t>(0)) == chat_id) {
            if (!name.empty()) p["name"] = name;
            if (!uname.empty()) p["username"] = uname;
            if (p.value("share_code", std::string{}).empty()) {
                p["share_code"] = generateCode(data);
            }
            save(data);
            return p.get<Profile>();
        }
    }

    candidate.share_code = generateCode(data);
    data["profiles"].push_back(candidate);
    save(data);
    return candidate;
}

std::optional<Profile> FamilyStore::profileByCode(const std::string& code) const {
    auto norm = toUpper(code);
    auto data = load();
    for (const auto& p : data["profiles"]) {
        if (toUpper(p.value("share_code", std::string{})) == norm) {
            return p.get<Profile>();
        }
    }
    return std::nullopt;
}

std::optional<Profile> FamilyStore::profileByChatId(std::int64_t chat_id) const {
    auto data = load();
    for (const auto& p : data["profiles"]) {
        if (p.value("chat_id", static_cast<std::int64_t>(0)) == chat_id) {
            return p.get<Profile>();
        }
    }
    return std::nullopt;
}

std::optional<Profile> FamilyStore::profileByUsername(const std::string& username) const {
    auto norm = normUsername(username);
    if (norm.empty()) return std::nullopt;
    auto data = load();
    for (const auto& p : data["profiles"]) {
        if (toLower(p.value("username", std::string{})) == norm) {
            return p.get<Profile>();
        }
    }
    return std::nullopt;
}

// Связи

void FamilyStore::link(std::int64_t follower, std::int64_t target,
                       const std::string& relation) {
    FamilyLink l;
    l.follower = follower;
    l.target = target;
    l.relation = relation;
    l.validate();

    auto data = load();
    auto now_str = formatTimePoint(Clock::now());

    // A → B (создаём или обновляем)
    bool foundAB = false;
    for (auto& e : data["links"]) {
        if (e.value("follower", static_cast<std::int64_t>(0)) == follower &&
            e.value("target", static_cast<std::int64_t>(0)) == target) {
            e["relation"] = relation;
            foundAB = true;
            break;
        }
    }
    if (!foundAB) {
        l.created_at = now_str;
        data["links"].push_back(l);
    }

    // B → A (обратная связь — создаём если нет)
    bool foundBA = false;
    for (const auto& e : data["links"]) {
        if (e.value("follower", static_cast<std::int64_t>(0)) == target &&
            e.value("target", static_cast<std::int64_t>(0)) == follower) {
            foundBA = true;
            break;
        }
    }
    if (!foundBA) {
        // Обратная связь создаётся с пустой меткой: получатель сам не давал
        // имени отправителю (иначе он видел бы себя «мамой» и т.п.).
        FamilyLink rev;
        rev.follower = target;
        rev.target = follower;
        rev.relation = "";
        rev.created_at = now_str;
        rev.validate();
        data["links"].push_back(rev);
    }

    save(data);
}

void FamilyStore::unlink(std::int64_t follower, std::int64_t target) {
    auto data = load();
    auto& links = data["links"];
    // Удаляем обе связи: A→B и B→A
    auto it = std::remove_if(
        links.begin(), links.end(), [follower, target](const nlohmann::json& e) {
            auto f = e.value("follower", static_cast<std::int64_t>(0));
            auto t = e.value("target", static_cast<std::int64_t>(0));
            return (f == follower && t == target) || (f == target && t == follower);
        });
    if (it == links.end()) {
        throw NotFoundError("Family link not found");
    }
    links.erase(it, links.end());
    save(data);
}

std::vector<FamilyLink> FamilyStore::following(std::int64_t follower) const {
    auto data = load();
    std::vector<FamilyLink> result;
    for (const auto& e : data["links"]) {
        if (e.value("follower", static_cast<std::int64_t>(0)) == follower) {
            result.push_back(e.get<FamilyLink>());
        }
    }
    return result;
}

std::vector<FamilyLink> FamilyStore::followers(std::int64_t target) const {
    auto data = load();
    std::vector<FamilyLink> result;
    for (const auto& e : data["links"]) {
        if (e.value("target", static_cast<std::int64_t>(0)) == target) {
            result.push_back(e.get<FamilyLink>());
        }
    }
    return result;
}

// Запросы

FamilyRequest FamilyStore::addRequest(std::int64_t from, const std::string& from_name,
                                      std::int64_t to, const std::string& relation) {
    if (from == 0 || to == 0) {
        throw ValidationError("Request requires non-zero from/to");
    }
    if (from == to) {
        throw ValidationError("Cannot request yourself");
    }

    auto data = load();

    // Если уже подписан — нет смысла слать запрос.
    for (const auto& e : data["links"]) {
        if (e.value("follower", static_cast<std::int64_t>(0)) == from &&
            e.value("target", static_cast<std::int64_t>(0)) == to) {
            throw ValidationError("Already in your family");
        }
    }

    // Идемпотентность: возвращаем существующий pending-запрос from → to.
    for (const auto& e : data["requests"]) {
        if (e.value("from", static_cast<std::int64_t>(0)) == from &&
            e.value("to", static_cast<std::int64_t>(0)) == to) {
            return e.get<FamilyRequest>();
        }
    }

    FamilyRequest r;
    r.id = makeRequestId();
    r.from = from;
    r.from_name = from_name.empty() ? std::string{"Пользователь"} : from_name;
    r.to = to;
    r.relation = relation;
    r.created_at = formatTimePoint(Clock::now());
    r.notified = false;
    data["requests"].push_back(r);
    save(data);
    return r;
}

std::vector<FamilyRequest> FamilyStore::incomingRequests(std::int64_t to) const {
    auto data = load();
    std::vector<FamilyRequest> result;
    for (const auto& e : data["requests"]) {
        if (e.value("to", static_cast<std::int64_t>(0)) == to) {
            result.push_back(e.get<FamilyRequest>());
        }
    }
    return result;
}

std::vector<FamilyRequest> FamilyStore::unnotifiedRequests() const {
    auto data = load();
    std::vector<FamilyRequest> result;
    for (const auto& e : data["requests"]) {
        if (!e.value("notified", false)) {
            result.push_back(e.get<FamilyRequest>());
        }
    }
    return result;
}

std::optional<FamilyRequest> FamilyStore::requestById(const std::string& id) const {
    auto data = load();
    for (const auto& e : data["requests"]) {
        if (e.value("id", std::string{}) == id) {
            return e.get<FamilyRequest>();
        }
    }
    return std::nullopt;
}

void FamilyStore::markNotified(const std::string& id) {
    auto data = load();
    for (auto& e : data["requests"]) {
        if (e.value("id", std::string{}) == id) {
            e["notified"] = true;
            break;
        }
    }
    save(data);
}

void FamilyStore::removeRequest(const std::string& id) {
    auto data = load();
    auto& reqs = data["requests"];
    reqs.erase(std::remove_if(reqs.begin(), reqs.end(),
                              [&id](const nlohmann::json& e) {
                                  return e.value("id", std::string{}) == id;
                              }),
               reqs.end());
    save(data);
}

}  // namespace pillio
