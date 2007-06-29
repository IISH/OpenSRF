#include <opensrf/socket_bundle.h>

/* buffer used to read from the sockets */
#define RBUFSIZE 1024

static socket_node* _socket_add_node(socket_manager* mgr,
		int endpoint, int addr_type, int sock_fd, int parent_id );
static socket_node* socket_find_node(socket_manager* mgr, int sock_fd);
static void socket_remove_node(socket_manager*, int sock_fd);
static int _socket_send(int sock_fd, const char* data, int flags);
static int _socket_route_data(socket_manager* mgr, int num_active, fd_set* read_set);
static int _socket_route_data_id( socket_manager* mgr, int sock_id);
static int _socket_handle_new_client(socket_manager* mgr, socket_node* node);
static int _socket_handle_client_data(socket_manager* mgr, socket_node* node);


/* -------------------------------------------------------------------- 
	Test Code 
	-------------------------------------------------------------------- */
/*
int count = 0;
void printme(void* blob, socket_manager* mgr, 
		int sock_fd, char* data, int parent_id) {

	fprintf(stderr, "Got data from socket %d with parent %d => %s", 
			sock_fd, parent_id, data );

	socket_send(sock_fd, data);

	if(count++ > 2) {
		socket_disconnect(mgr, sock_fd);
		_socket_print_list(mgr);
	}
}

int main(int argc, char* argv[]) {
	socket_manager manager;
	memset(&manager, 0, sizeof(socket_manager));
	int port = 11000;
	if(argv[1])
		port = atoi(argv[1]);

	manager.data_received = &printme;
	socket_open_tcp_server(&manager, port);

	while(1)
		socket_wait_all(&manager, -1);

	return 0;
}
*/
/* -------------------------------------------------------------------- */


/* allocates and inserts a new socket node into the nodeset.
	if parent_id is positive and non-zero, it will be set */
static socket_node* _socket_add_node(socket_manager* mgr, 
		int endpoint, int addr_type, int sock_fd, int parent_id ) {

	if(mgr == NULL) return NULL;
	osrfLogInternal( OSRF_LOG_MARK, "Adding socket node with fd %d", sock_fd);
	socket_node* new_node = safe_malloc(sizeof(socket_node));

	new_node->endpoint	= endpoint;
	new_node->addr_type	= addr_type;
	new_node->sock_fd		= sock_fd;
	new_node->next			= NULL;
	new_node->parent_id = 0;
	if(parent_id > 0)
		new_node->parent_id = parent_id;

	new_node->next			= mgr->socket;
	mgr->socket				= new_node;
	return new_node;
}

/* creates a new server socket node and adds it to the socket set.
	returns new socket fd on success.  -1 on failure.
	socket_type is one of INET or UNIX  */
int socket_open_tcp_server(socket_manager* mgr, int port, char* listen_ip) {

	if( mgr == NULL ) {
		osrfLogWarning( OSRF_LOG_MARK, "socket_open_tcp_server(): NULL mgr"); 
		return -1;
	}

	int sock_fd;
	struct sockaddr_in server_addr;

	errno = 0;
	sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	if(sock_fd < 0) {
		osrfLogWarning( OSRF_LOG_MARK, "socket_open_tcp_server(): Unable to create TCP socket: %s",
			strerror( errno ) );
		return -1;
	}

	server_addr.sin_family = AF_INET;

	if(listen_ip != NULL) {
		server_addr.sin_addr.s_addr = inet_addr(listen_ip);
	} else {
		server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	}

	server_addr.sin_port = htons(port);

	errno = 0;
	if(bind( sock_fd, (struct sockaddr*) &server_addr, sizeof(server_addr)) < 0) {
		osrfLogWarning( OSRF_LOG_MARK, "socket_open_tcp_server(): cannot bind to port %d: %s",
			port, strerror( errno ) );
		return -1;
	}

	errno = 0;
	if(listen(sock_fd, 20) == -1) {
		osrfLogWarning( OSRF_LOG_MARK, "socket_open_tcp_server(): listen() returned error: %s",
			strerror( errno ) );
		return -1;
	}

	_socket_add_node(mgr, SERVER_SOCKET, INET, sock_fd, 0);
	return sock_fd;
}

