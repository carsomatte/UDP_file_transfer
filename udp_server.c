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
#include <dirent.h>
#include <stdint.h>

# define BUFFER_SIZE 1024
# define MAX_FILENAME_LEN 255
# define CHUNK_DATA_SIZE 1400
# define TIMEOUT_MSEC 20

typedef struct {
	int sockfd;
	struct sockaddr_in client_addr;
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
void save_file(const char *filename, char *buf, size_t buf_len) {
    FILE *file = fopen(filename, "wb");
    if (!file) {
        perror("fopen");
        return;
    }

    fwrite(buf, 1, buf_len, file);
    fclose(file);
	printf("File '%s' saved in server's current working directory\n", filename);
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
		//printf("Server -> sending chunk with seq: %u\n", chunk->seq);
		sendto(ctx->sockfd, chunk, sizeof(Chunk), 0, (struct sockaddr *)&ctx->client_addr, ctx->addr_len);

		uint32_t ack;
		ssize_t n = recvfrom(ctx->sockfd, &ack, sizeof(ack), 0, NULL, NULL);
		//printf("Server -> received ack: %u\n", ack);

		if (n < 0) {
			printf("timeout on chunk %u, resending...\n", chunk->seq);
			retries++;
			if (retries > 5) {
				printf("could not connect to client after 5 retries, aborting...\n");
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
	ssize_t n = recvfrom(ctx->sockfd, chunk, sizeof(Chunk), 0, (struct sockaddr *)&ctx->client_addr, &ctx->addr_len);
    if (n < 0) {
        return -1;
	}

    uint32_t ack = chunk->seq;
    sendto(ctx->sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&ctx->client_addr, ctx->addr_len);
	//printf("sending ack: %u\n", ack);
    return 0;
}

/* 
Receives a buffer and breaks it into chunks to send 
if 'is_error' is 1, the chunk's 'is_error' field will be set to 1, otherwise it will be set to 0
*/
void send_buffer(UDPContext *ctx, char *buf, uint8_t is_error) {
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
			printf("Error with connecting to client. Chunks are not being sent properly...\n");
			return;
		}
	}
	clear_socket_timeout(ctx);
}



/*
 ********************************
 FUNCTIONS FOR SERVER JOBS
 ********************************
*/


/*
write to the buffer with error information 
and call 'send_buffer' to break it into chunks and send it
*/
void send_error(const char *msg, UDPContext *ctx) {
	char return_buf[BUFFER_SIZE];
	memset(return_buf, 0, sizeof(return_buf));

	snprintf(return_buf, sizeof(return_buf), "%s: %s", msg, strerror(errno));
	send_buffer(ctx, return_buf, 1);
}

/* 
**************
LS

fill an allocated buffer with list of files in current directory and
call 'send_buffer' to break it into chunks and send it
************** 
*/
void ls_helper(UDPContext *ctx) {
	DIR *dir = opendir(".");
    if (!dir) {
		send_error("Error opening directory on server", ctx);
		return;
	}

	// calculate total size needed for return_buff
    size_t total_size = 0;
    struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		/* skip the "." and ".." entries */
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		total_size += strlen(entry->d_name) + 2;  // +2 for ", "
	}
	rewinddir(dir);

	char *return_buff = calloc(total_size + 1, 1);
	if (!return_buff) {
		perror("calloc");
		send_error("Error allocating memory for directory listing", ctx);
		closedir(dir);
		return;
	}

	size_t offset = 0;
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;

		size_t name_len = strlen(entry->d_name);
		memcpy(return_buff + offset, entry->d_name, name_len);
		offset += name_len;
		return_buff[offset++] = ',';
		return_buff[offset++] = ' ';
	}

	if (offset >= 2)
		offset -= 2;
	return_buff[offset] = '\0';

	closedir(dir);
	send_buffer(ctx, return_buff, 0);
	free(return_buff);
}

/* 
**************
GET

lets skip the buffer and read the file directly into chunks 
and call 'chunk_send' on each one
************** 
*/
void get_helper(UDPContext *ctx, const char *filename) {
	init_socket_timeout(ctx);

	FILE *file = fopen(filename, "rb");
	if (!file) {
		send_error("Could not open file", ctx);
		return;
	}

	// calculate total chunks
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
			printf("Error with connecting to client. Chunks are not being sent properly...\n");
			fclose(file);
			return;
		}
    }
	fclose(file);

	clear_socket_timeout(ctx);
}

