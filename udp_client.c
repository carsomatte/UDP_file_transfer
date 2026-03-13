#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <inttypes.h>

# define BUFFER_SIZE 1024
# define MAX_FILENAME_LEN 255
# define CHUNK_DATA_SIZE 1400
# define TIMEOUT_MSEC 20

typedef struct {
    int sockfd;
    struct sockaddr_in server_addr;
    socklen_t addr_len;
} UDPContext;

typedef struct {
    uint32_t seq; //seq num of this chunk
    uint32_t total; //total number of chunks
    uint16_t data_len; //chunk size
	uint8_t is_error; //1 if this chunk contains error message, 0 otherwise
    char     data[CHUNK_DATA_SIZE]; //chunk data
} Chunk;



void close_socket_helper(int fd) {
	if (close(fd) == -1) {
		perror("Closing Socket Failed");
		exit(EXIT_FAILURE);
	}
	printf("Closed Sockets\n");
}

/* Receive file contents in a buffer and saves to file in working directory */
void save_file(char *filename, char *buf, size_t buf_len) {
    FILE *file = fopen(filename, "wb");
    if (!file) {
        perror("fopen");
        return;
    }

    fwrite(buf, 1, buf_len, file);
    fclose(file);
	printf("File '%s' saved in client's current working directory\n", filename);
}


/*
 *******************************
 Graceful Shutdown Handler
 *******************************
 */
volatile sig_atomic_t stop = 0;

void handle_sigint(int sig) {
	stop = 1;
}

/*
 *******************************
 SOCKET TIMEOUTS HELPERS
 
 we only want the socket to have a timeout during the send/ack part of our program
 otherwise, 'recvfrom' in main will constantly be timing out
 *******************************
 */
