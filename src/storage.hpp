#pragma once
/**
 * @file storage.hpp
 * @brief CRUD-хранилище (лекарства + расписание) в одном JSON-файле.
 */

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "models.hpp"

namespace pillio {

/**
 * @brief Файловое хранилище лекарств и расписания приёмов.
 *
 * Внешней БД нет: каждый метод выполняет полный цикл
 * «прочитать файл → изменить → записать обратно».
 */
class Storage {
   public:
    /**
     * @brief Открывает (или создаёт пустое) хранилище по указанному пути.
     * @param db_path путь к JSON-файлу хранилища
     * @throws StorageError если каталог нельзя создать или файл недоступен
     */
    explicit Storage(const std::filesystem::path& db_path);

    /**
     * @brief Возвращает все лекарства.
     * @return вектор всех сохранённых Pill
     */
    std::vector<Pill> getAllPills() const;

    /**
     * @brief Находит лекарство по идентификатору.
     * @param id идентификатор лекарства
     * @return найденное лекарство
     * @throws NotFoundError если лекарство с таким id отсутствует
     */
    Pill getPillById(std::uint64_t id) const;

    /**
     * @brief Добавляет лекарство, присваивая новый уникальный id.
     * @param pill лекарство без id (id будет назначен автоматически)
     * @return сохранённое лекарство с проставленным id
     * @throws ValidationError если поля лекарства некорректны
     */
    Pill addPill(Pill pill);

    /**
     * @brief Удаляет лекарство и все связанные записи расписания.
     * @param id идентификатор удаляемого лекарства
     * @throws NotFoundError если лекарство с таким id отсутствует
     */
    void removePill(std::uint64_t id);

    /**
     * @brief Возвращает все записи расписания.
     * @return вектор всех Schedule
     */
    std::vector<Schedule> getAllSchedules() const;

    /**
     * @brief Возвращает записи расписания за конкретную дату.
     * @param date_prefix префикс даты в формате "YYYY-MM-DD"
     * @return записи, чьё scheduled_time начинается с этого префикса
     */
    std::vector<Schedule> getSchedulesForDate(const std::string& date_prefix) const;

    /**
     * @brief Добавляет запись расписания.
     * @param entry запись для добавления
     * @throws ValidationError если запись некорректна
     */
    void addSchedule(Schedule entry);

    /**
     * @brief Отмечает конкретный приём как выполненный.
     * @param pill_id идентификатор лекарства
     * @param scheduled_time запланированное время слота (ISO 8601)
     * @param taken_at фактическое время приёма (ISO 8601)
     * @throws NotFoundError если такой слот не найден
     */
    void markTaken(std::uint64_t pill_id, const std::string& scheduled_time,
                   const std::string& taken_at);

   private:
    std::filesystem::path db_path_;  ///< Путь к JSON-файлу хранилища

    /// Читает и разбирает JSON-файл хранилища.
    nlohmann::json load() const;
    /// Атомарно записывает JSON в файл (через временный файл + rename).
    void save(const nlohmann::json& data) const;
    /// Возвращает следующий свободный id лекарства (max + 1).
    std::uint64_t nextPillId(const nlohmann::json& data) const;
};

}  // namespace pillio
