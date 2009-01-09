#include <opensrf/transport_session.h>

// ---------------------------------------------------------------------------------
// Callback for handling the startElement event.  Much of the jabber logic occurs
// in this and the characterHandler callbacks.
// Here we check for the various top level jabber elements: body, iq, etc.
// ---------------------------------------------------------------------------------
static void startElementHandler( 
		void *session, const xmlChar *name, const xmlChar **atts);

// ---------------------------------------------------------------------------------
// Callback for handling the endElement event.  Updates the Jabber state machine
// to let us know the element is over.
// ---------------------------------------------------------------------------------
static void endElementHandler( void *session, const xmlChar *name);

// ---------------------------------------------------------------------------------
// This is where we extract XML text content.  In particular, this is useful for
// extracting Jabber message bodies.
// ---------------------------------------------------------------------------------
static void characterHandler(
		void *session, const xmlChar *ch, int len);

static void parseWarningHandler( void *session, const char* msg, ... );
static void parseErrorHandler( void *session, const char* msg, ... );

// ---------------------------------------------------------------------------------
// Tells the SAX parser which functions will be used as event callbacks
// ---------------------------------------------------------------------------------
static xmlSAXHandler SAXHandlerStruct = {
   NULL,							/* internalSubset */
   NULL,							/* isStandalone */
   NULL,							/* hasInternalSubset */
   NULL,							/* hasExternalSubset */
   NULL,							/* resolveEntity */
   NULL,							/* getEntity */
   NULL,							/* entityDecl */
   NULL,							/* notationDecl */
   NULL,							/* attributeDecl */
   NULL,							/* elementDecl */
   NULL,							/* unparsedEntityDecl */
   NULL,							/* setDocumentLocator */
   NULL,							/* startDocument */
   NULL,							/* endDocument */
   startElementHandler,		/* startElement */
   endElementHandler,		/* endElement */
   NULL,							/* reference */
   characterHandler,			/* characters */
   NULL,							/* ignorableWhitespace */
   NULL,							/* processingInstruction */
   NULL,							/* comment */
   parseWarningHandler,		/* xmlParserWarning */
   parseErrorHandler,		/* xmlParserError */
   NULL,							/* xmlParserFatalError : unused */
   NULL,							/* getParameterEntity */
   NULL,							/* cdataBlock; */
   NULL,							/* externalSubset; */
   1,
   NULL,
   NULL,							/* startElementNs */
   NULL,							/* endElementNs */
   NULL							/* xmlStructuredErrorFunc */
};

// ---------------------------------------------------------------------------------
// Our SAX handler pointer.
// ---------------------------------------------------------------------------------
static const xmlSAXHandlerPtr SAXHandler = &SAXHandlerStruct;

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 256
#endif

static void grab_incoming(void* blob, socket_manager* mgr, int sockid, char* data, int parent);
static int reset_session_buffers( transport_session* session );
static char* get_xml_attr( const xmlChar** atts, const char* attr_name );

