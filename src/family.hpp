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
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "models.hpp"  // ValidationError, NotFoundError, StorageError

namespace pillio {


struct Profile {
    std::int64_t chat_id{0};  ///< Telegram chat_id владельца профиля
    std::string name;         ///< Отображаемое имя (UTF-8)
    std::string share_code;   ///< Уникальный код для подписки (6 символов)
    std::string username;     ///< Telegram \@username (без @, нижний регистр)

    void validate() const {
        if (chat_id == 0) {
            throw ValidationError("Profile chat_id must not be zero");
        }
        if (name.empty()) {
            throw ValidationError("Profile name must not be empty");
        }
    }
};

/// @brief JSON-сериализация Profile.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Profile, chat_id, name, share_code, username)


struct FamilyRequest {
    std::string id;          ///< Уникальный идентификатор запроса
    std::int64_t from{0};    ///< chat_id отправителя
    std::string from_name;   ///< Имя отправителя (для показа получателю)
    std::int64_t to{0};      ///< chat_id получателя
    std::string relation;    ///< Как отправитель назвал получателя
    std::string created_at;  ///< Момент создания (ISO 8601)
    bool notified{false};    ///< Уведомление уже отправлено ботом
};

/// @brief JSON-сериализация FamilyRequest.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FamilyRequest, id, from, from_name, to, relation,
                                                created_at, notified)


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

/// @brief JSON-сериализация FamilyLink.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FamilyLink, follower, target, relation, created_at)


/**
 * @brief JSON-хранилище профилей, связей подписок и запросов в семью.
 */
class FamilyStore {
   public:
    /**
     * @brief Открывает (или создаёт пустое) хранилище семьи.
     * @param path путь к JSON-файлу family.json
     * @throws StorageError если каталог нельзя создать
     */
    explicit FamilyStore(std::filesystem::path path);

    /**
     * @brief Создаёт профиль (если его ещё нет) и возвращает его с share-кодом.
     * @param chat_id Telegram chat_id владельца профиля
     * @param name отображаемое имя
     * @param username Telegram \@username (сохраняется в нижнем регистре для поиска)
     * @return существующий или только что созданный профиль
     * @throws ValidationError если chat_id == 0 или имя пустое
     */
    Profile ensureProfile(std::int64_t chat_id, const std::string& name,
                          const std::string& username = "");

    /**
     * @brief Ищет профиль по 6-символьному share-коду (регистронезависимо).
     * @param code share-код
     * @return профиль или std::nullopt, если код не найден
     */
    std::optional<Profile> profileByCode(const std::string& code) const;

    /**
     * @brief Ищет профиль по Telegram chat_id.
     * @param chat_id идентификатор чата
     * @return профиль или std::nullopt, если не найден
     */
    std::optional<Profile> profileByChatId(std::int64_t chat_id) const;

    /**
     * @brief Ищет профиль по \@username (регистронезависимо, без ведущего @).
     * @param username имя пользователя
     * @return профиль или std::nullopt, если не найден
     */
    std::optional<Profile> profileByUsername(const std::string& username) const;

    /**
     * @brief Создаёт связь «follower следит за target».
     *
     * Идемпотентно: повторный вызов обновляет метку relation.
     * Обратная связь target → follower создаётся с пустой меткой.
     *
     * @param follower кто следит
     * @param target за кем следят
     * @param relation как follower назвал target (например, «мама»)
     * @throws ValidationError при нулевых id или follower == target
     */
    void link(std::int64_t follower, std::int64_t target, const std::string& relation);

    /**
     * @brief Удаляет связь в обоих направлениях.
     * @param follower кто следит
     * @param target за кем следят
     * @throws NotFoundError если связь отсутствует
     */
    void unlink(std::int64_t follower, std::int64_t target);

    /**
     * @brief Возвращает связи, где указанный пользователь — наблюдатель.
     * @param follower идентификатор наблюдателя
     * @return вектор связей follower → *
     */
    std::vector<FamilyLink> following(std::int64_t follower) const;

    /**
     * @brief Возвращает связи, где за указанным пользователем следят.
     * @param target идентификатор наблюдаемого
     * @return вектор связей * → target
     */
    std::vector<FamilyLink> followers(std::int64_t target) const;

    /**
     * @brief Создаёт запрос на добавление в семью (или возвращает существующий).
     * @param from chat_id отправителя
     * @param from_name имя отправителя (для показа получателю)
     * @param to chat_id получателя
     * @param relation как отправитель назвал получателя
     * @return созданный или существующий ожидающий запрос
     * @throws ValidationError при нулевых id, from == to или уже имеющейся связи
     */
    FamilyRequest addRequest(std::int64_t from, const std::string& from_name, std::int64_t to,
                             const std::string& relation);

    /**
     * @brief Возвращает входящие запросы для получателя.
     * @param to chat_id получателя
     * @return вектор запросов, адресованных to
     */
    std::vector<FamilyRequest> incomingRequests(std::int64_t to) const;

    /**
     * @brief Возвращает запросы, о которых бот ещё не уведомил (notified == false).
     * @return вектор неотправленных запросов
     */
    std::vector<FamilyRequest> unnotifiedRequests() const;

    /**
     * @brief Ищет запрос по его идентификатору.
     * @param id идентификатор запроса
     * @return запрос или std::nullopt, если не найден
     */
    std::optional<FamilyRequest> requestById(const std::string& id) const;

    /**
     * @brief Помечает запрос как доставленный получателю.
     * @param id идентификатор запроса
     */
    void markNotified(const std::string& id);

    /**
     * @brief Удаляет запрос (после принятия или отклонения).
     * @param id идентификатор запроса
     */
    void removeRequest(const std::string& id);

   private:
    std::filesystem::path path_;  ///< Путь к JSON-файлу хранилища семьи

    /// Читает и разбирает family.json (добавляя недостающие секции).
    nlohmann::json load() const;
    /// Атомарно записывает JSON в файл.
    void save(const nlohmann::json& data) const;
    /// Генерирует уникальный 6-символьный share-код.
    std::string generateCode(const nlohmann::json& data) const;
};

}  // namespace pillio
