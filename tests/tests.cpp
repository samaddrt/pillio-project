/**
 * @file tests.cpp
 * @brief Unit-тесты Pillio (doctest).
 *
 * Покрывает: модели (валидация), tracker (calculateNextIntake,
 * generateDailySlots, dailyProgress), storage (CRUD, ошибки).
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "analytics.hpp"
#include "checker.hpp"
#include "family.hpp"
#include "models.hpp"
#include "storage.hpp"
#include "tracker.hpp"

using namespace pillio;

// ════════════════════════════════════════════════════════════════════
// Helpers
// ════════════════════════════════════════════════════════════════════

/// Создаёт валидный Pill для тестов.
static Pill makePill(std::uint64_t id = 1, int interval = 8,
                     int start_h = 8, int start_m = 0) {
    Pill p;
    p.id = id;
    p.name = "TestVitamin";
    p.dosage = 500.0;
    p.unit = "mg";
    p.interval_hours = interval;
    p.start_hour = start_h;
    p.start_minute = start_m;
    return p;
}

// ════════════════════════════════════════════════════════════════════
// Model validation
// ════════════════════════════════════════════════════════════════════

TEST_CASE("Pill validation — positive") {
    auto p = makePill();
    CHECK_NOTHROW(p.validate());
}

TEST_CASE("Pill validation — empty name") {
    auto p = makePill();
    p.name = "";
    CHECK_THROWS_AS(p.validate(), ValidationError);
}

TEST_CASE("Pill validation — zero dosage") {
    auto p = makePill();
    p.dosage = 0;
    CHECK_THROWS_AS(p.validate(), ValidationError);
}

TEST_CASE("Pill validation — negative dosage") {
    auto p = makePill();
    p.dosage = -10.0;
    CHECK_THROWS_AS(p.validate(), ValidationError);
}

TEST_CASE("Pill validation — bad interval") {
    auto p = makePill();
    p.interval_hours = 0;
    CHECK_THROWS_AS(p.validate(), ValidationError);
    p.interval_hours = 25;
    CHECK_THROWS_AS(p.validate(), ValidationError);
}

TEST_CASE("Pill validation — bad start_hour") {
    auto p = makePill();
    p.start_hour = 24;
    CHECK_THROWS_AS(p.validate(), ValidationError);
    p.start_hour = -1;
    CHECK_THROWS_AS(p.validate(), ValidationError);
}

TEST_CASE("Schedule validation — zero pill_id") {
    Schedule s;
    s.pill_id = 0;
    s.scheduled_time = "2025-06-15T08:00:00";
    CHECK_THROWS_AS(s.validate(), ValidationError);
}

TEST_CASE("Schedule validation — empty time") {
    Schedule s;
    s.pill_id = 1;
    s.scheduled_time = "";
    CHECK_THROWS_AS(s.validate(), ValidationError);
}

// ════════════════════════════════════════════════════════════════════
// JSON round-trip
// ════════════════════════════════════════════════════════════════════

TEST_CASE("Pill JSON round-trip") {
    auto p = makePill(42, 12, 9, 30);
    nlohmann::json j = p;
    auto p2 = j.get<Pill>();
    CHECK(p2.id == 42);
    CHECK(p2.name == "TestVitamin");
    CHECK(p2.dosage == doctest::Approx(500.0));
    CHECK(p2.interval_hours == 12);
    CHECK(p2.start_hour == 9);
    CHECK(p2.start_minute == 30);
}

TEST_CASE("Schedule JSON round-trip") {
    Schedule s;
    s.pill_id = 7;
    s.scheduled_time = "2025-06-15T14:00:00";
    s.taken = true;
    s.taken_at = "2025-06-15T14:05:00";
    nlohmann::json j = s;
    auto s2 = j.get<Schedule>();
    CHECK(s2.pill_id == 7);
    CHECK(s2.taken == true);
    CHECK(s2.taken_at == "2025-06-15T14:05:00");
}

// ════════════════════════════════════════════════════════════════════
// Tracker — time helpers
// ════════════════════════════════════════════════════════════════════

TEST_CASE("formatTimePoint / parseTimePoint round-trip") {
    auto now = Clock::now();
    auto iso = formatTimePoint(now);
    CHECK(iso.size() >= 19);  // "YYYY-MM-DDTHH:MM:SS"
    auto parsed = parseTimePoint(iso);
    // Допускаем разницу <= 1 с из-за усечения субсекунд
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(
                    now - parsed).count();
    CHECK(std::abs(diff) <= 1);
}

TEST_CASE("parseTimePoint — invalid string") {
    CHECK_THROWS_AS(parseTimePoint("not-a-date"), ValidationError);
}

// ════════════════════════════════════════════════════════════════════
// Tracker — generateDailySlots
// ════════════════════════════════════════════════════════════════════

TEST_CASE("generateDailySlots — 8h interval, start 08:00") {
    auto pill = makePill(1, 8, 8, 0);
    auto day = parseTimePoint("2025-06-15T12:00:00");
    auto slots = generateDailySlots(pill, day);
    // Ожидаем: 08:00, 16:00 (24:00 уже за пределами)
    CHECK(slots.size() == 2);
    CHECK(slots[0].scheduled_time == "2025-06-15T08:00:00");
    CHECK(slots[1].scheduled_time == "2025-06-15T16:00:00");
}

TEST_CASE("generateDailySlots — 6h interval, start 06:00") {
    auto pill = makePill(2, 6, 6, 0);
    auto day = parseTimePoint("2025-06-15T12:00:00");
    auto slots = generateDailySlots(pill, day);
    // 06:00, 12:00, 18:00
    CHECK(slots.size() == 3);
}

TEST_CASE("generateDailySlots — 24h interval") {
    auto pill = makePill(3, 24, 10, 30);
    auto day = parseTimePoint("2025-06-15T12:00:00");
    auto slots = generateDailySlots(pill, day);
    CHECK(slots.size() == 1);
    CHECK(slots[0].scheduled_time == "2025-06-15T10:30:00");
}

// ════════════════════════════════════════════════════════════════════
// Tracker — calculateNextIntake
// ════════════════════════════════════════════════════════════════════

TEST_CASE("calculateNextIntake — no history, before first slot") {
    auto pill = makePill(1, 8, 8, 0);
    auto now = parseTimePoint("2025-06-15T07:00:00");
    std::vector<Schedule> history;
    auto next = calculateNextIntake(pill, history, now);
    auto next_str = formatTimePoint(next);
    CHECK(next_str == "2025-06-15T08:00:00");
}

TEST_CASE("calculateNextIntake — no history, between slots") {
    auto pill = makePill(1, 8, 8, 0);
    auto now = parseTimePoint("2025-06-15T10:00:00");
    std::vector<Schedule> history;
    auto next = calculateNextIntake(pill, history, now);
    auto next_str = formatTimePoint(next);
    CHECK(next_str == "2025-06-15T16:00:00");
}

TEST_CASE("calculateNextIntake — with taken history skips slot") {
    auto pill = makePill(1, 8, 8, 0);
    auto now = parseTimePoint("2025-06-15T07:00:00");

    Schedule taken;
    taken.pill_id = 1;
    taken.scheduled_time = "2025-06-15T08:00:00";
    taken.taken = true;
    taken.taken_at = "2025-06-15T08:05:00";
    std::vector<Schedule> history = {taken};

    auto next = calculateNextIntake(pill, history, now);
    auto next_str = formatTimePoint(next);
    // Should skip 08:00 (taken) and go to 16:00
    CHECK(next_str == "2025-06-15T16:00:00");
}

TEST_CASE("calculateNextIntake — all slots today passed, goes tomorrow") {
    auto pill = makePill(1, 8, 8, 0);
    auto now = parseTimePoint("2025-06-15T23:00:00");
    std::vector<Schedule> history;
    auto next = calculateNextIntake(pill, history, now);
    auto next_str = formatTimePoint(next);
    CHECK(next_str == "2025-06-16T08:00:00");
}

TEST_CASE("calculateNextIntake — invalid pill throws") {
    Pill bad;
    bad.name = "";  // invalid
    bad.dosage = 0;
    std::vector<Schedule> h;
    auto now = Clock::now();
    CHECK_THROWS_AS(calculateNextIntake(bad, h, now), ValidationError);
}

// ════════════════════════════════════════════════════════════════════
// Tracker — dailyProgress
// ════════════════════════════════════════════════════════════════════

TEST_CASE("dailyProgress — empty") {
    std::vector<Schedule> empty;
    CHECK(dailyProgress(empty) == doctest::Approx(0.0));
}

TEST_CASE("dailyProgress — all taken") {
    Schedule s1, s2;
    s1.pill_id = 1; s1.scheduled_time = "t1"; s1.taken = true;
    s2.pill_id = 1; s2.scheduled_time = "t2"; s2.taken = true;
    CHECK(dailyProgress({s1, s2}) == doctest::Approx(1.0));
}

TEST_CASE("dailyProgress — half taken") {
    Schedule s1, s2;
    s1.pill_id = 1; s1.scheduled_time = "t1"; s1.taken = true;
    s2.pill_id = 1; s2.scheduled_time = "t2"; s2.taken = false;
    CHECK(dailyProgress({s1, s2}) == doctest::Approx(0.5));
}

// ════════════════════════════════════════════════════════════════════
// Storage — CRUD & exceptions
// ════════════════════════════════════════════════════════════════════

TEST_CASE("Storage — add and get pill") {
    std::filesystem::path tmp = "./test_tmp_db.json";
    {
        Storage st(tmp);
        auto pill = makePill();
        pill.id = 0;  // will be auto-assigned
        auto saved = st.addPill(pill);
        CHECK(saved.id >= 1);
        auto all = st.getAllPills();
        CHECK(all.size() == 1);
        CHECK(all[0].name == "TestVitamin");

        auto found = st.getPillById(saved.id);
        CHECK(found.dosage == doctest::Approx(500.0));
    }
    std::filesystem::remove(tmp);
}

TEST_CASE("Storage — getPillById throws NotFoundError") {
    std::filesystem::path tmp = "./test_tmp_db2.json";
    {
        Storage st(tmp);
        CHECK_THROWS_AS(st.getPillById(999), NotFoundError);
    }
    std::filesystem::remove(tmp);
}

TEST_CASE("Storage — remove pill") {
    std::filesystem::path tmp = "./test_tmp_db3.json";
    {
        Storage st(tmp);
        auto saved = st.addPill(makePill());
        st.removePill(saved.id);
        CHECK(st.getAllPills().empty());
    }
    std::filesystem::remove(tmp);
}

TEST_CASE("Storage — markTaken throws when not found") {
    std::filesystem::path tmp = "./test_tmp_db4.json";
    {
        Storage st(tmp);
        CHECK_THROWS_AS(
            st.markTaken(999, "2025-01-01T00:00:00", "now"),
            NotFoundError);
    }
    std::filesystem::remove(tmp);
}

TEST_CASE("Storage — corrupted file throws StorageError") {
    std::filesystem::path tmp = "./test_tmp_corrupt.json";
    {
        std::ofstream ofs(tmp);
        ofs << "NOT VALID JSON {{{";
    }
    CHECK_THROWS_AS({
        Storage st(tmp);
        st.getAllPills();
    }, StorageError);
    std::filesystem::remove(tmp);
}

// ══════════════════════════════════════════════════════════════════
// Analytics — computeAnalytics
// ══════════════════════════════════════════════════════════════════

TEST_CASE("Analytics — empty schedules") {
    std::vector<Schedule> empty;
    auto now = Clock::now();
    auto r = computeAnalytics(empty, now);
    CHECK(r.best_streak == 0);
    CHECK(r.current_streak == 0);
    CHECK(r.total_taken == 0);
    CHECK(r.total_missed == 0);
    CHECK(r.avg_delay_minutes == doctest::Approx(0.0));
}

TEST_CASE("Analytics — counts taken and missed") {
    std::vector<Schedule> scheds;
    Schedule s1; s1.pill_id=1; s1.scheduled_time="2025-06-15T08:00:00"; s1.taken=true; s1.taken_at="2025-06-15T08:05:00";
    Schedule s2; s2.pill_id=1; s2.scheduled_time="2025-06-15T16:00:00"; s2.taken=false; s2.taken_at="";
    Schedule s3; s3.pill_id=1; s3.scheduled_time="2025-06-16T08:00:00"; s3.taken=true; s3.taken_at="2025-06-16T08:10:00";
    scheds = {s1, s2, s3};
    auto now = parseTimePoint("2025-06-17T12:00:00");
    auto r = computeAnalytics(scheds, now);
    CHECK(r.total_taken == 2);
    CHECK(r.total_missed == 1);
}

TEST_CASE("Analytics — avg delay") {
    Schedule s1; s1.pill_id=1; s1.scheduled_time="2025-06-15T08:00:00"; s1.taken=true; s1.taken_at="2025-06-15T08:10:00";
    Schedule s2; s2.pill_id=1; s2.scheduled_time="2025-06-15T16:00:00"; s2.taken=true; s2.taken_at="2025-06-15T16:20:00";
    auto now = parseTimePoint("2025-06-16T12:00:00");
    auto r = computeAnalytics({s1, s2}, now);
    // (10 + 20) / 2 = 15 minutes
    CHECK(r.avg_delay_minutes == doctest::Approx(15.0));
}

TEST_CASE("Analytics — adherenceForPeriod template") {
    Schedule s1; s1.pill_id=1; s1.scheduled_time="t1"; s1.taken=true;
    Schedule s2; s2.pill_id=2; s2.scheduled_time="t2"; s2.taken=false;
    Schedule s3; s3.pill_id=1; s3.scheduled_time="t3"; s3.taken=true;
    auto adh = adherenceForPeriod<std::function<bool(const Schedule&)>>(
        {s1, s2, s3},
        [](const Schedule& s) { return s.pill_id == 1; }
    );
    CHECK(adh == doctest::Approx(1.0)); // 2/2 for pill_id=1
}

// ══════════════════════════════════════════════════════════════════
// Checker — checkInteractions
// ══════════════════════════════════════════════════════════════════

TEST_CASE("Checker — no interactions for safe drugs") {
    auto result = checkInteractions({"Vitamin C", "Zinc"});
    CHECK(result.empty());
}

TEST_CASE("Checker — detects warfarin + aspirin") {
    auto result = checkInteractions({"Warfarin", "Aspirin"});
    CHECK(result.size() == 1);
    CHECK(result[0].severity == Severity::Dangerous);
}

TEST_CASE("Checker — case insensitive matching") {
    auto result = checkInteractions({"WARFARIN", "aspirin"});
    CHECK(result.size() == 1);
}

TEST_CASE("Checker — detects multiple interactions") {
    auto result = checkInteractions({"Warfarin", "Aspirin", "Ibuprofen"});
    // warfarin+aspirin, warfarin+ibuprofen, aspirin+ibuprofen = 3
    CHECK(result.size() == 3);
}

TEST_CASE("Checker — single drug is safe") {
    auto result = checkInteractions({"Aspirin"});
    CHECK(result.empty());
}

TEST_CASE("Checker — empty list is safe") {
    auto result = checkInteractions({});
    CHECK(result.empty());
}

TEST_CASE("Checker — database has entries") {
    CHECK(interactionDatabaseSize() >= 15);
}

// ══════════════════════════════════════════════════════════════════
// FamilyStore — семейный доступ
// ══════════════════════════════════════════════════════════════════

TEST_CASE("Family — ensureProfile creates profile with share code") {
    std::filesystem::path tmp = "./test_tmp_family1.json";
    {
        FamilyStore fs(tmp);
        auto p = fs.ensureProfile(111, "Мама");
        CHECK(p.chat_id == 111);
        CHECK(p.name == "Мама");
        CHECK(p.share_code.size() == 6);
    }
    std::filesystem::remove(tmp);
}

TEST_CASE("Family — ensureProfile is idempotent (stable code)") {
    std::filesystem::path tmp = "./test_tmp_family2.json";
    {
        FamilyStore fs(tmp);
        auto p1 = fs.ensureProfile(111, "Мама");
        auto p2 = fs.ensureProfile(111, "Мама обновлённая");
        CHECK(p1.share_code == p2.share_code);  // код не меняется
        CHECK(p2.name == "Мама обновлённая");    // имя обновилось
    }
    std::filesystem::remove(tmp);
}

TEST_CASE("Family — profileByCode finds profile (case-insensitive)") {
    std::filesystem::path tmp = "./test_tmp_family3.json";
    {
        FamilyStore fs(tmp);
        auto p = fs.ensureProfile(111, "Мама");
        auto found = fs.profileByCode(p.share_code);
        REQUIRE(found.has_value());
        CHECK(found->chat_id == 111);
        // нижний регистр тоже находит
        std::string lower = p.share_code;
        for (auto& c : lower) c = static_cast<char>(std::tolower(c));
        CHECK(fs.profileByCode(lower).has_value());
    }
    std::filesystem::remove(tmp);
}

TEST_CASE("Family — profileByCode returns nullopt for unknown") {
    std::filesystem::path tmp = "./test_tmp_family4.json";
    {
        FamilyStore fs(tmp);
        CHECK_FALSE(fs.profileByCode("ZZZZZZ").has_value());
    }
    std::filesystem::remove(tmp);
}

TEST_CASE("Family — link creates following/followers relationship") {
    std::filesystem::path tmp = "./test_tmp_family5.json";
    {
        FamilyStore fs(tmp);
        fs.ensureProfile(111, "Мама");
        fs.ensureProfile(222, "Сын");
        fs.link(222, 111, "сын");  // сын следит за мамой

        auto following = fs.following(222);
        REQUIRE(following.size() == 1);
        CHECK(following[0].target == 111);
        CHECK(following[0].relation == "сын");

        auto followers = fs.followers(111);
        REQUIRE(followers.size() == 1);
        CHECK(followers[0].follower == 222);
    }
    std::filesystem::remove(tmp);
}

TEST_CASE("Family — link is idempotent (no duplicates)") {
    std::filesystem::path tmp = "./test_tmp_family6.json";
    {
        FamilyStore fs(tmp);
        fs.link(222, 111, "сын");
        fs.link(222, 111, "сынок");  // повтор обновляет relation
        auto following = fs.following(222);
        REQUIRE(following.size() == 1);
        CHECK(following[0].relation == "сынок");
    }
    std::filesystem::remove(tmp);
}

TEST_CASE("Family — cannot follow yourself") {
    std::filesystem::path tmp = "./test_tmp_family7.json";
    {
        FamilyStore fs(tmp);
        CHECK_THROWS_AS(fs.link(111, 111, "я"), ValidationError);
    }
    std::filesystem::remove(tmp);
}

TEST_CASE("Family — unlink removes relationship") {
    std::filesystem::path tmp = "./test_tmp_family8.json";
    {
        FamilyStore fs(tmp);
        fs.link(222, 111, "сын");
        fs.unlink(222, 111);
        CHECK(fs.following(222).empty());
    }
    std::filesystem::remove(tmp);
}

TEST_CASE("Family — unlink throws when link absent") {
    std::filesystem::path tmp = "./test_tmp_family9.json";
    {
        FamilyStore fs(tmp);
        CHECK_THROWS_AS(fs.unlink(222, 111), NotFoundError);
    }
    std::filesystem::remove(tmp);
}

TEST_CASE("Family — profile validation rejects zero id / empty name") {
    Profile p;
    p.chat_id = 0;
    p.name = "X";
    CHECK_THROWS_AS(p.validate(), ValidationError);
    p.chat_id = 5;
    p.name = "";
    CHECK_THROWS_AS(p.validate(), ValidationError);
}

TEST_CASE("Family — reverse link has empty relation") {
    std::filesystem::path tmp = "./test_tmp_family_rev.json";
    {
        FamilyStore fs(tmp);
        fs.link(222, 111, "сын");  // 222 называет 111 «сын»
        // Обратная связь 111 → 222 должна существовать, но без метки.
        auto following111 = fs.following(111);
        REQUIRE(following111.size() == 1);
        CHECK(following111[0].target == 222);
        CHECK(following111[0].relation == "");
    }
    std::filesystem::remove(tmp);
}

TEST_CASE("Family — profileByUsername (case-insensitive, strips @)") {
    std::filesystem::path tmp = "./test_tmp_family_uname.json";
    {
        FamilyStore fs(tmp);
        fs.ensureProfile(111, "Мама", "MamaUser");
        auto p = fs.profileByUsername("@mamauser");
        REQUIRE(p.has_value());
        CHECK(p->chat_id == 111);
        CHECK(fs.profileByUsername("nope").has_value() == false);
    }
    std::filesystem::remove(tmp);
}

TEST_CASE("Family — request create / incoming / accept creates link") {
    std::filesystem::path tmp = "./test_tmp_family_req.json";
    {
        FamilyStore fs(tmp);
        fs.ensureProfile(111, "Алиса", "alice");
        fs.ensureProfile(222, "Боб", "bob");

        auto r = fs.addRequest(111, "Алиса", 222, "сестра");
        CHECK(r.from == 111);
        CHECK(r.to == 222);
        CHECK(r.notified == false);

        auto incoming = fs.incomingRequests(222);
        REQUIRE(incoming.size() == 1);
        CHECK(incoming[0].id == r.id);

        // Повторный запрос идемпотентен
        auto r2 = fs.addRequest(111, "Алиса", 222, "сестра");
        CHECK(r2.id == r.id);

        // Принятие: создаём связь и удаляем запрос
        auto found = fs.requestById(r.id);
        REQUIRE(found.has_value());
        fs.link(found->from, found->to, found->relation);
        fs.removeRequest(r.id);

        CHECK(fs.incomingRequests(222).empty());
        auto following = fs.following(111);
        REQUIRE(following.size() == 1);
        CHECK(following[0].target == 222);
        CHECK(following[0].relation == "сестра");
    }
    std::filesystem::remove(tmp);
}

TEST_CASE("Family — cannot request yourself / duplicate when linked") {
    std::filesystem::path tmp = "./test_tmp_family_req2.json";
    {
        FamilyStore fs(tmp);
        CHECK_THROWS_AS(fs.addRequest(111, "A", 111, "я"), ValidationError);
        fs.link(111, 222, "брат");
        // Уже в семье — повторный запрос отклоняется
        CHECK_THROWS_AS(fs.addRequest(111, "A", 222, "брат"), ValidationError);
    }
    std::filesystem::remove(tmp);
}