// ---------------------------------------------------------------------------------
// returns a built and allocated transport_session object.
// This codes does no network activity, only memory initilization
// ---------------------------------------------------------------------------------
transport_session* init_transport(  const char* server, 
	int port, const char* unix_path, void* user_data, int component ) {

	/* create the session struct */
	transport_session* session = 
		(transport_session*) safe_malloc( sizeof(transport_session) );

	session->user_data = user_data;

	session->component = component;

	/* initialize the data buffers */
	session->body_buffer			= buffer_init( JABBER_BODY_BUFSIZE );
	session->subject_buffer		= buffer_init( JABBER_SUBJECT_BUFSIZE );
	session->thread_buffer		= buffer_init( JABBER_THREAD_BUFSIZE );
	session->from_buffer			= buffer_init( JABBER_JID_BUFSIZE );
	session->status_buffer		= buffer_init( JABBER_STATUS_BUFSIZE );
	session->recipient_buffer	= buffer_init( JABBER_JID_BUFSIZE );
	session->message_error_type = buffer_init( JABBER_JID_BUFSIZE );
	session->session_id			= buffer_init( 64 );

	session->message_error_code = 0;

	/* for OpenSRF extensions */
	session->router_to_buffer		= buffer_init( JABBER_JID_BUFSIZE );
	session->router_from_buffer	= buffer_init( JABBER_JID_BUFSIZE );
	session->osrf_xid_buffer	= buffer_init( JABBER_JID_BUFSIZE );
	session->router_class_buffer	= buffer_init( JABBER_JID_BUFSIZE );
	session->router_command_buffer	= buffer_init( JABBER_JID_BUFSIZE );

	session->router_broadcast   = 0;

	/* initialize the jabber state machine */
	session->state_machine = (jabber_machine*) safe_malloc( sizeof(jabber_machine) );
	session->state_machine->connected        = 0;
	session->state_machine->connecting       = 0;
	session->state_machine->in_message       = 0;
	session->state_machine->in_message_body  = 0;
	session->state_machine->in_thread        = 0;
	session->state_machine->in_subject       = 0;
	session->state_machine->in_error         = 0;
	session->state_machine->in_message_error = 0;
	session->state_machine->in_iq            = 0;
	session->state_machine->in_presence      = 0;
	session->state_machine->in_status        = 0;

	/* initialize the sax push parser */
	session->parser_ctxt = xmlCreatePushParserCtxt(SAXHandler, session, "", 0, NULL);

	/* initialize the transport_socket structure */
	session->sock_mgr = (socket_manager*) safe_malloc( sizeof(socket_manager) );

	session->sock_mgr->data_received = &grab_incoming;
	session->sock_mgr->on_socket_closed = NULL;
	session->sock_mgr->socket = NULL;
	session->sock_mgr->blob	= session;
	
	session->port = port;
	session->server = strdup(server);
	if(unix_path) 	
		session->unix_path = strdup(unix_path);
	else session->unix_path = NULL;

	session->sock_id = 0;
	session->message_callback = NULL;

	return session;
}



/* XXX FREE THE BUFFERS */
int session_free( transport_session* session ) {
	if( ! session ) { return 0; }

	if(session->sock_mgr)
		socket_manager_free(session->sock_mgr);

	if( session->state_machine ) free( session->state_machine );
	if( session->parser_ctxt) {
		xmlFreeDoc( session->parser_ctxt->myDoc );
		xmlFreeParserCtxt(session->parser_ctxt);
	}

	xmlCleanupCharEncodingHandlers();
	xmlDictCleanup();
	xmlCleanupParser();

	buffer_free(session->body_buffer);
	buffer_free(session->subject_buffer);
	buffer_free(session->thread_buffer);
	buffer_free(session->from_buffer);
	buffer_free(session->recipient_buffer);
	buffer_free(session->status_buffer);
	buffer_free(session->message_error_type);
	buffer_free(session->router_to_buffer);
	buffer_free(session->router_from_buffer);
	buffer_free(session->osrf_xid_buffer);
	buffer_free(session->router_class_buffer);
	buffer_free(session->router_command_buffer);
	buffer_free(session->session_id);

	free(session->server);
	free(session->unix_path);

	free( session );
	return 1;
}


int session_wait( transport_session* session, int timeout ) {
	if( ! session || ! session->sock_mgr ) {
		return 0;
	}

	int ret =  socket_wait( session->sock_mgr, timeout, session->sock_id );

	if( ret ) {
		osrfLogDebug(OSRF_LOG_MARK, "socket_wait returned error code %d", ret);
		session->state_machine->connected = 0;
	}
	return ret;
}

int session_send_msg( 
		transport_session* session, transport_message* msg ) {

	if( ! session ) { return -1; }

	if( ! session->state_machine->connected ) {
		osrfLogWarning(OSRF_LOG_MARK, "State machine is not connected in send_msg()");
		return -1;
	}

	message_prepare_xml( msg );
	return socket_send( session->sock_id, msg->msg_xml );

}


