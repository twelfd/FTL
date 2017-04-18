/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Socket connection routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"

// The backlog argument defines the maximum length
// to which the queue of pending connections for
// sockfd may grow. If a connection request arrives
// when the queue is full, the client may receive an
// error with an indication of ECONNREFUSED or, if
// the underlying protocol supports retransmission,
// the request may be ignored so that a later
// reattempt at connection succeeds.
#define BACKLOG 5
#define PORT 4711

int sockfd;
// Private prototype
void saveport(int port);

void init_socket(void)
{
	sockfd = socket(AF_INET, SOCK_STREAM, 0);

	if(sockfd < 0)
	{
		logg("Error opening socket");
		exit(EXIT_FAILURE);
	}

	// Set SO_REUSEADDR to allow re-binding to the port that has been used
	// previously by FTL. A common pattern is that you change FTL's
	// configuration file and need to restart that server to make it reload
	// its configuration. Without SO_REUSEADDR, the bind() call in the restarted
	// new instance will fail if there were connections open to the previous
	// instance when you killed it. Those connections will hold the TCP port in
	// the TIME_WAIT state for 30-120 seconds, so you fall into case 1 above.
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 }, sizeof(int));

	struct sockaddr_in serv_addr;
	// set all values in the buffer to zero
	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;

	if(config.socket_listenlocal)
		serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	else
		serv_addr.sin_addr.s_addr = INADDR_ANY;

	// The bind() system call binds a socket to an address,
	// in this case the address of the current host and
	// port number on which the server will run.
	// convert this to network byte order using the function htons()
	// which converts a port number in host byte order to a port number
	// in network byte order
	int port;
	for(port=PORT; port <= PORT+20; port++)
	{
		serv_addr.sin_port = htons(port);
		if(bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
		{
			logg("Error on binding on port %i", port);
		}
		else
		{
			break;
		}
	}
	if(port == PORT+20)
	{
		logg("Error listening on any port");
		exit(EXIT_FAILURE);
	}
	saveport(port);

	// The listen system call allows the process to listen on the socket for connections
	if(listen(sockfd,BACKLOG) == -1)
	{
		logg("Error on listening");
		exit(EXIT_FAILURE);
	}
}

void saveport(int port)
{
	FILE *f;
	if((f = fopen(FTLfiles.port, "w+")) == NULL)
	{
		logg("WARNING: Unable to write used port to file.");
		logg("         Continuing anyway (API might not find the port).");
	}
	else
	{
		fprintf(f, "%i", port);
		fclose(f);
	}
	logg("Listening on port %i", port);
}

void removeport(void)
{
	FILE *f;
	if((f = fopen(FTLfiles.port, "w+")) == NULL)
	{
		logg("WARNING: Unable to empty port file");
		return;
	}
	fclose(f);
}

void seom(char server_message[SOCKETBUFFERLEN], int sock)
{
	sprintf(server_message,"---EOM---\n\n");
	swrite(server_message, sock);
}

void swrite(char server_message[SOCKETBUFFERLEN], int sock)
{
	if(!write(sock, server_message, strlen(server_message)))
		logg("WARNING: Socket write returned error code %i", errno);
}

int listen_socket(void)
{
	struct sockaddr_in cli_addr;
	// set all values in the buffer to zero
	memset(&cli_addr, 0, sizeof(cli_addr));
	socklen_t clilen = sizeof(cli_addr);
	int clientsocket = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
	// if (newsockfd < 0)
		// printf("ERROR on accept");
	if(debugclients)
		logg("Client connected: %s, ID: %i", inet_ntoa (cli_addr.sin_addr), clientsocket);

	return clientsocket;
}

void close_socket(void)
{
	close(sockfd);
}

void *listenting_thread(void *args)
{
	int *newsock;
	// We will use the attributes object later to start all threads in detached mode
	pthread_attr_t attr;
	// Initialize thread attributes object with default attribute values
	pthread_attr_init(&attr);
	// When a detached thread terminates, its resources are automatically released back to
	// the system without the need for another thread to join with the terminated thread
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	// Set thread name
	prctl(PR_SET_NAME,"listener",0,0,0);

	// Listen as long as FTL is not killed
	while(!killed)
	{
		// Look for new clients that want to connect
		int csck = listen_socket();

		// Allocate memory used to transport client socket ID to client listening thread
		newsock = calloc(1,sizeof(int));
		*newsock = csck;

		pthread_t connection_thread;
		// Create a new thread
		if(pthread_create( &connection_thread, &attr, connection_handler_thread, (void*) newsock ) != 0)
		{
			// Log the error code description
			logg("WARNING: Unable to open clients processing thread, error: %s", strerror(errno));
		}
	}
	return 0;
}

void *connection_handler_thread(void *socket_desc)
{
	//Get the socket descriptor
	int sock = *(int*)socket_desc;
	// Store copy only for displaying the debug messages
	int sockID = sock;
	char client_message[SOCKETBUFFERLEN] = "";

	// Set thread name
	char threadname[16];
	sprintf(threadname,"client-%i",sockID);
	prctl(PR_SET_NAME,threadname,0,0,0);
	//Receive from client
	ssize_t n;
	while((n = recv(sock,client_message,SOCKETBUFFERLEN-1, 0)))
	{
		if (n > 0)
		{
			char *message = calloc(strlen(client_message)+1,sizeof(char));
			strcpy(message, client_message);

			// Clear client message receive buffer
			memset(client_message, 0, sizeof client_message);

			// Lock FTL data structure, since it is likely that it will be changed here
			// Requests should not be processed/answered when data is about to change
			enable_read_lock(threadname);

			process_request(message, &sock);
			free(message);

			// Release thread lock
			disable_thread_locks("connection_handler_thread");

			if(sock == 0)
			{
				// Client disconnected by seding EOT or ">quit"
				break;
			}
		}
		else if(n == -1)
		{
			if(debugclients)
				logg("Client connection interrupted, ID: %i", sockID);
		}
	}
	if(debugclients)
		logg("Client disconnected, ID: %i", sockID);

	//Free the socket pointer
	if(sock != 0)
		close(sock);
	free(socket_desc);

	return 0;
}
