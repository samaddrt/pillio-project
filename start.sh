#!/bin/bash
set -e

# Останавливаем старые процессы, если они остались.
pkill -f './build/pillio' 2>/dev/null || true
pkill -f 'bot/bot.py' 2>/dev/null || true
sleep 1

# Собираем C++ сервер напрямую через g++. Это удобно для быстрого запуска в Replit.
mkdir -p build data
echo "Building pillio..."
g++ -std=c++20 -O2 -I src -I third_party \
    src/main.cpp src/tracker.cpp src/storage.cpp \
    src/family.cpp \
    -o build/pillio -lpthread 2>&1
echo "Build OK"

# Устанавливаем Python-зависимости в локальную папку, которая не попадает в git.
pip install --target ./pylibs -q -r bot/requirements.txt 2>/dev/null || true

# Перед стартом восстанавливаем снимок базы из Replit Object Storage, если он есть.
PYTHONPATH=./pylibs python3 bot/persist.py restore || true

# Запускаем C++ сервер и Telegram-бота.
STORAGE_PATH=./data/store.json \
FAMILY_STORAGE_PATH=./data/family.json \
API_PORT=8080 \
./build/pillio &
SERVER_PID=$!

PYTHONPATH=./pylibs python3 bot/bot.py &
BOT_PID=$!

echo "Server PID: $SERVER_PID"
echo "Bot PID: $BOT_PID"

# Ждём завершения обоих процессов.
wait $SERVER_PID $BOT_PID