/* connects to server and connects to jabber */
int session_connect( transport_session* session, 
		const char* username, const char* password, 
		const char* resource, int connect_timeout, enum TRANSPORT_AUTH_TYPE auth_type ) {

	int size1 = 0;
	int size2 = 0;

	if( ! session ) { 
		osrfLogWarning(OSRF_LOG_MARK,  "session is null in connect" );
		return 0; 
	}


	char* server = session->server;

	if( ! session->sock_id ) {

		if(session->port > 0) {
			if( (session->sock_id = socket_open_tcp_client(
				session->sock_mgr, session->port, session->server)) <= 0 ) 
			return 0;

		} else if(session->unix_path != NULL) {
			if( (session->sock_id = socket_open_unix_client(
				session->sock_mgr, session->unix_path)) <= 0 ) 
			return 0;
		}
		else {
			osrfLogWarning( OSRF_LOG_MARK, "Can't open session: no port or unix path" );
			return 0;
		}
	}

	if( session->component ) {

		/* the first Jabber connect stanza */
		char our_hostname[HOST_NAME_MAX + 1] = "";
		gethostname(our_hostname, sizeof(our_hostname) );
		our_hostname[HOST_NAME_MAX] = '\0';
		size1 = 150 + strlen( server );
		char stanza1[ size1 ]; 
		snprintf( stanza1, sizeof(stanza1),
				"<stream:stream version='1.0' xmlns:stream='http://etherx.jabber.org/streams' "
				"xmlns='jabber:component:accept' to='%s' from='%s' xml:lang='en'>",
				username, our_hostname );

		/* send the first stanze */
		session->state_machine->connecting = CONNECTING_1;

		if( socket_send( session->sock_id, stanza1 ) ) {
			osrfLogWarning(OSRF_LOG_MARK, "error sending");
			return 0;
		}
	
		/* wait for reply */
		socket_wait(session->sock_mgr, connect_timeout, session->sock_id);
	
		/* server acknowledges our existence, now see if we can login */
		if( session->state_machine->connecting == CONNECTING_2 ) {
	
			int ss = session->session_id->n_used + strlen(password) + 5;
			char hashstuff[ss];
			snprintf( hashstuff, sizeof(hashstuff), "%s%s", session->session_id->buf, password );

			char* hash = shahash( hashstuff );
			size2 = 100 + strlen( hash );
			char stanza2[ size2 ];
			snprintf( stanza2, sizeof(stanza2), "<handshake>%s</handshake>", hash );
	
			if( socket_send( session->sock_id, stanza2 )  ) {
				osrfLogWarning(OSRF_LOG_MARK, "error sending");
				return 0;
			}
		}

	} else { /* we're not a component */

		/* the first Jabber connect stanza */
		size1 = 100 + strlen( server );
		char stanza1[ size1 ]; 
		snprintf( stanza1, sizeof(stanza1), 
				"<stream:stream to='%s' xmlns='jabber:client' "
				"xmlns:stream='http://etherx.jabber.org/streams'>",
			server );
	

		/* send the first stanze */
		session->state_machine->connecting = CONNECTING_1;
		if( socket_send( session->sock_id, stanza1 ) ) {
			osrfLogWarning(OSRF_LOG_MARK, "error sending");
			return 0;
		}


		/* wait for reply */
		socket_wait( session->sock_mgr, connect_timeout, session->sock_id ); /* make the timeout smarter XXX */

		if( auth_type == AUTH_PLAIN ) {

			/* the second jabber connect stanza including login info*/
			size2 = 150 + strlen( username ) + strlen(password) + strlen(resource);
			char stanza2[ size2 ];
			snprintf( stanza2, sizeof(stanza2), 
					"<iq id='123456789' type='set'><query xmlns='jabber:iq:auth'>"
					"<username>%s</username><password>%s</password><resource>%s</resource></query></iq>",
					username, password, resource );
	
			/* server acknowledges our existence, now see if we can login */
			if( session->state_machine->connecting == CONNECTING_2 ) {
				if( socket_send( session->sock_id, stanza2 )  ) {
					osrfLogWarning(OSRF_LOG_MARK, "error sending");
					return 0;
				}
			}

		} else if( auth_type == AUTH_DIGEST ) {

			int ss = session->session_id->n_used + strlen(password) + 5;
			char hashstuff[ss];
			snprintf( hashstuff, sizeof(hashstuff), "%s%s", session->session_id->buf, password );

			char* hash = shahash( hashstuff );

			/* the second jabber connect stanza including login info*/
			size2 = 150 + strlen( hash ) + strlen(password) + strlen(resource);
			char stanza2[ size2 ];
			snprintf( stanza2, sizeof(stanza2), 
					"<iq id='123456789' type='set'><query xmlns='jabber:iq:auth'>"
					"<username>%s</username><digest>%s</digest><resource>%s</resource></query></iq>",
					username, hash, resource );
	
			/* server acknowledges our existence, now see if we can login */
			if( session->state_machine->connecting == CONNECTING_2 ) {
				if( socket_send( session->sock_id, stanza2 )  ) {
					osrfLogWarning(OSRF_LOG_MARK, "error sending");
					return 0;
				}
			}

		}

	} // not component


	/* wait for reply */
	socket_wait( session->sock_mgr, connect_timeout, session->sock_id );

	if( session->state_machine->connected ) {
		/* yar! */
		return 1;
	}

	return 0;
}