/* 
**************
PUT

we will receive the file in chunks and reassemble it in an allocated buffer,
then write the buffer to a file in our current working directory.
finally, we will check if the file was saved successfully and send a status message back to the client
************** 
*/
void put_helper(UDPContext *ctx, const char *filename) {
	// Receive all chunks
	Chunk chunk;
	uint32_t received = 0;
	uint32_t total = UINT32_MAX; // total is unknown until first chunk arrives
	char *result = NULL; // for reassembling full result

	while (received < total) {
		if (chunk_recv(ctx, &chunk) == -1) {
			printf("Error receiving chunk from client\n");
			free(result);
			return;
		}
		total = chunk.total;	

		// discard already received chunks
		if (chunk.seq != received) {
			printf("Server -> Received out of order chunk with seq %u, expected seq %u. Discarding chunk...\n", chunk.seq, received);
			continue;
		}

		// allocate once we know size from first chunk
		if (result == NULL) { 
			result = calloc(total * CHUNK_DATA_SIZE + 1, 1);
			if (!result) {
				perror("calloc");
				return;
			}
		}
	
		// copy chunk data into buffer at correct position
		memcpy(result + (chunk.seq * CHUNK_DATA_SIZE), chunk.data, chunk.data_len);
		received++;
	}
	save_file(filename, result, strlen(result));
	free(result);


	/*
	verify file was saved correctly by trying to open it
	send status message back to client about whether save was successful or not
	*/
	char return_buf[BUFFER_SIZE];
	memset(return_buf, 0, sizeof(return_buf));
	
	FILE *file = fopen(filename, "rb");
	if (!file) {
		send_error("Error saving file on server", ctx);
	}
	else {
		snprintf(return_buf, BUFFER_SIZE, "File '%s' received and saved successfully on server", filename);
		send_buffer(ctx, return_buf, 0);
		fclose(file);
	}
}

/* 
**************
DELETE

we will receive a file and check if it exists in our current working directory
if it does, we will delete the file and send a success message back to the client
if it does not, we will send an error message back to the client
************** 
*/
void delete_helper(UDPContext *ctx, const char *filename) {
	char return_buf[BUFFER_SIZE];
	memset(return_buf, 0, sizeof(return_buf));

	if (remove(filename) == 0) {
		snprintf(return_buf, BUFFER_SIZE, "File '%s' deleted successfully on server", filename);
		send_buffer(ctx, return_buf, 0);
	}
	else {
		send_error("Error deleting file on server", ctx);
	}
}



/* parse user input and call correct helper function */
int handle_datagram(const char *command, UDPContext *ctx) {
    char cmd[16];
    char filename[MAX_FILENAME_LEN];

    // Clear buffers
    memset(cmd, 0, sizeof(cmd));
    memset(filename, 0, sizeof(filename));

    // Try parsing two arguments
    int count = sscanf(command, "%15s %254s", cmd, filename);

    if (strcmp(cmd, "ls") == 0) {
	    ls_helper(ctx);
	    return 0;
    }
    else if (strcmp(cmd, "exit") == 0) { 
	    return -1;    
    }
    else if (strcmp(cmd, "get") == 0) {
	    get_helper(ctx, filename);
	    return 0;
    }
    else if (strcmp(cmd, "put") == 0) {
		put_helper(ctx, filename);
	    return 0;
    }
    else if (strcmp(cmd, "delete") == 0) {
		delete_helper(ctx, filename);
	    return 0;
    }
    else { //unknown command. will never be reached since client validates user input
	    send_error("Unknown Command", ctx);
	    return 0;
    }

}


int main(int argc, char* argv[]) {
    int port;
    int server_fd;
    struct sockaddr_in server_addr;
	struct sockaddr_in client_addr;
	
	char buf[BUFFER_SIZE]; // for receiving first command from client (ls, put, get, delete, exit)
	
	int opt = 1; //for SO_REUSEADDR in setsockopt

    if (argc != 2) {
        printf("Usage: ./udp_server <port_number>\n");
        return 1;
    }
    port = atoi(argv[1]);

	// Create Socket
	if ((server_fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
			perror("Socket Failed");
			exit(EXIT_FAILURE);
	}
	
	// Set Socket Options. Eliminates address already in use error
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
		perror("Setting Socket Options Failed");
		exit(EXIT_FAILURE);
	}

	// Build Server's Address
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = INADDR_ANY; // Bind to all interfaces
	server_addr.sin_port = htons(port); // Convert port to big-endian 

	// Bind Socket
	if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
		perror("Bind Failed");
		exit(EXIT_FAILURE);
	}
	
	printf("Ready to receive datagrams\n");
	while(1) {
		/* we will receive the first message from client 
		into a buffer on stack since it won't be variable length like files */
		memset(buf, 0, sizeof(buf));

		// Content needed for sending and receiving
		UDPContext ctx = {0};
		ctx.sockfd = server_fd;
		ctx.addr_len = sizeof(client_addr);

		// Receive client command
		Chunk chunk = {0};
		uint32_t received = 0;
		uint32_t total = UINT32_MAX;

		while (received < total) {
			if (chunk_recv(&ctx, &chunk) == -1) {
				printf("Error receiving chunk from client\n");
				break;
			}
			total = chunk.total;

			// discard already received chunks
			if (chunk.seq != received) {
				printf("Server -> Received out of order chunk with seq %u, expected seq %u. Discarding chunk...\n", chunk.seq, received);
				continue;
			}

			memcpy(buf + (chunk.seq * CHUNK_DATA_SIZE), chunk.data, chunk.data_len);
			// TODO: add bounds checking
			received++;
		}

		// Craft return datagram and send it back to client
		if (handle_datagram(buf, &ctx) == -1) {
			printf("Client Requested Server Exit...\n");
			break;
		}
	}
	close_socket_helper(server_fd);
	return 0;
}
