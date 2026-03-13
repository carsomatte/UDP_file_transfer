# UDP File Transfer 
A simple client-server file transfer application using UDP with a stop-and-wait reliability protocol

## Features 
- Commands and files are broken into 1400-byte chunks and sent one at a time
- Each chunk must be acknowledged before the next is sent (stop-and-wait)
- Unacknowledged chunks are retried up to 5 times before aborting
- The client validates commands locally before sending them to the server

## Commands
| Command                   | Description |
|---------------------------|-------------|
|```get <filename> ``` | Download a file from the server|
|```put <filename>``` | Upload a file to the server|
|```delete <filename>``` | Delete a file on the server|
|```ls``` | List files in server's working directory|
|```exit``` | Shut down the server and exit|
|```help``` | Show available commands|





## Building and Usage
Compile server and client using the provided Makefile

Start the server: ```./udp_server <port>```

Start the client: ```./udp_client <ip_address> <port>```