// ---------------------------------------------------------------------------------
// TCP data callback.  Takes data from the socket handler and pushes it directly
// into the push parser
// ---------------------------------------------------------------------------------
static void grab_incoming(void* blob, socket_manager* mgr, int sockid, char* data, int parent) {
	transport_session* ses = (transport_session*) blob;
	if( ! ses ) { return; }
	xmlParseChunk(ses->parser_ctxt, data, strlen(data), 0);
}


static void startElementHandler(
	void *session, const xmlChar *name, const xmlChar **atts) {

	transport_session* ses = (transport_session*) session;
	if( ! ses ) { return; }

	
	if( strcmp( (char*) name, "message" ) == 0 ) {
		ses->state_machine->in_message = 1;
		buffer_add( ses->from_buffer, get_xml_attr( atts, "from" ) );
		buffer_add( ses->recipient_buffer, get_xml_attr( atts, "to" ) );
		buffer_add( ses->router_from_buffer, get_xml_attr( atts, "router_from" ) );
		buffer_add( ses->osrf_xid_buffer, get_xml_attr( atts, "osrf_xid" ) );
		buffer_add( ses->router_to_buffer, get_xml_attr( atts, "router_to" ) );
		buffer_add( ses->router_class_buffer, get_xml_attr( atts, "router_class" ) );
		buffer_add( ses->router_command_buffer, get_xml_attr( atts, "router_command" ) );
		char* broadcast = get_xml_attr( atts, "broadcast" );
		if( broadcast )
			ses->router_broadcast = atoi( broadcast );

		return;
	}

	if( ses->state_machine->in_message ) {

		if( strcmp( (char*) name, "body" ) == 0 ) {
			ses->state_machine->in_message_body = 1;
			return;
		}
	
		if( strcmp( (char*) name, "subject" ) == 0 ) {
			ses->state_machine->in_subject = 1;
			return;
		}
	
		if( strcmp( (char*) name, "thread" ) == 0 ) {
			ses->state_machine->in_thread = 1;
			return;
		}

	}

	if( strcmp( (char*) name, "presence" ) == 0 ) {
		ses->state_machine->in_presence = 1;
		buffer_add( ses->from_buffer, get_xml_attr( atts, "from" ) );
		buffer_add( ses->recipient_buffer, get_xml_attr( atts, "to" ) );
		return;
	}

	if( strcmp( (char*) name, "status" ) == 0 ) {
		ses->state_machine->in_status = 1;
		return;
	}


	if( strcmp( (char*) name, "stream:error" ) == 0 ) {
		ses->state_machine->in_error = 1;
		ses->state_machine->connected = 0;
		osrfLogWarning(  OSRF_LOG_MARK, "Received <stream:error> message from Jabber server" );
		return;
	}


	/* first server response from a connect attempt */
	if( strcmp( (char*) name, "stream:stream" ) == 0 ) {
		if( ses->state_machine->connecting == CONNECTING_1 ) {
			ses->state_machine->connecting = CONNECTING_2;
			buffer_add( ses->session_id, get_xml_attr(atts, "id") );
		}
	}

	if( strcmp( (char*) name, "handshake" ) == 0 ) {
		ses->state_machine->connected = 1;
		ses->state_machine->connecting = 0;
		return;
	}


	if( strcmp( (char*) name, "error" ) == 0 ) {
		ses->state_machine->in_message_error = 1;
		buffer_add( ses->message_error_type, get_xml_attr( atts, "type" ) );
		ses->message_error_code = atoi( get_xml_attr( atts, "code" ) );
		osrfLogInfo( OSRF_LOG_MARK,  "Received <error> message with type %s and code %s", 
			get_xml_attr( atts, "type"), get_xml_attr( atts, "code") );
		return;
	}

	if( strcmp( (char*) name, "iq" ) == 0 ) {
		ses->state_machine->in_iq = 1;

		if( strcmp( get_xml_attr(atts, "type"), "result") == 0 
				&& ses->state_machine->connecting == CONNECTING_2 ) {
			ses->state_machine->connected = 1;
			ses->state_machine->connecting = 0;
			return;
		}

		if( strcmp( get_xml_attr(atts, "type"), "error") == 0 ) {
			osrfLogWarning( OSRF_LOG_MARK,  "Error connecting to jabber" );
			return;
		}
	}
}

