OS Lab 7-8: Simple Dropbox

Group Members
bscs23207 Faseeha Noor 
bscs23119 Qudsia Khan
Compile server
gcc -pthread -o dropbox_server dropbox_server.c

Compile client  
gcc -pthread -o dropbox_client dropbox_client.c

Run server
./dropbox_server

Run client
./dropbox_client 127.0.0.1 8080