void init_socket_timeout(UDPContext *ctx) {
	struct timeval tv = { .tv_sec = 0, .tv_usec = TIMEOUT_MSEC * 1000 };
    if (setsockopt(ctx->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
        perror("Failed to set socket timeout");
        exit(EXIT_FAILURE);
    }
}

void clear_socket_timeout(UDPContext *ctx) {
    struct timeval tv = {0};
    setsockopt(ctx->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/*
 ********************************
 STOP AND WAIT/SENDING LOGIC
 ********************************
 */ 

/* Send a single chunk and wait for an ACK, retrying on timeout */
int chunk_send(UDPContext *ctx, Chunk *chunk) {
	int retries = 0;

	while (1) {
		//printf("sending chunk with seq: %u\n", chunk->seq);
		sendto(ctx->sockfd, chunk, sizeof(Chunk), 0, (struct sockaddr *)&ctx->server_addr, ctx->addr_len);

		uint32_t ack;
		ssize_t n = recvfrom(ctx->sockfd, &ack, sizeof(ack), 0, NULL, NULL);
		//printf("received ack: %u\n", ack);

		if (n < 0) {
			printf("timeout on chunk %u, resending...\n", chunk->seq);
			retries++;
			if (retries > 5) {
				printf("could not connect to server after 5 retries, aborting...\n");
				return -1;
			}
		}
		else if (ack == chunk->seq) {
			return 0;
		}
	}
}

/* Receives a single chunk and sends an ACK back */
int chunk_recv(UDPContext *ctx, Chunk *chunk) {
	ssize_t n = recvfrom(ctx->sockfd, chunk, sizeof(Chunk), 0, NULL, NULL);
    if (n < 0) {
        return -1;
	}

    uint32_t ack = chunk->seq;
    sendto(ctx->sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&ctx->server_addr, ctx->addr_len);
	//printf("Client -> sending ack: %u\n", ack);
    return 0;
}

/* 
Receives a buffer and breaks it into chunks to send 
if 'is_error' is 1, the chunk's 'is_error' field will be set to 1, otherwise it will be set to 0
*/
int send_buffer(UDPContext *ctx, char *buf, uint8_t is_error) {
	size_t total_len = strlen(buf);
	uint32_t total_chunks = (total_len + CHUNK_DATA_SIZE -1) / CHUNK_DATA_SIZE;

	Chunk chunk = {0};
	uint32_t seq = 0;
	uint32_t offset = 0;

	init_socket_timeout(ctx);

	while (offset < total_len) {
		size_t to_copy = total_len - offset;
		if (to_copy > CHUNK_DATA_SIZE) {
            to_copy = CHUNK_DATA_SIZE;
		}

		chunk.seq = seq++;
		chunk.total = total_chunks;
		chunk.data_len = to_copy;
		chunk.is_error = is_error;
		memcpy(chunk.data, buf + offset, to_copy);
		offset += to_copy;

		
		if (chunk_send(ctx, &chunk) == -1) {
			return -1;
		}
	}
	clear_socket_timeout(ctx);
	return 0;
}



/*
**************
PUT

receive a filename and read the file into chunks 
and call 'chunk_send' on each one
************** 
*/
int put_helper(char *filename, UDPContext *ctx) {
	init_socket_timeout(ctx);
	
	FILE *file = fopen(filename, "rb");
	if (!file) { // should never happen since we check for file existence in 'validate_command' before even sending the command to the server
        perror("fopen");
        return -1;
    }

	fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);
    uint32_t total = (file_size + CHUNK_DATA_SIZE - 1) / CHUNK_DATA_SIZE;

    Chunk chunk;
    uint32_t seq = 0;
    size_t bytes_read;

	 while ((bytes_read = fread(chunk.data, 1, CHUNK_DATA_SIZE, file)) > 0) {
        chunk.seq      = seq++;
        chunk.total    = total;
        chunk.data_len = bytes_read;
		chunk.is_error = 0;
        if (chunk_send(ctx, &chunk) == -1) {
			fclose(file);
			return -1;
		}
    }

    fclose(file);
	//printf("Finished sending file '%s'\n", filename);

	clear_socket_timeout(ctx);
	return 0;
}

/* 
**************
Validate User Input

'char *command' is for returning the command string to main
'char *fname' is for returning the filename to main
**************
*/
int validate_command(const char *buffer, char *command, char *fname) {
    if (buffer == NULL) {
        return -1;
    }

    char cmd[16];
    char filename[MAX_FILENAME_LEN];

    // Clear buffers
    memset(cmd, 0, sizeof(cmd));
    memset(filename, 0, sizeof(filename));

    // Try parsing two arguments
    int count = sscanf(buffer, "%15s %254s", cmd, filename);

	strcpy(command, cmd);
    
    // Help Command
    if (strcmp(cmd, "help") == 0) {
		printf("get [file_name]    - the server will transmits the requested file\n");
    	printf("put [file_name]    - the server will receive the transmitted file and store it locally\n");
		printf("delete [file_name] - the server deletes the file if it exists\n");
		printf("ls                 - the server will search all the files it has in its current working directory and send a list of all these files\n");
		printf("exit               - the server will terminate\n");
		return -1; // return -1 to indicate that this is not a valid command to send to the server, it's just a client side helper command
    }
    
    // Commands with NO arguments
    if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "exit") == 0) {
        if (count == 1) {
            return 0;
        }
        return -1;  // Extra arguments
    }

    // Commands that REQUIRE filename
    if (strcmp(cmd, "get") == 0 ||
        strcmp(cmd, "put") == 0 ||
        strcmp(cmd, "delete") == 0) {

        if (count != 2) {
            return -1;  // Missing filename
        }

		strcpy(fname, filename);
		return 0;
    }

    return -1;  // Unknown command
}

