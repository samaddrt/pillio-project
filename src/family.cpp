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
                                  {"links", nlohmann::json::array()}};
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

Profile FamilyStore::ensureProfile(std::int64_t chat_id, const std::string& name) {
    Profile candidate;
    candidate.chat_id = chat_id;
    candidate.name = name;
    candidate.validate();

    auto data = load();
    for (auto& p : data["profiles"]) {
        if (p.value("chat_id", static_cast<std::int64_t>(0)) == chat_id) {
            if (!name.empty()) p["name"] = name;
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
        FamilyLink rev;
        rev.follower = target;
        rev.target = follower;
        rev.relation = relation;
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

}  // namespace pillio