int socket_open_unix_server(socket_manager* mgr, char* path) {
	if(mgr == NULL || path == NULL) return -1;

	osrfLogDebug( OSRF_LOG_MARK, "opening unix socket at %s", path);
	int sock_fd;
	struct sockaddr_un server_addr;

	errno = 0;
	sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if(sock_fd < 0){
		osrfLogWarning( OSRF_LOG_MARK, "socket_open_unix_server(): socket() failed: %s",
			strerror( errno ) );
		return -1;
	}

	server_addr.sun_family = AF_UNIX;
	strcpy(server_addr.sun_path, path);

	errno = 0;
	if( bind(sock_fd, (struct sockaddr*) &server_addr, 
				sizeof(struct sockaddr_un)) < 0) {
		osrfLogWarning( OSRF_LOG_MARK, 
			"socket_open_unix_server(): cannot bind to unix port %s: %s",
			path, strerror( errno ) );
		return -1;
	}

	errno = 0;
	if(listen(sock_fd, 20) == -1) {
		osrfLogWarning( OSRF_LOG_MARK, "socket_open_unix_server(): listen() returned error: %s",
			strerror( errno ) );
		return -1;
	}

	osrfLogDebug( OSRF_LOG_MARK, "unix socket successfully opened");
	
	int i = 1;

	/* causing problems with router for some reason ... */
	//osrfLogDebug( OSRF_LOG_MARK, "Setting SO_REUSEADDR");
	//setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i));
	
	//osrfLogDebug( OSRF_LOG_MARK, "Setting TCP_NODELAY");
	setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &i, sizeof(i));

	_socket_add_node(mgr, SERVER_SOCKET, UNIX, sock_fd, 0);
	return sock_fd;
}



int socket_open_udp_server( 
		socket_manager* mgr, int port, char* listen_ip ) {

	int sockfd;
	struct sockaddr_in server_addr;

	errno = 0;
	if( (sockfd = socket( AF_INET, SOCK_DGRAM, 0 )) < 0 ) {
		osrfLogWarning( OSRF_LOG_MARK, "Unable to create UDP socket: %s", strerror( errno ) );
		return -1;
	}

	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port);
	if(listen_ip) server_addr.sin_addr.s_addr = inet_addr(listen_ip);
	else server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	errno = 0;
	if( (bind (sockfd, (struct sockaddr *) &server_addr,sizeof(server_addr))) ) {
		osrfLogWarning( OSRF_LOG_MARK, "Unable to bind to UDP port %d: %s",
			port, strerror( errno ) );
		return -1;
	}

	_socket_add_node(mgr, SERVER_SOCKET, INET, sockfd, 0);
	return sockfd;
}


