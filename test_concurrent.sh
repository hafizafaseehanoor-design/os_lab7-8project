#!/bin/bash
echo "=== Testing Multiple Clients ==="
./dropbox_server &
SERVER_PID=$!
sleep 2
echo "Starting multiple clients..."
{
    echo "LOGIN hello hello1234"
    sleep 1
    echo "LIST"
    sleep 1
    echo "QUIT"
} | nc -q 1 localhost 8080 &
sleep 3
kill $SERVER_PID
echo "=== Concurrent test completed ==="