int main(int argc, char* argv[]) {
	char *ip_addr;
	int port;
	int client_fd;
	socklen_t server_addr_len;
	struct sockaddr_in server_addr;
	char buf[BUFFER_SIZE];
	char command[15];
	char fname[MAX_FILENAME_LEN];


	if (argc != 3) {
		printf("Usage: ./udp_client <ip_address> <port_number>");
		return 1;
	}
	ip_addr = argv[1];
	port = atoi(argv[2]);


	// Setup Signal Handler for graceful exit when pressing Ctrl+C
	struct sigaction sa = {0};
	sa.sa_handler = handle_sigint;
	sigaction(SIGINT, &sa, NULL);
	
	// Create Socket
	if ((client_fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
		perror("Socket Failed");
		exit(EXIT_FAILURE);
	}

	// Build Server's Internet Address
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port);
	if (inet_pton(AF_INET, ip_addr, &server_addr.sin_addr) <= 0) {
		perror("Invalid Address");
		exit(EXIT_FAILURE);
	}

	server_addr_len = sizeof(server_addr);
	
	while (!stop) {		
		if (errno == EINTR && stop) { // Ctrl+C triggered shutdown
			break;
		}
		
		// Clear Buffers
		memset(buf, 0, sizeof(buf));
		memset(command, 0, sizeof(command));
		memset(fname, 0, sizeof(fname));

		// Prompt user for command
		printf("Enter a valid command: ");
		fgets(buf, BUFFER_SIZE, stdin);

		// Verify user input
		if (validate_command(buf, command, fname) == -1) {
			if (strcmp(command, "help") != 0) {
				printf("Invalid command. Type 'help' for list of commands.\n");
			}
			continue;
		}

		// If Command is 'put', lets make sure the file exists before we even send the command to the server
		if (strcmp(command, "put") == 0) {
			if (access(fname, F_OK | R_OK) != 0) {
				printf("File '%s' does not exist or is not readable in client's working directory\n", fname);
				continue;
			}
		}

		// Content needed for sending and receiving
        UDPContext ctx = {0};
        ctx.sockfd = client_fd;
        ctx.server_addr = server_addr;
        ctx.addr_len = server_addr_len;

		//printf("sending '%s' to %s:%d\n", buf, ip_addr, port);

		// Send the message to the server
		if (send_buffer(&ctx, buf, 0) == -1) {
			continue;
		}

		// For an exit command, we should close our socket and exit
		if (strcmp(command, "exit") == 0) {
			close_socket_helper(client_fd);
			printf("Exiting...\n");

			return 0;
		}

		// For a 'put' command, we are sending a file to the server before receiving any response back
		if (strcmp(command, "put") == 0) {
			if (put_helper(fname, &ctx) == -1) {
				continue;
			}
		}
		
		/* 
		Here we will receive all chunks and reassemble them in an allocated buffer called 'result'
		PUT -> we will receive a status message from the server about whether the put was successful or not (likely 1 chunk)
		GET -> we will receive the file requested from the server in chunks and reassemble it
		LS -> we will receive a list of files in the server's current working directory in chunks and reassemble it
		DELETE -> we will receive a status message from the server about whether the delete was successful or not (likely 1 chunk)
		*/
		Chunk chunk = {0};
		uint32_t received = 0;
		uint32_t total = UINT32_MAX; // total is unknown until first chunk arrives
		char *result = NULL;
	
		while (received < total) {
			if (chunk_recv(&ctx, &chunk) == -1) {
				printf("Error with connecting to server. Chunks are not being received properly...\n");
				printf("Exiting...\n");
				close_socket_helper(client_fd);
				return -1;
			}
			total = chunk.total;	

			// discard already received chunks
			if (chunk.seq != received) {
				printf("Received out of order chunk with seq %u, expected seq %u. Discarding chunk...\n", chunk.seq, received);
				continue;
			}
		
			if (result == NULL) { //allocate once we know size
				result = calloc(total * CHUNK_DATA_SIZE + 1, 1);
				if (!result) {
					perror("calloc");
					close_socket_helper(client_fd);
					return -1;
				}
			}
			
			// copy chunk data into buffer at correct position
			memcpy(result + (chunk.seq * CHUNK_DATA_SIZE), chunk.data, chunk.data_len);
			// TODO: add bounds checking
			received++;
		}


		// Check if the chunks we received are marked as an error message from the server
		if (chunk.is_error) {
			printf("Error from server: %s\n", result);
			free(result);
			continue;
		}


		/* GET
		we've recieved the file from the sever in chunks and reassmebled it to a buffer.
		now we need to save the buffer to a file
		*/
		if (strcmp(command, "get") == 0) {  
			save_file(fname, result, strlen(result));
		}

		/* LS
		print results for 'ls' command 
		*/
		if (strcmp(command, "ls") == 0) {
			printf("Files in server's current working directory:\n%s\n", result);
		}

		/* PUT
		status message from server about whether put was successful or not
		*/
		if (strcmp(command, "put") == 0) { 
			printf("%s\n", result);
		}

		/* DELETE
		status message from server about whether delete was successful or not
		*/
		if (strcmp(command, "delete") == 0) {
			printf("%s\n", result);
		}
		free(result);
	}	


	close_socket_helper(client_fd);
	return 0;
}
