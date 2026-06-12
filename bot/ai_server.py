"""Мини HTTP-сервис AI-помощника Pillio (127.0.0.1:8090).

C++-сервер проксирует сюда POST /api/ai/ask → /ask. Python выполняет
HTTPS-запрос к Google Gemini (бесплатный тариф) — ключ берётся из
переменной окружения GEMINI_API_KEY и никогда не попадает в клиент.

Если ключ не задан или сеть недоступна — сервис отвечает понятной
ошибкой, не ломая остальное приложение.
"""

from __future__ import annotations

import json
import logging
import os
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import requests

log = logging.getLogger("pillio.ai")

GEMINI_KEY = os.getenv("GEMINI_API_KEY", "")
MODEL = os.getenv("GEMINI_MODEL", "gemini-2.0-flash")
SYSTEM = (
    "Ты — встроенный помощник приложения Pillio для отслеживания приёма лекарств. "
    "Отвечай по-русски, кратко (3–6 предложений), дружелюбно и по делу. "
    "Не ставь диагнозов и не назначай лечение; при серьёзных симптомах или "
    "сомнениях всегда советуй обратиться к врачу или фармацевту."
)


def ask_gemini(question: str, context: str) -> str:
    """Отправляет вопрос в Gemini и возвращает текст ответа."""
    url = (f"https://generativelanguage.googleapis.com/v1beta/models/"
           f"{MODEL}:generateContent?key={GEMINI_KEY}")
    prompt = (f"{SYSTEM}\n\n"
              f"Лекарства пользователя: {context or 'не указаны'}\n\n"
              f"Вопрос пользователя: {question}")
    r = requests.post(url, json={"contents": [{"parts": [{"text": prompt}]}]},
                      timeout=25)
    r.raise_for_status()
    return r.json()["candidates"][0]["content"]["parts"][0]["text"].strip()


class _Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):  # глушим стандартный лог http.server
        pass

    def do_POST(self):  # noqa: N802 - имя диктует BaseHTTPRequestHandler
        if self.path != "/ask":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", 0) or 0)
            body = json.loads(self.rfile.read(length) or b"{}")
            question = (body.get("question") or "").strip()
            if not question:
                out, code = {"error": "Пустой вопрос"}, 422
            elif not GEMINI_KEY:
                out, code = {"error": "AI не настроен: добавьте GEMINI_API_KEY"}, 503
            else:
                answer = ask_gemini(question[:2000], (body.get("context") or "")[:2000])
                out, code = {"answer": answer}, 200
        except Exception as e:  # noqa: BLE001 - любую ошибку превращаем в ответ
            log.warning("ai error: %s", e)
            out, code = {"error": "AI временно недоступен"}, 502
        data = json.dumps(out, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def start_ai_server(port: int = 8090) -> None:
    """Запускает AI-сервис в фоновом daemon-потоке."""
    srv = ThreadingHTTPServer(("127.0.0.1", port), _Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    log.info("AI-сервис запущен на 127.0.0.1:%d (ключ %s)",
             port, "задан" if GEMINI_KEY else "НЕ задан")


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s [ai] %(levelname)s: %(message)s")
    start_ai_server()
    threading.Event().wait()  # держим процесс живым при ручном запуске
