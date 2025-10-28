#!/bin/bash
echo "=== Testing for Race Conditions ==="
gcc -Wall -Wextra -pthread -g -fsanitize=thread -o dropbox_server dropbox_server.c
./dropbox_server &
SERVER_PID=$!
sleep 2
kill $SERVER_PID
echo "=== Race condition test completed ==="

