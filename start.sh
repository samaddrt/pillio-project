#!/bin/bash
set -e

# Kill old processes if any
pkill -f './build/pillio' 2>/dev/null || true
pkill -f 'bot/bot.py' 2>/dev/null || true
sleep 1

# Build C++ server (direct g++ — no cmake needed)
mkdir -p build
echo "Building pillio..."
g++ -std=c++20 -O2 -I src -I third_party \
    src/main.cpp src/tracker.cpp src/storage.cpp \
    src/analytics.cpp src/checker.cpp src/family.cpp \
    -o build/pillio -lpthread 2>&1
echo "Build OK"

# Install Python deps
pip install -q -r bot/requirements.txt 2>/dev/null || true

# Start both processes
./build/pillio &
SERVER_PID=$!

python3 bot/bot.py &
BOT_PID=$!

echo "Server PID: $SERVER_PID"
echo "Bot PID: $BOT_PID"

# Wait for both
wait $SERVER_PID $BOT_PID
