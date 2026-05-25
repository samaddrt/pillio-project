"""
Pillio Telegram Bot — бот-компаньон для Mini App трекера лекарств.

Команды:
  /start   — приветствие + кнопка открытия Mini App
  /help    — список команд
  /today   — сводка по приёмам на сегодня
  /pills   — список лекарств
  /family  — семейный код и близкие

Фоновый цикл (каждые 60 секунд):
  • напоминания о приёме — один раз на слот, с учётом связи с едой;
  • семейные дайджесты о пропусках близких;
  • доставка запросов на добавление в семью (по @username);
  • вечерний чек-ин самочувствия.

Все запросы к C++ API изолированы по пользователю заголовком X-Pillio-Uid.
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
    CallbackQueryHandler,
    ContextTypes,
)

# ── Конфигурация ──────────────────────────────────────────────────

load_dotenv()

BOT_TOKEN = os.getenv("BOT_TOKEN", "")
WEBAPP_URL = os.getenv("WEBAPP_URL", "https://your-domain.com")
API_URL = os.getenv("API_URL", "http://localhost:8080")

# Час вечернего чек-ина самочувствия (локальное время сервера)
MOOD_CHECKIN_HOUR = int(os.getenv("MOOD_CHECKIN_HOUR", "20"))

logging.basicConfig(
    format="%(asctime)s [%(name)s] %(levelname)s: %(message)s",
    level=logging.INFO,
)
logger = logging.getLogger("pillio_bot")

# Дедупликация: какие напоминания/чек-ины уже отправлены сегодня.
_sent_reminders: set[str] = set()
_mood_checkin_sent: set[str] = set()
_state_day: str = ""


def _reset_daily_state() -> None:
    """Сбрасывает дневные множества при смене даты."""
    global _state_day
    today = datetime.now().strftime("%Y-%m-%d")
    if today != _state_day:
        _state_day = today
        _sent_reminders.clear()
        _mood_checkin_sent.clear()


# ── HTTP-обёртки ──────────────────────────────────────────────────

def api_get(path: str, uid: str = ""):
    """GET-запрос к C++ API с X-Pillio-Uid header."""
    try:
        headers = {"X-Pillio-Uid": uid} if uid else {}
        r = requests.get(f"{API_URL}{path}", headers=headers, timeout=5)
        r.raise_for_status()
        return r.json()
    except Exception as e:
        logger.error("API GET %s (uid=%s) failed: %s", path, uid, e)
        return None


def api_post(path: str, data: dict, uid: str = ""):
    """POST-запрос к C++ API с X-Pillio-Uid header."""
    try:
        headers = {"X-Pillio-Uid": uid} if uid else {}
        r = requests.post(f"{API_URL}{path}", json=data, headers=headers, timeout=5)
        r.raise_for_status()
        return r.json()
    except Exception as e:
        logger.error("API POST %s (uid=%s) failed: %s", path, uid, e)
        return None


def today_iso() -> str:
    return datetime.now().strftime("%Y-%m-%d")


def register_user(user, chat_id) -> None:
    """Регистрирует chat_id (для напоминаний) и username (для поиска в семье)."""
    uid = str(user.id)
    api_post("/api/bot/register", {"telegram_id": uid, "chat_id": chat_id}, uid=uid)
    # Создаём/обновляем семейный профиль с username — чтобы по нему находили.
    name = user.first_name or "Пользователь"
    uname = user.username or ""
    api_get(
        f"/api/family/me?uid={uid}"
        f"&name={requests.utils.quote(name)}"
        f"&username={requests.utils.quote(uname)}",
        uid=uid,
    )


MEAL_LABELS = {
    "before": "🍽 До еды",
    "during": "🍽 Во время еды",
    "after": "🍽 После еды",
}

MEAL_HINTS = {
    "before": "примите за 30 минут до еды",
    "during": "примите во время еды",
    "after": "примите сразу после еды",
}


def mood_keyboard() -> InlineKeyboardMarkup:
    return InlineKeyboardMarkup([[
        InlineKeyboardButton("😣", callback_data="mood:1"),
        InlineKeyboardButton("😕", callback_data="mood:2"),
        InlineKeyboardButton("😐", callback_data="mood:3"),
        InlineKeyboardButton("🙂", callback_data="mood:4"),
        InlineKeyboardButton("😄", callback_data="mood:5"),
    ]])


def open_app_keyboard(label: str = "💊 Открыть Pillio") -> InlineKeyboardMarkup:
    return InlineKeyboardMarkup([[
        InlineKeyboardButton(label, web_app=WebAppInfo(url=WEBAPP_URL)),
    ]])


# ── Команды ───────────────────────────────────────────────────────

async def cmd_start(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    user = update.effective_user
    name = user.first_name if user else "друг"
    register_user(user, update.effective_chat.id)

    keyboard = InlineKeyboardMarkup([
        [InlineKeyboardButton("💊 Открыть Pillio", web_app=WebAppInfo(url=WEBAPP_URL))],
        [InlineKeyboardButton("📊 Мои приёмы сегодня", callback_data="today")],
    ])
    await update.message.reply_text(
        f"Привет, {name}! 👋\n\n"
        f"Я — *Pillio*, твой трекер лекарств 💊\n\n"
        f"🔹 Открывай приложение кнопкой ниже\n"
        f"🔹 Я сам напомню о каждом приёме в нужное время\n"
        f"🔹 Близкие смогут следить за твоим приёмом\n\n"
        f"_Напоминания уже включены. /help — список команд._",
        parse_mode="Markdown",
        reply_markup=keyboard,
    )


async def cmd_help(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    await update.message.reply_text(
        "📋 *Команды Pillio:*\n\n"
        "/start — Главное меню\n"
        "/today — Приёмы на сегодня\n"
        "/pills — Список лекарств\n"
        "/family — Семья и близкие\n"
        "/help — Эта справка\n\n"
        "💡 _Напоминания приходят автоматически в нужное время._",
        parse_mode="Markdown",
    )


async def _send_today(uid: str, send):
    """Формирует и отправляет сводку приёмов на сегодня через callable send(text, kb)."""
    data = api_get(f"/api/schedule?date={today_iso()}", uid=uid)
    if data is None:
        await send("⚠️ Не удалось получить данные с сервера.", None)
        return

    schedules = data.get("schedules", [])
    progress = data.get("progress", 0)
    pct = int(progress * 100)

    if not schedules:
        await send(
            "📭 На сегодня лекарств не запланировано.\n"
            "Добавьте их в приложении → кнопка ниже.",
            open_app_keyboard(),
        )
        return

    taken = sum(1 for s in schedules if s.get("taken"))
    total = len(schedules)
    lines = [f"📊 *Прогресс: {pct}%* ({taken}/{total})\n"]
    for s in sorted(schedules, key=lambda x: x.get("scheduled_time", "")):
        time_str = s.get("scheduled_time", "")[11:16]
        nm = s.get("pill_name", "?")
        dose = s.get("pill_dosage", "")
        unit = s.get("pill_unit", "")
        meal = s.get("meal_relation", "none")
        status = "✅" if s.get("taken") else "⬜"
        meal_tag = f" _{MEAL_LABELS.get(meal, '')}_" if meal in MEAL_LABELS else ""
        lines.append(f"{status} {time_str} — *{nm}* {dose} {unit}{meal_tag}")

    stats = api_get("/api/stats", uid=uid)
    if stats and stats.get("streak", 0) > 0:
        lines.append(f"\n🔥 Серия: *{stats['streak']}* дн. подряд!")

    await send("\n".join(lines), open_app_keyboard())


async def cmd_today(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    uid = str(update.effective_user.id)

    async def send(text, kb):
        await update.message.reply_text(text, parse_mode="Markdown", reply_markup=kb)

    await _send_today(uid, send)


async def cmd_pills(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    uid = str(update.effective_user.id)
    pills = api_get("/api/pills", uid=uid)
    if not pills:
        await update.message.reply_text(
            "📭 Лекарств пока нет. Добавьте их в приложении!",
            reply_markup=open_app_keyboard(),
        )
        return

    lines = ["💊 *Ваши лекарства:*\n"]
    for p in pills:
        nm = p.get("name", "?")
        dose = p.get("dosage", "")
        unit = p.get("unit", "")
        interval = p.get("interval_hours", 0)
        sh = p.get("start_hour", 0)
        sm = p.get("start_minute", 0)
        meal = p.get("meal_relation", "none")
        course = p.get("course_days", 0)
        meal_info = f"  {MEAL_LABELS.get(meal, '')}" if meal in MEAL_LABELS else ""
        course_info = f"  📅 Курс: {course} дн." if course > 0 else ""
        lines.append(
            f"• *{nm}* — {dose} {unit}\n"
            f"  ⏰ каждые {interval}ч, начало {sh:02d}:{sm:02d}{meal_info}{course_info}"
        )
    await update.message.reply_text("\n".join(lines), parse_mode="Markdown")


async def cmd_family(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    user = update.effective_user
    uid = str(user.id)
    register_user(user, update.effective_chat.id)

    me = api_get(f"/api/family/me?uid={uid}", uid=uid)
    if not me:
        await update.message.reply_text("⚠️ Не удалось получить данные.")
        return

    code = me.get("share_code", "------")
    lines = [
        "👨‍👩‍👧‍👦 *Моя семья*\n",
        f"🔑 Ваш код: `{code}`",
        "_Поделитесь кодом или попросите добавить вас по @username._\n",
    ]

    members = api_get(f"/api/family/members?uid={uid}", uid=uid)
    mlist = (members or {}).get("members", []) if isinstance(members, dict) else []
    if mlist:
        lines.append("👀 *Близкие:*")
        for m in mlist:
            nm = m.get("name", "?")
            rel = m.get("relation", "")
            status = m.get("status", {})
            pct = int((status.get("progress", 0)) * 100)
            taken = status.get("taken", 0)
            total = status.get("total", 0)
            emoji = "✅" if pct == 100 and total else "🟡" if pct > 0 else "⬜"
            rel_txt = f" ({rel})" if rel else ""
            lines.append(f"{emoji} *{nm}*{rel_txt} — {pct}% ({taken}/{total})")

    await update.message.reply_text(
        "\n".join(lines), parse_mode="Markdown", reply_markup=open_app_keyboard()
    )


# ── Обработчики inline-кнопок ─────────────────────────────────────

async def on_callback(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    query = update.callback_query
    data = query.data or ""
    user = query.from_user
    uid = str(user.id)

    if data == "today":
        await query.answer()

        async def send(text, kb):
            await query.message.reply_text(text, parse_mode="Markdown", reply_markup=kb)

        await _send_today(uid, send)
        return

    if data.startswith("mood:"):
        mood = data.split(":", 1)[1]
        api_post("/api/mood", {"date": today_iso(), "mood": int(mood)}, uid=uid)
        await query.answer("Записал! Спасибо 🙏")
        try:
            await query.edit_message_text(
                "Спасибо! Самочувствие за сегодня записано 🙏\n"
                "_Динамику видно в приложении → Статистика._",
                parse_mode="Markdown",
            )
        except Exception:
            pass
        return

    if data.startswith("famreq:"):
        _, action, req_id = data.split(":", 2)
        if action == "accept":
            resp = api_post("/api/family/request/accept",
                            {"request_id": req_id, "name": user.first_name or ""}, uid=uid)
            await query.answer("Принято!")
            try:
                await query.edit_message_text("✅ Вы приняли запрос. Теперь близкий видит ваш приём лекарств.")
            except Exception:
                pass
            # Уведомляем отправителя запроса
            if resp and resp.get("from"):
                try:
                    await ctx.bot.send_message(
                        chat_id=resp["from"],
                        text=f"✅ *{user.first_name or 'Пользователь'}* принял(а) ваш запрос! "
                             f"Теперь вы видите приём лекарств в разделе «Семья».",
                        parse_mode="Markdown",
                        reply_markup=open_app_keyboard("👀 Открыть семью"),
                    )
                except Exception as e:
                    logger.warning("notify requester failed: %s", e)
        else:
            resp = api_post("/api/family/request/decline", {"request_id": req_id}, uid=uid)
            await query.answer("Отклонено")
            try:
                await query.edit_message_text("❌ Запрос отклонён.")
            except Exception:
                pass
            if resp and resp.get("from"):
                try:
                    await ctx.bot.send_message(
                        chat_id=resp["from"],
                        text=f"❌ *{user.first_name or 'Пользователь'}* отклонил(а) ваш запрос на добавление в семью.",
                        parse_mode="Markdown",
                    )
                except Exception:
                    pass
        return


# ── Фоновые задачи ────────────────────────────────────────────────

async def check_reminders(bot, uid: str, chat_id) -> None:
    """Шлёт напоминание один раз на слот, когда подошло время приёма."""
    data = api_get("/api/bot/reminders", uid=uid)
    if not data:
        return
    reminders = data.get("reminders", [])
    if not reminders:
        return

    now = datetime.now()
    for r in reminders:
        st_iso = r.get("scheduled_time", "")
        time_str = st_iso[11:16]
        if not time_str:
            continue
        try:
            slot = datetime.strptime(st_iso[:16], "%Y-%m-%dT%H:%M")
        except ValueError:
            continue
        delta_min = (now - slot).total_seconds() / 60.0
        # Напоминаем в окне 0..90 минут после времени приёма, один раз.
        if not (0 <= delta_min <= 90):
            continue
        key = f"{uid}:{st_iso}"
        if key in _sent_reminders:
            continue
        _sent_reminders.add(key)

        nm = r.get("pill_name", "?")
        dose = r.get("pill_dosage", "")
        unit = r.get("pill_unit", "")
        meal = r.get("meal_relation", "none")
        hint = MEAL_HINTS.get(meal)
        text = f"🔔 *Пора принять лекарство!*\n\n💊 *{nm}* {dose} {unit} — {time_str}"
        if hint:
            text += f"\n🍽 _{hint.capitalize()}_"
        text += "\n\n_Отметьте приём в приложении._"
        try:
            await bot.send_message(chat_id=chat_id, text=text, parse_mode="Markdown",
                                   reply_markup=open_app_keyboard("✅ Открыть Pillio"))
        except Exception as e:
            logger.warning("send reminder failed for %s: %s", uid, e)


async def check_family_digests(bot, uid: str) -> None:
    """Уведомляет близких о пропущенных приёмах пользователя uid."""
    digest = api_get("/api/family/digest", uid=uid)
    if not digest:
        return
    overdue = digest.get("overdue", [])
    notify = digest.get("notify", [])
    if not overdue or not notify:
        return

    user_name = digest.get("profile", {}).get("name", "Близкий")
    lines = [f"⚠️ *{user_name}* пропускает лекарства:\n"]
    for item in overdue:
        nm = item.get("pill_name", "?") if isinstance(item, dict) else str(item)
        tm = item.get("scheduled_time", "")[11:16] if isinstance(item, dict) else ""
        lines.append(f"  💊 {nm}{(' — ' + tm) if tm else ''}")
    lines.append("\n_Свяжитесь, чтобы напомнить о приёме._")
    text = "\n".join(lines)

    for follower in notify:
        fid = follower.get("chat_id") if isinstance(follower, dict) else follower
        if not fid:
            continue
        # Один дайджест на близкого в день на каждый набор пропусков.
        key = f"digest:{uid}:{fid}:{today_iso()}:{len(overdue)}"
        if key in _sent_reminders:
            continue
        _sent_reminders.add(key)
        try:
            await bot.send_message(chat_id=fid, text=text, parse_mode="Markdown",
                                   reply_markup=open_app_keyboard("👀 Подробнее"))
        except Exception as e:
            logger.warning("notify follower failed: %s", e)


async def deliver_family_requests(bot) -> None:
    """Доставляет запросы на добавление в семью получателям с кнопками."""
    outbox = api_get("/api/family/requests/outbox")
    if not outbox:
        return
    for r in outbox.get("requests", []):
        to = r.get("to")
        req_id = r.get("id")
        if not to or not req_id:
            continue
        from_name = r.get("from_name", "Пользователь")
        relation = r.get("relation", "близкий")
        kb = InlineKeyboardMarkup([[
            InlineKeyboardButton("✅ Принять", callback_data=f"famreq:accept:{req_id}"),
            InlineKeyboardButton("❌ Отклонить", callback_data=f"famreq:decline:{req_id}"),
        ]])
        try:
            await bot.send_message(
                chat_id=to,
                text=f"👨‍👩‍👧 *{from_name}* хочет следить за вашим приёмом лекарств "
                     f"как «{relation}».\n\nРазрешить?",
                parse_mode="Markdown",
                reply_markup=kb,
            )
            api_post("/api/family/request/notified", {"request_id": req_id})
        except Exception as e:
            # Если не удалось доставить (бот не запущен у получателя) — тоже гасим,
            # чтобы не зацикливаться; запрос останется в семье как ожидающий в приложении.
            logger.warning("deliver request to %s failed: %s", to, e)
            api_post("/api/family/request/notified", {"request_id": req_id})


async def mood_checkin(bot, uid: str, chat_id) -> None:
    """Раз в день (после MOOD_CHECKIN_HOUR) спрашивает самочувствие."""
    if datetime.now().hour < MOOD_CHECKIN_HOUR:
        return
    key = f"{uid}:{today_iso()}"
    if key in _mood_checkin_sent:
        return
    # Спрашиваем только если человек сегодня пользовался лекарствами.
    sched = api_get(f"/api/schedule?date={today_iso()}", uid=uid)
    if not sched or not sched.get("schedules"):
        _mood_checkin_sent.add(key)
        return
    mood = api_get(f"/api/mood?date={today_iso()}", uid=uid)
    if mood and mood.get("mood", 0) > 0:
        _mood_checkin_sent.add(key)  # уже отметил в приложении
        return
    _mood_checkin_sent.add(key)
    try:
        await bot.send_message(
            chat_id=chat_id,
            text="🌙 Как ваше самочувствие сегодня?\nЭто поможет увидеть, как лекарства влияют на состояние.",
            reply_markup=mood_keyboard(),
        )
    except Exception as e:
        logger.warning("mood checkin failed for %s: %s", uid, e)


async def background_loop(app: Application):
    """Главный фоновый цикл: напоминания, дайджесты, запросы, чек-ины."""
    while True:
        await asyncio.sleep(60)
        try:
            _reset_daily_state()
            users = (api_get("/api/bot/users") or {}).get("users", {})
            for uid, chat_id in users.items():
                if not chat_id:
                    continue
                await check_reminders(app.bot, uid, chat_id)
                await check_family_digests(app.bot, uid)
                await mood_checkin(app.bot, uid, chat_id)
            await deliver_family_requests(app.bot)
        except Exception as e:
            logger.error("background loop error: %s", e)


# ── Запуск ────────────────────────────────────────────────────────

def main():
    if not BOT_TOKEN or BOT_TOKEN == "ВСТАВЬТЕ_ТОКЕН_БОТА":
        print("❌ Укажите BOT_TOKEN в bot/.env или переменных окружения")
        return

    app = Application.builder().token(BOT_TOKEN).build()

    app.add_handler(CommandHandler("start", cmd_start))
    app.add_handler(CommandHandler("help", cmd_help))
    app.add_handler(CommandHandler("today", cmd_today))
    app.add_handler(CommandHandler("pills", cmd_pills))
    app.add_handler(CommandHandler("family", cmd_family))
    app.add_handler(CallbackQueryHandler(on_callback))

    async def post_init(application: Application):
        await application.bot.set_my_commands([
            BotCommand("start", "Главное меню"),
            BotCommand("today", "Приёмы на сегодня"),
            BotCommand("pills", "Список лекарств"),
            BotCommand("family", "Моя семья"),
            BotCommand("help", "Справка"),
        ])
        asyncio.create_task(background_loop(application))
        logger.info("✅ Pillio Bot запущен!")
        logger.info("   API: %s", API_URL)
        logger.info("   WebApp: %s", WEBAPP_URL)

    app.post_init = post_init

    print("🤖 Pillio Bot запускается...")
    app.run_polling(allowed_updates=Update.ALL_TYPES)


if __name__ == "__main__":
    main()
