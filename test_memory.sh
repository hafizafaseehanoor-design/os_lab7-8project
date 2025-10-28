#!/bin/bash
echo "=== Testing for Memory Leaks ==="
gcc -Wall -Wextra -pthread -g -o dropbox_server dropbox_server.c
valgrind --leak-check=full ./dropbox_server &
SERVER_PID=$!
sleep 2
kill $SERVER_PID
echo "=== Memory test completed ==="