int socket_open_tcp_client(socket_manager* mgr, int port, char* dest_addr) {

	struct sockaddr_in remoteAddr, localAddr;
   struct hostent *hptr;
   int sock_fd;

   // ------------------------------------------------------------------
   // Create the socket
   // ------------------------------------------------------------------
   errno = 0;
   if( (sock_fd = socket( AF_INET, SOCK_STREAM, 0 )) < 0 ) {
	   osrfLogWarning( OSRF_LOG_MARK,  "socket_open_tcp_client(): Cannot create TCP socket: %s",
			strerror( errno ) );
      return -1;
   }

	int i = 1;
	//osrfLogDebug( OSRF_LOG_MARK, "Setting TCP_NODELAY");
	setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &i, sizeof(i));


   // ------------------------------------------------------------------
   // Get the hostname
   // ------------------------------------------------------------------
   errno = 0;
   if( (hptr = gethostbyname( dest_addr ) ) == NULL ) {
	   osrfLogWarning(  OSRF_LOG_MARK, "socket_open_tcp_client(): Unknown Host => %s: %s",
						dest_addr, strerror( errno ) );
      return -1;
   }

   // ------------------------------------------------------------------
   // Construct server info struct
   // ------------------------------------------------------------------
   memset( &remoteAddr, 0, sizeof(remoteAddr));
   remoteAddr.sin_family = AF_INET;
   remoteAddr.sin_port = htons( port );
   memcpy( (char*) &remoteAddr.sin_addr.s_addr,
         hptr->h_addr_list[0], hptr->h_length );

   // ------------------------------------------------------------------
   // Construct local info struct
   // ------------------------------------------------------------------
   memset( &localAddr, 0, sizeof( localAddr ) );
   localAddr.sin_family = AF_INET;
   localAddr.sin_addr.s_addr = htonl( INADDR_ANY );
   localAddr.sin_port = htons(0);

   // ------------------------------------------------------------------
   // Bind to a local port
   // ------------------------------------------------------------------
   errno = 0;
   if( bind( sock_fd, (struct sockaddr *) &localAddr, sizeof( localAddr ) ) < 0 ) {
	   osrfLogWarning( OSRF_LOG_MARK, "socket_open_tcp_client(): Cannot bind to local port: %s",
		strerror( errno ) );
      return -1;
   }

   // ------------------------------------------------------------------
   // Connect to server
   // ------------------------------------------------------------------
   errno = 0;
   if( connect( sock_fd, (struct sockaddr*) &remoteAddr, sizeof( struct sockaddr_in ) ) < 0 ) {
	   osrfLogWarning( OSRF_LOG_MARK, "socket_open_tcp_client(): Cannot connect to server %s: %s",
		   dest_addr, strerror(errno) );
	   return -1;
   }

	_socket_add_node(mgr, CLIENT_SOCKET, INET, sock_fd, -1 );

   return sock_fd;
}


int socket_open_udp_client( 
		socket_manager* mgr, int port, char* dest_addr) {

	int sockfd;
	struct sockaddr_in client_addr, server_addr;
	struct hostent* host;

	errno = 0;
	if( (host = gethostbyname(dest_addr)) == NULL) {
		osrfLogWarning( OSRF_LOG_MARK, "Unable to resolve host: %s: %s",
			dest_addr, strerror( errno ) );
		return -1;
	}

	server_addr.sin_family = host->h_addrtype;
	memcpy((char *) &server_addr.sin_addr.s_addr,
			     host->h_addr_list[0], host->h_length);
	server_addr.sin_port = htons(port);

	errno = 0;
	if( (sockfd = socket(AF_INET,SOCK_DGRAM,0)) < 0 ) {
		osrfLogWarning( OSRF_LOG_MARK, "socket_open_udp_client(): Unable to create UDP socket: %s", strerror( errno ) );
		return -1;
	}

	client_addr.sin_family = AF_INET;
	client_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	client_addr.sin_port = htons(0);

	errno = 0;
	if( (bind(sockfd, (struct sockaddr *) &client_addr, sizeof(client_addr))) < 0 ) {
		osrfLogWarning( OSRF_LOG_MARK, "Unable to bind UDP socket: %s", strerror( errno ) );
		return -1;
	}

	_socket_add_node(mgr, CLIENT_SOCKET, INET, sockfd, -1 );

	return sockfd;
}


int socket_open_unix_client(socket_manager* mgr, char* sock_path) {

	int sock_fd, len;
   struct sockaddr_un usock;

   errno = 0;
   if( (sock_fd = socket( AF_UNIX, SOCK_STREAM, 0 )) < 0 ) {
	   osrfLogWarning(  OSRF_LOG_MARK, "socket_open_unix_client(): Cannot create UNIX socket: %s", strerror( errno ) );
		return -1;
	}

   usock.sun_family = AF_UNIX;
   strcpy( usock.sun_path, sock_path );

   len = sizeof( usock.sun_family ) + strlen( usock.sun_path );

   errno = 0;
   if( connect( sock_fd, (struct sockaddr *) &usock, len ) < 0 ) {
	   osrfLogWarning(  OSRF_LOG_MARK, "Error connecting to unix socket: %s",
			strerror( errno ) );
		return -1;
	}

	_socket_add_node(mgr, CLIENT_SOCKET, UNIX, sock_fd, -1 );

   return sock_fd;
}


/* returns the socket_node with the given sock_fd */
static socket_node* socket_find_node(socket_manager* mgr, int sock_fd) {
	if(mgr == NULL) return NULL;
	socket_node* node = mgr->socket;
	while(node) {
		if(node->sock_fd == sock_fd)
			return node;
		node = node->next;
	}
	return NULL;
}

