"""
Pillio Telegram Bot — бот-компаньон для Mini App трекера лекарств.

Функции:
  /start   — приветствие + кнопка открытия Mini App
  /help    — список команд
  /today   — сводка по приёмам на сегодня
  /pills   — список лекарств
  /remind  — принудительная проверка напоминаний

Фоновый процесс каждые 60 секунд проверяет расписание через C++ API
и отправляет напоминания о пропущенных/предстоящих приёмах.
"""

import os
import asyncio
import logging
from datetime import datetime

import requests
from dotenv import load_dotenv
from telegram import (
    Update,
    InlineKeyboardButton,
    InlineKeyboardMarkup,
    WebAppInfo,
    BotCommand,
)
from telegram.ext import (
    Application,
    CommandHandler,
    ContextTypes,
)

# ── Конфигурация ──────────────────────────────────────────────────

load_dotenv()

BOT_TOKEN = os.getenv("BOT_TOKEN", "")
WEBAPP_URL = os.getenv("WEBAPP_URL", "https://your-domain.com")
API_URL = os.getenv("API_URL", "http://localhost:8080")

logging.basicConfig(
    format="%(asctime)s [%(name)s] %(levelname)s: %(message)s",
    level=logging.INFO,
)
logger = logging.getLogger("pillio_bot")

# ── Вспомогательные функции ───────────────────────────────────────

def api_get(path: str, uid: str = "") -> dict | list | None:
    """GET-запрос к C++ API с X-Pillio-Uid header."""
    try:
        headers = {}
        if uid:
            headers["X-Pillio-Uid"] = uid
        r = requests.get(f"{API_URL}{path}", headers=headers, timeout=5)
        r.raise_for_status()
        return r.json()
    except Exception as e:
        logger.error("API GET %s (uid=%s) failed: %s", path, uid, e)
        return None


def api_post(path: str, data: dict, uid: str = "") -> dict | None:
    """POST-запрос к C++ API с X-Pillio-Uid header."""
    try:
        headers = {}
        if uid:
            headers["X-Pillio-Uid"] = uid
        r = requests.post(f"{API_URL}{path}", json=data, headers=headers, timeout=5)
        r.raise_for_status()
        return r.json()
    except Exception as e:
        logger.error("API POST %s (uid=%s) failed: %s", path, uid, e)
        return None


def today_iso() -> str:
    """Текущая дата в формате YYYY-MM-DD."""
    return datetime.now().strftime("%Y-%m-%d")


MEAL_LABELS = {
    "before": "🍽 До еды",
    "during": "🍽 Во время еды",
    "after": "🍽 После еды",
}

MEAL_HINTS = {
    "before": "за 30 мин до еды",
    "during": "прямо во время еды",
    "after": "сразу после еды",
}


# ── Команды бота ──────────────────────────────────────────────────