// ------------------------------------------------------------------
// Returns the value of the given XML attribute
// The xmlChar** construct is commonly returned from SAX event
// handlers.  Pass that in with the name of the attribute you want
// to retrieve.
// ------------------------------------------------------------------
static char* get_xml_attr( const xmlChar** atts, const char* attr_name ) {
	int i;
	if (atts != NULL) {
		for(i = 0;(atts[i] != NULL);i++) {
			if( strcmp( (char*) atts[i++], attr_name ) == 0 ) {
				if( atts[i] != NULL ) {
					return (char*) atts[i];
				}
			}
		}
	}
	return NULL;
}


// ------------------------------------------------------------------
// See which tags are ending
// ------------------------------------------------------------------
static void endElementHandler( void *session, const xmlChar *name) {
	transport_session* ses = (transport_session*) session;
	if( ! ses ) { return; }

	if( strcmp( (char*) name, "message" ) == 0 ) {


		/* pass off the message info the callback */
		if( ses->message_callback ) {

			/* here it's ok to pass in the raw buffers because
				message_init allocates new space for the chars 
				passed in */
			transport_message* msg =  message_init( 
				ses->body_buffer->buf, 
				ses->subject_buffer->buf,
				ses->thread_buffer->buf, 
				ses->recipient_buffer->buf, 
				ses->from_buffer->buf );

			message_set_router_info( msg, 
				ses->router_from_buffer->buf, 
				ses->router_to_buffer->buf, 
				ses->router_class_buffer->buf,
				ses->router_command_buffer->buf,
				ses->router_broadcast );

         message_set_osrf_xid( msg, ses->osrf_xid_buffer->buf );

			if( ses->message_error_type->n_used > 0 ) {
				set_msg_error( msg, ses->message_error_type->buf, ses->message_error_code );
			}

			if( msg == NULL ) { return; }
			ses->message_callback( ses->user_data, msg );
		}

		ses->state_machine->in_message = 0;
		reset_session_buffers( session );
		return;
	}
	
	if( strcmp( (const char*) name, "body" ) == 0 ) {
		ses->state_machine->in_message_body = 0;
		return;
	}

	if( strcmp( (const char*) name, "subject" ) == 0 ) {
		ses->state_machine->in_subject = 0;
		return;
	}

	if( strcmp( (const char*) name, "thread" ) == 0 ) {
		ses->state_machine->in_thread = 0;
		return;
	}
	
	if( strcmp( (const char*) name, "iq" ) == 0 ) {
		ses->state_machine->in_iq = 0;
		if( ses->message_error_code > 0 ) {
			osrfLogWarning( OSRF_LOG_MARK,  "Error in IQ packet: code %d",  ses->message_error_code );
			osrfLogWarning( OSRF_LOG_MARK,  "Error 401 means not authorized" );
		}
		reset_session_buffers( session );
		return;
	}

	if( strcmp( (const char*) name, "presence" ) == 0 ) {
		ses->state_machine->in_presence = 0;
		/*
		if( ses->presence_callback ) {
			// call the callback with the status, etc.
		}
		*/
		reset_session_buffers( session );
		return;
	}

	if( strcmp( (const char*) name, "status" ) == 0 ) {
		ses->state_machine->in_status = 0;
		return;
	}

	if( strcmp( (const char*) name, "error" ) == 0 ) {
		ses->state_machine->in_message_error = 0;
		return;
	}

	if( strcmp( (const char*) name, "error:error" ) == 0 ) {
		ses->state_machine->in_error = 0;
		return;
	}
}