/* removes the node with the given sock_fd from the list and frees it */
static void socket_remove_node(socket_manager* mgr, int sock_fd) {

	if(mgr == NULL) return;

	osrfLogDebug( OSRF_LOG_MARK, "removing socket %d", sock_fd);

	socket_node* head = mgr->socket;
	socket_node* tail = head;
	if(head == NULL) return;

	/* if removing the first node in the list */
	if(head->sock_fd == sock_fd) {
		mgr->socket = head->next;
		free(head);
		return;
	}

	head = head->next;

	/* if removing any other node */
	while(head) {
		if(head->sock_fd == sock_fd) {
			tail->next = head->next;
			free(head);
			return;
		}
		tail = head;
		head = head->next;
	}
}


void _socket_print_list(socket_manager* mgr) {
	if(mgr == NULL) return;
	socket_node* node = mgr->socket;
	osrfLogDebug( OSRF_LOG_MARK, "socket_node list: [");
	while(node) {
		osrfLogDebug( OSRF_LOG_MARK, "sock_fd: %d | parent_id: %d", 
				node->sock_fd, node->parent_id);
		node = node->next;
	}
	osrfLogDebug( OSRF_LOG_MARK, "]");
}

/* sends the given data to the given socket */
int socket_send(int sock_fd, const char* data) {
	return _socket_send( sock_fd, data, 0);
}

/* utility method */
static int _socket_send(int sock_fd, const char* data, int flags) {

	signal(SIGPIPE, SIG_IGN); /* in case a unix socket was closed */

	errno = 0;
	size_t r = send( sock_fd, data, strlen(data), flags );
	int local_errno = errno;
	
	if( r == -1 ) {
		osrfLogWarning( OSRF_LOG_MARK, "_socket_send(): Error sending data with return %d", r );
		osrfLogWarning( OSRF_LOG_MARK, "Last Sys Error: %s", strerror(local_errno));
		return -1;
	}

	return 0;
}


/* sends the given data to the given socket. 
 * sets the send flag MSG_DONTWAIT which will allow the 
 * process to continue even if the socket buffer is full
 * returns 0 on success, -1 otherwise */
//int socket_send_nowait( int sock_fd, const char* data) {
//	return _socket_send( sock_fd, data, MSG_DONTWAIT);
//}


/*
 * Waits at most usecs microseconds for the send buffer of the given
 * socket to accept new data.  This does not guarantee that the 
 * socket will accept all the data we want to give it.
 */
int socket_send_timeout( int sock_fd, const char* data, int usecs ) {

	fd_set write_set;
	FD_ZERO( &write_set );
	FD_SET( sock_fd, &write_set );

	int mil = 1000000;
	int secs = (int) usecs / mil;
	usecs = usecs - (secs * mil);

	struct timeval tv;
	tv.tv_sec = secs;
	tv.tv_usec = usecs;

	errno = 0;
	int ret = select( sock_fd + 1, NULL, &write_set, NULL, &tv);
	if( ret > 0 ) return _socket_send( sock_fd, data, 0);

	osrfLogError(OSRF_LOG_MARK, "socket_send_timeout(): "
		"timed out on send for socket %d after %d secs, %d usecs: %s",
		sock_fd, secs, usecs, strerror( errno ) );

	return -1;
}


/* disconnects the node with the given sock_fd and removes
	it from the socket set */
void socket_disconnect(socket_manager* mgr, int sock_fd) {
	osrfLogInternal( OSRF_LOG_MARK, "Closing socket %d", sock_fd);
	close( sock_fd );
	socket_remove_node(mgr, sock_fd);
}


/* we assume that if select() fails, the socket is no longer valid */
int socket_connected(int sock_fd) {
	fd_set read_set;
	FD_ZERO( &read_set );
	FD_SET( sock_fd, &read_set );
	if( select( sock_fd + 1, &read_set, NULL, NULL, NULL) == -1 ) 
		return 0;
	return 1;

}

/* this only waits on the server socket and does not handle the actual
	data coming in from the client..... XXX */