async def cmd_start(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    """Приветствие + кнопка Mini App."""
    user = update.effective_user
    name = user.first_name if user else "друг"
    uid = str(user.id) if user else ""
    chat_id = update.effective_chat.id

    # Регистрируем telegram_id -> chat_id маппинг на сервере для напоминаний
    api_post("/api/bot/register", {"telegram_id": uid, "chat_id": chat_id}, uid=uid)

    keyboard = InlineKeyboardMarkup([
        [InlineKeyboardButton(
            "💊 Открыть Pillio",
            web_app=WebAppInfo(url=WEBAPP_URL),
        )],
        [InlineKeyboardButton(
            "📊 Мои приёмы сегодня",
            callback_data="today",
        )],
    ])

    await update.message.reply_text(
        f"Привет, {name}! 👋\n\n"
        f"Я — *Pillio*, твой персональный трекер лекарств 💊\n\n"
        f"🔹 Нажми кнопку ниже, чтобы открыть приложение\n"
        f"🔹 Я буду напоминать о каждом приёме\n"
        f"🔹 Отслеживай серии и получай статистику\n\n"
        f"_Используй /help для списка команд_",
        parse_mode="Markdown",
        reply_markup=keyboard,
    )


async def cmd_help(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    """Список доступных команд."""
    await update.message.reply_text(
        "📋 *Команды Pillio:*\n\n"
        "/start — Главное меню + открыть приложение\n"
        "/today — Сводка приёмов на сегодня\n"
        "/pills — Список лекарств\n"
        "/remind — Проверить напоминания\n"
        "/help — Эта справка\n\n"
        "💡 _Напоминания приходят автоматически!_",
        parse_mode="Markdown",
    )


async def cmd_today(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    """Показывает сводку приёмов на сегодня."""
    uid = str(update.effective_user.id) if update.effective_user else ""
    data = api_get(f"/api/schedule?date={today_iso()}", uid=uid)
    if not data:
        await update.message.reply_text("⚠️ Не удалось получить данные с сервера.")
        return

    schedules = data.get("schedules", [])
    progress = data.get("progress", 0)
    pct = int(progress * 100)

    if not schedules:
        await update.message.reply_text(
            "📭 На сегодня лекарств не запланировано.\n"
            "Добавьте их через /start → Открыть Pillio"
        )
        return

    # Строим красивый список
    taken = sum(1 for s in schedules if s.get("taken"))
    total = len(schedules)

    lines = [f"📊 *Прогресс: {pct}%* ({taken}/{total})\n"]

    for s in sorted(schedules, key=lambda x: x.get("scheduled_time", "")):
        time_str = s.get("scheduled_time", "")[11:16]
        name = s.get("pill_name", "?")
        dose = s.get("pill_dosage", "")
        unit = s.get("pill_unit", "")
        is_taken = s.get("taken", False)
        meal = s.get("meal_relation", "none")
        status = "✅" if is_taken else "⬜"
        meal_tag = f" _{MEAL_LABELS.get(meal, '')}_" if meal != "none" else ""
        lines.append(f"{status} {time_str} — *{name}* {dose} {unit}{meal_tag}")

    # Статистика
    stats = api_get("/api/stats", uid=uid)
    if stats:
        streak = stats.get("streak", 0)
        if streak > 0:
            lines.append(f"\n🔥 Серия: *{streak}* дн. подряд!")

    keyboard = InlineKeyboardMarkup([
        [InlineKeyboardButton(
            "💊 Открыть Pillio",
            web_app=WebAppInfo(url=WEBAPP_URL),
        )],
    ])

    await update.message.reply_text(
        "\n".join(lines),
        parse_mode="Markdown",
        reply_markup=keyboard,
    )


async def cmd_pills(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    """Показывает список лекарств."""
    uid = str(update.effective_user.id) if update.effective_user else ""
    pills = api_get("/api/pills", uid=uid)
    if not pills:
        await update.message.reply_text(
            "📭 Лекарств пока нет. Добавьте через приложение!"
        )
        return

    lines = ["💊 *Ваши лекарства:*\n"]
    for p in pills:
        name = p.get("name", "?")
        dose = p.get("dosage", "")
        unit = p.get("unit", "")
        interval = p.get("interval_hours", 0)
        start_h = p.get("start_hour", 0)
        start_m = p.get("start_minute", 0)
        meal = p.get("meal_relation", "none")
        course = p.get("course_days", 0)
        meal_info = f"  {MEAL_LABELS.get(meal, '')}" if meal != "none" else ""
        course_info = f"  📅 Курс: {course} дн." if course > 0 else ""
        lines.append(
            f"• *{name}* — {dose} {unit}\n"
            f"  ⏰ каждые {interval}ч, начало {start_h:02d}:{start_m:02d}"
            f"{meal_info}{course_info}"
        )

    await update.message.reply_text("\n".join(lines), parse_mode="Markdown")


async def cmd_remind(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    """Принудительно проверяет и отправляет напоминания."""
    uid = str(update.effective_user.id) if update.effective_user else ""
    await check_and_send_reminders(ctx.bot, uid=uid)
    await update.message.reply_text("🔔 Проверка напоминаний выполнена!")


async def cmd_family(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    """Показывает семейный код и статус близких."""
    uid = str(update.effective_user.id) if update.effective_user else ""
    name = update.effective_user.first_name if update.effective_user else "Профиль"

    me = api_get("/api/family/me", uid=uid)
    if not me:
        await update.message.reply_text("⚠️ Не удалось получить данные.")
        return

    code = me.get("share_code", "------")
    lines = [
        f"👨‍👩‍👧‍👦 *Моя семья*\n",
        f"🔑 Ваш код: `{code}`",
        f"_Поделитесь кодом с близкими, чтобы они видели ваш прогресс_\n",
    ]

    # Following
    fdata = api_get("/api/family/following", uid=uid)
    following = fdata.get("following", []) if fdata else []
    if following:
        lines.append("👀 *Я слежу за:*")
        for f in following:
            fname = f.get("name", "?")
            rel = f.get("relation", "")
            status = f.get("status", {})
            pct = int((status.get("progress", 0)) * 100)
            taken = status.get("taken", 0)
            total = status.get("total", 0)
            pending = status.get("pending", [])
            emoji = "✅" if pct == 100 else "🟡" if pct > 0 else "⬜"
            pending_txt = ""
            if pending:
                pending_txt = f"\n    ⏰ _Пропущено: {', '.join(pending)}_"
            lines.append(f"{emoji} *{fname}* ({rel}) — {pct}% ({taken}/{total}){pending_txt}")

    await update.message.reply_text("\n".join(lines), parse_mode="Markdown")


# ── Фоновые напоминания ──────────────────────────────────────────

async def check_and_send_reminders(bot, uid: str = ""):
    """
    Проверяет непринятые лекарства и отправляет напоминания.
    Вызывается каждые 60 секунд или по команде /remind.
    """
    try:
        if not uid:
            return

        # Получаем непринятые лекарства
        reminders_data = api_get("/api/bot/reminders", uid=uid)
        if not reminders_data:
            return

        reminders = reminders_data.get("reminders", [])
        if not reminders:
            return

        # Получаем telegram_id и chat_id из bot.json
        bot_data_path = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "..", "data", "bot.json"
        )

        chat_id = None
        try:
            import json
            with open(bot_data_path, "r") as f:
                bot_cfg = json.load(f)
                # bot.json хранит mapping: {uid: chat_id}
                chat_id = bot_cfg.get(uid)
        except Exception:
            return

        if not chat_id:
            return

        now = datetime.now()
        current_time = now.strftime("%H:%M")

        # Фильтруем: напоминаем только о текущем или прошедшем времени
        due = []
        for r in reminders:
            sched_time = r.get("scheduled_time", "")[11:16]
            if sched_time and sched_time <= current_time:
                due.append(r)

        if not due:
            return

        lines = ["🔔 *Напоминание о лекарствах!*\n"]
        for r in due:
            name = r.get("pill_name", "?")
            dose = r.get("pill_dosage", "")
            unit = r.get("pill_unit", "")
            time_str = r.get("scheduled_time", "")[11:16]
            meal = r.get("meal_relation", "none")
            # Smart contextual message based on meal relation
            if meal in MEAL_HINTS:
                hint = MEAL_HINTS[meal]
                lines.append(f"💊 *{name}* {dose} {unit} — {time_str}\n    🍽 _Примите {hint}_")
            else:
                lines.append(f"💊 *{name}* {dose} {unit} — {time_str}")

        lines.append("\n_Откройте приложение, чтобы отметить приём_")

        keyboard = InlineKeyboardMarkup([
            [InlineKeyboardButton(
                "✅ Открыть Pillio",
                web_app=WebAppInfo(url=WEBAPP_URL),
            )],
        ])

        await bot.send_message(
            chat_id=chat_id,
            text="\n".join(lines),
            parse_mode="Markdown",
            reply_markup=keyboard,
        )

    except Exception as e:
        logger.error("Reminder check failed: %s", e)


async def check_family_digests(bot):
    """
    Проверяет дайджесты семейного модуля и уведомляет подписчиков
    о пропущенных лекарствах их близких.
    """
    try:
        # Get all registered users from bot.json
        bot_data_path = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "..", "data", "bot.json"
        )
        try:
            import json
            with open(bot_data_path, "r") as f:
                bot_cfg = json.load(f)
        except Exception:
            return

        # Iterate through all registered users
        for uid, chat_id in bot_cfg.items():
            if uid == "_meta":  # Skip metadata
                continue
            if not chat_id:
                continue

            digest = api_get("/api/family/digest", uid=uid)
            if not digest:
                continue

            overdue = digest.get("overdue", [])
            notify = digest.get("notify", [])

            # Send notifications to followers about overdue meds
            if overdue and notify:
                # Get user's name
                me = api_get("/api/family/me", uid=uid)
                user_name = me.get("name", "Близкий") if me else "Близкий"

                lines = [f"⚠️ *{user_name}* пропускает лекарства:\n"]
                for pill_name in overdue:
                    lines.append(f"  💊 {pill_name}")
                lines.append("\n_Свяжитесь, чтобы напомнить о приёме_")

                keyboard = InlineKeyboardMarkup([
                    [InlineKeyboardButton(
                        "👀 Посмотреть подробнее",
                        web_app=WebAppInfo(url=WEBAPP_URL),
                    )],
                ])

                for follower_id in notify:
                    try:
                        await bot.send_message(
                            chat_id=follower_id,
                            text="\n".join(lines),
                            parse_mode="Markdown",
                            reply_markup=keyboard,
                        )
                    except Exception as e:
                        logger.warning("Failed to notify follower %s: %s", follower_id, e)

    except Exception as e:
        logger.error("Family digest check failed: %s", e)


async def reminder_loop(app: Application):
    """Фоновый цикл проверки напоминаний каждые 60 секунд."""
    while True:
        await asyncio.sleep(60)
        try:
            await check_and_send_reminders(app.bot)
            await check_family_digests(app.bot)
        except Exception as e:
            logger.error("Reminder loop error: %s", e)


# ── Запуск ────────────────────────────────────────────────────────

def main():
    """Точка входа бота."""
    if not BOT_TOKEN or BOT_TOKEN == "ВСТАВЬТЕ_ТОКЕН_БОТА":
        print("❌ Укажите BOT_TOKEN в файле bot/.env")
        print("   Получите токен у @BotFather в Telegram")
        return

    # Создаём приложение
    app = Application.builder().token(BOT_TOKEN).build()

    # Регистрируем команды
    app.add_handler(CommandHandler("start", cmd_start))
    app.add_handler(CommandHandler("help", cmd_help))
    app.add_handler(CommandHandler("today", cmd_today))
    app.add_handler(CommandHandler("pills", cmd_pills))
    app.add_handler(CommandHandler("remind", cmd_remind))
    app.add_handler(CommandHandler("family", cmd_family))

    # Устанавливаем команды в меню бота
    async def post_init(application: Application):
        await application.bot.set_my_commands([
            BotCommand("start", "Главное меню"),
            BotCommand("today", "Приёмы на сегодня"),
            BotCommand("pills", "Список лекарств"),
            BotCommand("remind", "Проверить напоминания"),
            BotCommand("family", "Моя семья"),
            BotCommand("help", "Справка"),
        ])
        # Запускаем фоновый цикл напоминаний
        asyncio.create_task(reminder_loop(application))
        logger.info("✅ Pillio Bot запущен!")
        logger.info("   API: %s", API_URL)
        logger.info("   WebApp: %s", WEBAPP_URL)

    app.post_init = post_init

    # Запуск
    print("🤖 Pillio Bot запускается...")
    app.run_polling(allowed_updates=Update.ALL_TYPES)


if __name__ == "__main__":
    main()
