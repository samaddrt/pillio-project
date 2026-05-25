#!/bin/bash
set -e

# Kill old processes if any
pkill -f './build/pillio' 2>/dev/null || true
pkill -f 'bot/bot.py' 2>/dev/null || true
sleep 1

# Build C++ server
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1
make -j$(nproc) 2>&1
cd ..

# Install Python deps
pip install -q -r bot/requirements.txt 2>/dev/null

# Start both processes
./build/pillio &
SERVER_PID=$!

python3 bot/bot.py &
BOT_PID=$!

echo "Server PID: $SERVER_PID"
echo "Bot PID: $BOT_PID"

# Wait for both
wait $SERVER_PID $BOT_PID