int socket_wait(socket_manager* mgr, int timeout, int sock_fd) {

	int retval = 0;
	fd_set read_set;
	FD_ZERO( &read_set );
	FD_SET( sock_fd, &read_set );

	struct timeval tv;
	tv.tv_sec = timeout;
	tv.tv_usec = 0;
	errno = 0;

	if( timeout < 0 ) {  

		// If timeout is -1, we block indefinitely
		if( (retval = select( sock_fd + 1, &read_set, NULL, NULL, NULL)) == -1 ) {
			osrfLogDebug( OSRF_LOG_MARK, "Call to select() interrupted: Sys Error: %s", strerror(errno));
			return -1;
		}

	} else if( timeout > 0 ) { /* timeout of 0 means don't block */

		if( (retval = select( sock_fd + 1, &read_set, NULL, NULL, &tv)) == -1 ) {
			osrfLogDebug( OSRF_LOG_MARK, "Call to select() interrupted: Sys Error: %s", strerror(errno));
			return -1;
		}
	}

	osrfLogInternal( OSRF_LOG_MARK, "%d active sockets after select()", retval);
	return _socket_route_data_id(mgr, sock_fd);
}


int socket_wait_all(socket_manager* mgr, int timeout) {

	if(mgr == NULL) {
		osrfLogWarning( OSRF_LOG_MARK,  "socket_wait_all(): null mgr" );
		return -1;
	}

	int retval = 0;
	fd_set read_set;
	FD_ZERO( &read_set );

	socket_node* node = mgr->socket;
	int max_fd = 0;
	while(node) {
		osrfLogInternal( OSRF_LOG_MARK, "Adding socket fd %d to select set",node->sock_fd);
		FD_SET( node->sock_fd, &read_set );
		if(node->sock_fd > max_fd) max_fd = node->sock_fd;
		node = node->next;
	}
	max_fd += 1;

	struct timeval tv;
	tv.tv_sec = timeout;
	tv.tv_usec = 0;
	errno = 0;

	if( timeout < 0 ) {  

		// If timeout is -1, there is no timeout passed to the call to select
		if( (retval = select( max_fd, &read_set, NULL, NULL, NULL)) == -1 ) {
			osrfLogWarning( OSRF_LOG_MARK, "select() call aborted: %s", strerror(errno));
			return -1;
		}

	} else if( timeout != 0 ) { /* timeout of 0 means don't block */

		if( (retval = select( max_fd, &read_set, NULL, NULL, &tv)) == -1 ) {
			osrfLogWarning( OSRF_LOG_MARK, "select() call aborted: %s", strerror(errno));
			return -1;
		}
	}

	osrfLogDebug( OSRF_LOG_MARK, "%d active sockets after select()", retval);
	return _socket_route_data(mgr, retval, &read_set);
}

/* iterates over the sockets in the set and handles active sockets.
	new sockets connecting to server sockets cause the creation
	of a new socket node.
	Any new data read is is passed off to the data_received callback
	as it arrives */
/* determines if we're receiving a new client or data
	on an existing client */
static int _socket_route_data(
	socket_manager* mgr, int num_active, fd_set* read_set) {

	if(!(mgr && read_set)) return -1;

	int last_failed_id = -1;


	/* come back here if someone yanks a socket_node from beneath us */
	while(1) {

		socket_node* node = mgr->socket;
		int handled = 0;
		int status = 0;
		
		while(node && (handled < num_active)) {
	
			int sock_fd = node->sock_fd;
			
			if(last_failed_id != -1) {
				/* in case it was not removed by our overlords */
				osrfLogInternal( OSRF_LOG_MARK, "Attempting to remove last_failed_id of %d", last_failed_id);
				socket_remove_node( mgr, last_failed_id );
				last_failed_id = -1;
				status = -1;
				break;
			}
	
			/* does this socket have data? */
			if( FD_ISSET( sock_fd, read_set ) ) {
	
				osrfLogInternal( OSRF_LOG_MARK, "Socket %d active", sock_fd);
				handled++;
				FD_CLR(sock_fd, read_set);
	
				if(node->endpoint == SERVER_SOCKET) 
					_socket_handle_new_client(mgr, node);
	
				else
					status = _socket_handle_client_data(mgr, node);
	
				/* someone may have yanked a socket_node out from under 
					us...start over with the first socket */
				if(status == -1)  {
					last_failed_id = sock_fd;
					osrfLogInternal( OSRF_LOG_MARK, "Backtracking back to start of loop because "
							"of -1 return code from _socket_handle_client_data()");
				}
			}

			if(status == -1) break;
			node = node->next;

		} // is_set

		if(status == 0) break;
		if(status == -1) status = 0;
	} 

	return 0;
}


