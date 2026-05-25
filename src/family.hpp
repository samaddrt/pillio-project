#pragma once
/**
 * @file family.hpp
 * @brief Семейный доступ — профили, share-коды, связи подписок.
 *
 * Позволяет делиться статусом приёма лекарств с близкими:
 * сын подписывается по 6-символьному коду и видит, приняла ли
 * мама лекарство. Данные в отдельном family.json.
 */

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models.hpp"  // ValidationError, NotFoundError, StorageError

namespace pillio {

// ── Profile ──────────────────────────────────────────────────────

struct Profile {
    std::int64_t chat_id{0};   ///< Telegram chat_id владельца профиля
    std::string name;          ///< Отображаемое имя (UTF-8)
    std::string share_code;    ///< Уникальный код для подписки (6 символов)
    std::string username;      ///< Telegram @username (без @, нижний регистр)

    void validate() const {
        if (chat_id == 0) {
            throw ValidationError("Profile chat_id must not be zero");
        }
        if (name.empty()) {
            throw ValidationError("Profile name must not be empty");
        }
    }
};

inline void to_json(nlohmann::json& j, const Profile& p) {
    j = nlohmann::json{{"chat_id", p.chat_id},
                       {"name", p.name},
                       {"share_code", p.share_code},
                       {"username", p.username}};
}

inline void from_json(const nlohmann::json& j, Profile& p) {
    j.at("chat_id").get_to(p.chat_id);
    j.at("name").get_to(p.name);
    p.share_code = j.value("share_code", std::string{});
    p.username = j.value("username", std::string{});
}

// ── FamilyRequest — запрос на добавление в семью по @username ─────

struct FamilyRequest {
    std::string id;            ///< Уникальный идентификатор запроса
    std::int64_t from{0};      ///< chat_id отправителя
    std::string from_name;     ///< Имя отправителя (для показа получателю)
    std::int64_t to{0};        ///< chat_id получателя
    std::string relation;      ///< Как отправитель назвал получателя
    std::string created_at;    ///< Момент создания (ISO 8601)
    bool notified{false};      ///< Уведомление уже отправлено ботом
};

inline void to_json(nlohmann::json& j, const FamilyRequest& r) {
    j = nlohmann::json{{"id", r.id},
                       {"from", r.from},
                       {"from_name", r.from_name},
                       {"to", r.to},
                       {"relation", r.relation},
                       {"created_at", r.created_at},
                       {"notified", r.notified}};
}

inline void from_json(const nlohmann::json& j, FamilyRequest& r) {
    r.id = j.value("id", std::string{});
    r.from = j.value("from", static_cast<std::int64_t>(0));
    r.from_name = j.value("from_name", std::string{});
    r.to = j.value("to", static_cast<std::int64_t>(0));
    r.relation = j.value("relation", std::string{});
    r.created_at = j.value("created_at", std::string{});
    r.notified = j.value("notified", false);
}

// ── FamilyLink — подписка «кто → за кем» ─────────────────────────

struct FamilyLink {
    std::int64_t follower{0};  ///< chat_id того, кто следит
    std::int64_t target{0};    ///< chat_id того, за кем следят
    std::string relation;      ///< Метка отношения, заданная подписчиком
    std::string created_at;    ///< Момент создания связи (ISO 8601)

    void validate() const {
        if (follower == 0 || target == 0) {
            throw ValidationError("FamilyLink requires non-zero follower/target");
        }
        if (follower == target) {
            throw ValidationError("Cannot follow yourself");
        }
    }
};

inline void to_json(nlohmann::json& j, const FamilyLink& l) {
    j = nlohmann::json{{"follower", l.follower},
                       {"target", l.target},
                       {"relation", l.relation},
                       {"created_at", l.created_at}};
}

inline void from_json(const nlohmann::json& j, FamilyLink& l) {
    j.at("follower").get_to(l.follower);
    j.at("target").get_to(l.target);
    l.relation = j.value("relation", std::string{});
    l.created_at = j.value("created_at", std::string{});
}

// ── FamilyStore — JSON-хранилище профилей и связей ───────────────

class FamilyStore {
public:
    explicit FamilyStore(std::filesystem::path path);

    /// Создаёт профиль (если нет) и возвращает его с share_code.
    /// username (если задан) сохраняется в нижнем регистре для поиска.
    Profile ensureProfile(std::int64_t chat_id, const std::string& name,
                          const std::string& username = "");
    /// Поиск по 6-символьному коду (регистронезависимый).
    std::optional<Profile> profileByCode(const std::string& code) const;
    std::optional<Profile> profileByChatId(std::int64_t chat_id) const;
    /// Поиск по @username (регистронезависимый, без ведущего @).
    std::optional<Profile> profileByUsername(const std::string& username) const;

    /// Подписка follower → target (идемпотентно: повтор обновляет relation).
    /// Обратная связь target → follower создаётся с пустой меткой.
    void link(std::int64_t follower, std::int64_t target,
              const std::string& relation);
    void unlink(std::int64_t follower, std::int64_t target);

    std::vector<FamilyLink> following(std::int64_t follower) const;
    std::vector<FamilyLink> followers(std::int64_t target) const;

    // ── Запросы на добавление в семью ─────────────────────────────
    /// Создаёт запрос from → to (или возвращает существующий pending).
    FamilyRequest addRequest(std::int64_t from, const std::string& from_name,
                             std::int64_t to, const std::string& relation);
    /// Входящие запросы для получателя to.
    std::vector<FamilyRequest> incomingRequests(std::int64_t to) const;
    /// Запросы, о которых бот ещё не уведомил (notified == false).
    std::vector<FamilyRequest> unnotifiedRequests() const;
    std::optional<FamilyRequest> requestById(const std::string& id) const;
    void markNotified(const std::string& id);
    void removeRequest(const std::string& id);

private:
    std::filesystem::path path_;
    nlohmann::json load() const;
    void save(const nlohmann::json& data) const;
    std::string generateCode(const nlohmann::json& data) const;
};

}  // namespace pillio
