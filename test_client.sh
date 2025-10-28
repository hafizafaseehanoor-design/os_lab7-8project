#!/bin/bash

echo "=== Testing Dropbox Client ==="

echo "Test: Complete workflow in single session"
{
    echo "LOGIN hello hello1234"
    sleep 1
    echo "UPLOAD test.txt"
    sleep 2
    echo "LIST"
    sleep 1
    echo "DOWNLOAD test.txt"
    sleep 2
    echo "DELETE test.txt"
    sleep 1
    echo "LIST"
    sleep 1
    echo "QUIT"
} | nc -q 1 localhost 8080

echo "=== Test completed ==="