/* routes data from a single known socket */
static int _socket_route_data_id( socket_manager* mgr, int sock_id) {
	socket_node* node = socket_find_node(mgr, sock_id);	
	int status = 0;

	if(node) {
		if(node->endpoint == SERVER_SOCKET) 
			_socket_handle_new_client(mgr, node);
	
		if(node->endpoint == CLIENT_SOCKET ) 
			status = _socket_handle_client_data(mgr, node);

		if(status == -1) {
			socket_remove_node(mgr, sock_id);
			return -1;
		}
		return 0;
	} 

	return -1;
}


static int _socket_handle_new_client(socket_manager* mgr, socket_node* node) {
	if(mgr == NULL || node == NULL) return -1;

	errno = 0;
	int new_sock_fd;
	new_sock_fd = accept(node->sock_fd, NULL, NULL);
	if(new_sock_fd < 0) {
		osrfLogWarning( OSRF_LOG_MARK, "_socket_handle_new_client(): accept() failed: %s",
			strerror( errno ) );
		return -1;
	}

	if(node->addr_type == INET) {
		_socket_add_node(mgr, CLIENT_SOCKET, INET, new_sock_fd, node->sock_fd);
		osrfLogDebug( OSRF_LOG_MARK, "Adding new INET client for %d", node->sock_fd);

	} else if(node->addr_type == UNIX) {
		_socket_add_node(mgr, CLIENT_SOCKET, UNIX, new_sock_fd, node->sock_fd);
		osrfLogDebug( OSRF_LOG_MARK, "Adding new UNIX client for %d", node->sock_fd);
	}

	return 0;
}


static int _socket_handle_client_data(socket_manager* mgr, socket_node* node) {
	if(mgr == NULL || node == NULL) return -1;

	char buf[RBUFSIZE];
	int read_bytes;
	int sock_fd = node->sock_fd;

	memset(buf, 0, RBUFSIZE);
	set_fl(sock_fd, O_NONBLOCK);

	osrfLogInternal( OSRF_LOG_MARK, "%ld : Received data at %f\n", (long) getpid(), get_timestamp_millis());

	while( (read_bytes = recv(sock_fd, buf, RBUFSIZE-1, 0) ) > 0 ) {
		osrfLogInternal( OSRF_LOG_MARK, "Socket %d Read %d bytes and data: %s", sock_fd, read_bytes, buf);
		if(mgr->data_received)
			mgr->data_received(mgr->blob, mgr, sock_fd, buf, node->parent_id);

		memset(buf, 0, RBUFSIZE);
	}
    int local_errno = errno; /* capture errno as set by recv() */

	if(socket_find_node(mgr, sock_fd)) {  /* someone may have closed this socket */
		clr_fl(sock_fd, O_NONBLOCK); 
		if(read_bytes < 0) { 
			if(local_errno != EAGAIN) 
				osrfLogWarning(OSRF_LOG_MARK,  " * Error reading socket with error %s", strerror(local_errno));
		}

	} else { return -1; } /* inform the caller that this node has been tampered with */

	if(read_bytes == 0) {  /* socket closed by client */
		if(mgr->on_socket_closed) {
			mgr->on_socket_closed(mgr->blob, sock_fd);
		}
		return -1;
	}

	return 0;

}


void socket_manager_free(socket_manager* mgr) {
	if(mgr == NULL) return;
	socket_node* tmp;
	while(mgr->socket) {
		tmp = mgr->socket->next;
		socket_disconnect(mgr, mgr->socket->sock_fd);
		mgr->socket = tmp;
	}
	free(mgr);

}