static int reset_session_buffers( transport_session* ses ) {
	buffer_reset( ses->body_buffer );
	buffer_reset( ses->subject_buffer );
	buffer_reset( ses->thread_buffer );
	buffer_reset( ses->from_buffer );
	buffer_reset( ses->recipient_buffer );
	buffer_reset( ses->router_from_buffer );
	buffer_reset( ses->osrf_xid_buffer );
	buffer_reset( ses->router_to_buffer );
	buffer_reset( ses->router_class_buffer );
	buffer_reset( ses->router_command_buffer );
	buffer_reset( ses->message_error_type );
	buffer_reset( ses->session_id );

	return 1;
}

// ------------------------------------------------------------------
// takes data out of the body of the message and pushes it into
// the appropriate buffer
// ------------------------------------------------------------------
static void characterHandler(
		void *session, const xmlChar *ch, int len) {

	const char* p = (const char*) ch;

	transport_session* ses = (transport_session*) session;
	if( ! ses ) { return; }

	/* set the various message parts */
	if( ses->state_machine->in_message ) {

		if( ses->state_machine->in_message_body ) {
			buffer_add_n( ses->body_buffer, p, len );
		}

		if( ses->state_machine->in_subject ) {
			buffer_add_n( ses->subject_buffer, p, len );
		}

		if( ses->state_machine->in_thread ) {
			buffer_add_n( ses->thread_buffer, p, len );
		}
	}

	/* set the presence status */
	if( ses->state_machine->in_presence && ses->state_machine->in_status ) {
		buffer_add_n( ses->status_buffer, p, len );
	}

	if( ses->state_machine->in_error ) {
		/* for now... */
		osrfLogWarning( OSRF_LOG_MARK,  "ERROR XML fragment: %s\n", ch );
	}

}

/* XXX change to warning handlers */
static void  parseWarningHandler( void *session, const char* msg, ... ) {

	va_list args;
	va_start(args, msg);
	fprintf(stdout, "transport_session XML WARNING");
	vfprintf(stdout, msg, args);
	va_end(args);
	fprintf(stderr, "XML WARNING: %s\n", msg ); 
}

static void  parseErrorHandler( void *session, const char* msg, ... ){

	va_list args;
	va_start(args, msg);
	fprintf(stdout, "transport_session XML ERROR");
	vfprintf(stdout, msg, args);
	va_end(args);
	fprintf(stderr, "XML ERROR: %s\n", msg ); 

}

int session_disconnect( transport_session* session ) {
	if( session == NULL ) { return 0; }
	socket_send(session->sock_id, "</stream:stream>");
	socket_disconnect(session->sock_mgr, session->sock_id);
	return 0;
}

