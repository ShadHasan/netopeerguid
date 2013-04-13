/*
 * libwebsockets-test-server - libwebsockets test implementation
 *
 * Copyright (C) 2010-2011 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include "libwebsockets.h"
#include "notification_module.h"

#ifndef TEST_NOTIFICATION_SERVER
#include <httpd.h>
#include <http_log.h>
#endif

#if defined(TEST_NOTIFICATION_SERVER) || defined(WITH_NOTIFICATIONS)
static int close_testing;
static int max_poll_elements;

static struct pollfd *pollfds;
static int *fd_lookup;
static int count_pollfds;
static int force_exit = 0;
static struct libwebsocket_context *context = NULL;
static server_rec *http_server = NULL;

/*
 * This demo server shows how to use libwebsockets for one or more
 * websocket protocols in the same server
 *
 * It defines the following websocket protocols:
 *
 *  dumb-increment-protocol:  once the socket is opened, an incrementing
 *				ascii string is sent down it every 50ms.
 *				If you send "reset\n" on the websocket, then
 *				the incrementing number is reset to 0.
 *
 *  lws-mirror-protocol: copies any received packet to every connection also
 *				using this protocol, including the sender
 */

enum demo_protocols {
	/* always first */
	PROTOCOL_HTTP = 0,

	PROTOCOL_NOTIFICATION,

	/* always last */
	DEMO_PROTOCOL_COUNT
};


#define LOCAL_RESOURCE_PATH "."
char *resource_path = LOCAL_RESOURCE_PATH;

/*
 * We take a strict whitelist approach to stop ../ attacks
 */

struct serveable {
	const char *urlpath;
	const char *mimetype;
}; 

static const struct serveable whitelist[] = {
	{ "/favicon.ico", "image/x-icon" },
	{ "/libwebsockets.org-logo.png", "image/png" },

	/* last one is the default served if no match */
	{ "/test.html", "text/html" },
};

struct per_session_data__http {
	int fd;
};

/* this protocol server (always the first one) just knows how to do HTTP */

static int callback_http(struct libwebsocket_context *context,
		struct libwebsocket *wsi,
		enum libwebsocket_callback_reasons reason, void *user,
							   void *in, size_t len)
{
	char client_name[128];
	char client_ip[128];
	char buf[256];
	int n, m;
	unsigned char *p;
	static unsigned char buffer[4096];
	struct stat stat_buf;
	struct per_session_data__http *pss = (struct per_session_data__http *)user;
	int fd = (int)(long)in;

	switch (reason) {
	case LWS_CALLBACK_HTTP:

		/* check for the "send a big file by hand" example case */

		if (!strcmp((const char *)in, "/leaf.jpg")) {
			char leaf_path[1024];
			snprintf(leaf_path, sizeof(leaf_path), "%s/leaf.jpg", resource_path);

			/* well, let's demonstrate how to send the hard way */

			p = buffer;

			pss->fd = open(leaf_path, O_RDONLY);

			if (pss->fd < 0)
				return -1;

			fstat(pss->fd, &stat_buf);

			/*
			 * we will send a big jpeg file, but it could be
			 * anything.  Set the Content-Type: appropriately
			 * so the browser knows what to do with it.
			 */

			p += sprintf((char *)p,
				"HTTP/1.0 200 OK\x0d\x0a"
				"Server: libwebsockets\x0d\x0a"
				"Content-Type: image/jpeg\x0d\x0a"
					"Content-Length: %u\x0d\x0a\x0d\x0a",
					(unsigned int)stat_buf.st_size);

			/*
			 * send the http headers...
			 * this won't block since it's the first payload sent
			 * on the connection since it was established
			 * (too small for partial)
			 */

			n = libwebsocket_write(wsi, buffer,
				   p - buffer, LWS_WRITE_HTTP);

			if (n < 0) {
				close(pss->fd);
				return -1;
			}
			/*
			 * book us a LWS_CALLBACK_HTTP_WRITEABLE callback
			 */
			libwebsocket_callback_on_writable(context, wsi);
			break;
		}

		/* if not, send a file the easy way */

		for (n = 0; n < (sizeof(whitelist) / sizeof(whitelist[0]) - 1); n++)
			if (in && strcmp((const char *)in, whitelist[n].urlpath) == 0)
				break;

		sprintf(buf, "%s%s", resource_path, whitelist[n].urlpath);

		if (libwebsockets_serve_http_file(context, wsi, buf, whitelist[n].mimetype))
			return -1; /* through completion or error, close the socket */

		/*
		 * notice that the sending of the file completes asynchronously,
		 * we'll get a LWS_CALLBACK_HTTP_FILE_COMPLETION callback when
		 * it's done
		 */

		break;

	case LWS_CALLBACK_HTTP_FILE_COMPLETION:
//		lwsl_info("LWS_CALLBACK_HTTP_FILE_COMPLETION seen\n");
		/* kill the connection after we sent one file */
		return -1;

	case LWS_CALLBACK_HTTP_WRITEABLE:
		/*
		 * we can send more of whatever it is we were sending
		 */

		do {
			n = read(pss->fd, buffer, sizeof buffer);
			/* problem reading, close conn */
			if (n < 0)
				goto bail;
			/* sent it all, close conn */
			if (n == 0)
				goto bail;
			/*
			 * because it's HTTP and not websocket, don't need to take
			 * care about pre and postamble
			 */
			m = libwebsocket_write(wsi, buffer, n, LWS_WRITE_HTTP);
			if (m < 0)
				/* write failed, close conn */
				goto bail;
			if (m != n)
				/* partial write, adjust */
				lseek(pss->fd, m - n, SEEK_CUR);

		} while (!lws_send_pipe_choked(wsi));
		libwebsocket_callback_on_writable(context, wsi);
		break;

bail:
		close(pss->fd);
		return -1;

	/*
	 * callback for confirming to continue with client IP appear in
	 * protocol 0 callback since no websocket protocol has been agreed
	 * yet.  You can just ignore this if you won't filter on client IP
	 * since the default uhandled callback return is 0 meaning let the
	 * connection continue.
	 */

	case LWS_CALLBACK_FILTER_NETWORK_CONNECTION:
		libwebsockets_get_peer_addresses(context, wsi, (int)(long)in, client_name,
			     sizeof(client_name), client_ip, sizeof(client_ip));

		fprintf(stderr, "Received network connect from %s (%s)\n",
							client_name, client_ip);
		/* if we returned non-zero from here, we kill the connection */
		break;

	/*
	 * callbacks for managing the external poll() array appear in
	 * protocol 0 callback
	 */

	case LWS_CALLBACK_ADD_POLL_FD:

		if (count_pollfds >= max_poll_elements) {
			lwsl_err("LWS_CALLBACK_ADD_POLL_FD: too many sockets to track\n");
			return 1;
		}

		fd_lookup[fd] = count_pollfds;
		pollfds[count_pollfds].fd = fd;
		pollfds[count_pollfds].events = (int)(long)len;
		pollfds[count_pollfds++].revents = 0;
		break;

	case LWS_CALLBACK_DEL_POLL_FD:
		if (!--count_pollfds)
			break;
		m = fd_lookup[fd];
		/* have the last guy take up the vacant slot */
		pollfds[m] = pollfds[count_pollfds];
		fd_lookup[pollfds[count_pollfds].fd] = m;
		break;

	case LWS_CALLBACK_SET_MODE_POLL_FD:
		pollfds[fd_lookup[fd]].events |= (int)(long)len;
		break;

	case LWS_CALLBACK_CLEAR_MODE_POLL_FD:
		pollfds[fd_lookup[fd]].events &= ~(int)(long)len;
		break;

	default:
		break;
	}

	return 0;
}

/*
 * this is just an example of parsing handshake headers, you don't need this
 * in your code unless you will filter allowing connections by the header
 * content
 */

static void
dump_handshake_info(struct libwebsocket *wsi)
{
	int n;
	static const char *token_names[WSI_TOKEN_COUNT] = {
		/*[WSI_TOKEN_GET_URI]		=*/ "GET URI",
		/*[WSI_TOKEN_HOST]		=*/ "Host",
		/*[WSI_TOKEN_CONNECTION]	=*/ "Connection",
		/*[WSI_TOKEN_KEY1]		=*/ "key 1",
		/*[WSI_TOKEN_KEY2]		=*/ "key 2",
		/*[WSI_TOKEN_PROTOCOL]		=*/ "Protocol",
		/*[WSI_TOKEN_UPGRADE]		=*/ "Upgrade",
		/*[WSI_TOKEN_ORIGIN]		=*/ "Origin",
		/*[WSI_TOKEN_DRAFT]		=*/ "Draft",
		/*[WSI_TOKEN_CHALLENGE]		=*/ "Challenge",

		/* new for 04 */
		/*[WSI_TOKEN_KEY]		=*/ "Key",
		/*[WSI_TOKEN_VERSION]		=*/ "Version",
		/*[WSI_TOKEN_SWORIGIN]		=*/ "Sworigin",

		/* new for 05 */
		/*[WSI_TOKEN_EXTENSIONS]	=*/ "Extensions",

		/* client receives these */
		/*[WSI_TOKEN_ACCEPT]		=*/ "Accept",
		/*[WSI_TOKEN_NONCE]		=*/ "Nonce",
		/*[WSI_TOKEN_HTTP]		=*/ "Http",
		/*[WSI_TOKEN_MUXURL]	=*/ "MuxURL",
	};
	char buf[256];

	for (n = 0; n < WSI_TOKEN_COUNT; n++) {
		if (!lws_hdr_total_length(wsi, n))
			continue;

		lws_hdr_copy(wsi, buf, sizeof buf, n);

		fprintf(stderr, "    %s = %s\n", token_names[n], buf);
	}
}

/* dumb_increment protocol */

/*
 * one of these is auto-created for each connection and a pointer to the
 * appropriate instance is passed to the callback in the user parameter
 *
 * for this example protocol we use it to individualize the count for each
 * connection.
 */

struct per_session_data__dumb_increment {
	int number;
};

//static void get_client_notification()
//{
//	/* get non-exclusive (read) access to sessions_list (conns) */
//	if (pthread_rwlock_rdlock (&session_lock) != 0) {
//		ap_log_error (APLOG_MARK, APLOG_ERR, 0, server, "Error while locking rwlock: %d (%s)", errno, strerror(errno));
//		return NULL;
//	}
//	/* get session where send the RPC */
//	locked_session = (struct session_with_mutex *)apr_hash_get(conns, session_key, APR_HASH_KEY_STRING);
//	if (locked_session != NULL) {
//		session = locked_session->session;
//	}
//	if (session != NULL) {
//		/* get exclusive access to session */
//		if (pthread_mutex_lock(&locked_session->lock) != 0) {
//			/* unlock before returning error */
//			if (pthread_rwlock_unlock (&session_lock) != 0) {
//				ap_log_error (APLOG_MARK, APLOG_ERR, 0, server, "Error while locking rwlock: %d (%s)", errno, strerror(errno));
//				return NULL;
//			}
//			return NULL;
//		}
//		/* send the request and get the reply */
//		msgt = nc_session_send_recv(session, rpc, &reply);
//
//		/* first release exclusive lock for this session */
//		pthread_mutex_unlock(&locked_session->lock);
//		/* end of critical section */
//		if (pthread_rwlock_unlock (&session_lock) != 0) {
//			ap_log_error (APLOG_MARK, APLOG_ERR, 0, server, "Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
//			return (NULL);
//		}
//
//		/* process the result of the operation */
//		switch (msgt) {
//		case NC_MSG_UNKNOWN:
//			if (nc_session_get_status(session) != NC_SESSION_STATUS_WORKING) {
//				ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: receiving rpc-reply failed");
//				netconf_close(server, conns, session_key);
//				return (NULL);
//			}
//			/* no break */
//		case NC_MSG_NONE:
//			/* there is error handled by callback */
//			return (NULL);
//			break;
//		case NC_MSG_REPLY:
//			switch (replyt = nc_reply_get_type(reply)) {
//			case NC_REPLY_DATA:
//				if ((data = nc_reply_get_data (reply)) == NULL) {
//					ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: no data from reply");
//					data = NULL;
//				}
//				break;
//			default:
//				ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: unexpected rpc-reply (%d)", replyt);
//				data = NULL;
//				break;
//			}
//			break;
//		default:
//			ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "mod_netconf: unexpected reply message received (%d)", msgt);
//			data = NULL;
//			break;
//		}
//		nc_reply_free(reply);
//		return (data);
//	} else {
//		/* release lock on failure */
//		if (pthread_rwlock_unlock (&session_lock) != 0) {
//			ap_log_error (APLOG_MARK, APLOG_ERR, 0, server, "Error while unlocking rwlock: %d (%s)", errno, strerror(errno));
//		}
//		ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Unknown session to process.");
//		return (NULL);
//	}
//}

static int callback_notification(struct libwebsocket_context *context,
			struct libwebsocket *wsi,
			enum libwebsocket_callback_reasons reason,
					       void *user, void *in, size_t len)
{
	int isnotif = 0;
	int n, m;
	unsigned char buf[LWS_SEND_BUFFER_PRE_PADDING + 512 + LWS_SEND_BUFFER_POST_PADDING];
	unsigned char *p = &buf[LWS_SEND_BUFFER_PRE_PADDING];
	struct per_session_data__dumb_increment *pss = (struct per_session_data__dumb_increment *)user;
	#ifndef TEST_NOTIFICATION_SERVER
	if (http_server != NULL) {
		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, http_server, "libwebsockets callback_notification");
	}
	#endif

	switch (reason) {

	case LWS_CALLBACK_ESTABLISHED:
		lwsl_info("callback_notification: LWS_CALLBACK_ESTABLISHED\n");
		pss->number = 0;
		break;

	case LWS_CALLBACK_SERVER_WRITEABLE:
//		get_client_notification();
		n = sprintf((char *)p, "not%d\n", pss->number++);
		m = libwebsocket_write(wsi, p, n, LWS_WRITE_TEXT);
		if (m < n) {
			lwsl_err("ERROR %d writing to di socket\n", n);
			return -1;
		}
		if (close_testing && pss->number == 50) {
			lwsl_info("close tesing limit, closing\n");
			return -1;
		}
		break;

	case LWS_CALLBACK_RECEIVE:
//		fprintf(stderr, "rx %d\n", (int)len);
		if (len < 6)
			break;
		if (strcmp((const char *)in, "reset\n") == 0)
			pss->number = 0;
		break;
	/*
	 * this just demonstrates how to use the protocol filter. If you won't
	 * study and reject connections based on header content, you don't need
	 * to handle this callback
	 */

	case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
		//dump_handshake_info(wsi);
		/* you could return non-zero here and kill the connection */
		break;

	default:
		break;
	}

	return 0;
}

/* list of supported protocols and callbacks */

static struct libwebsocket_protocols protocols[] = {
	/* first protocol must always be HTTP handler */

	{
		"http-only",		/* name */
		callback_http,		/* callback */
		sizeof (struct per_session_data__http),	/* per_session_data_size */
		0,			/* max frame size / rx buffer */
	},
	{
		"notification-protocol",
		callback_notification,
		sizeof(struct per_session_data__dumb_increment),
		10,
	},
	{ NULL, NULL, 0, 0 } /* terminator */
};


int notification_init(apr_pool_t * pool, server_rec * server)
{
	char cert_path[1024];
	char key_path[1024];
	int use_ssl = 0;
	struct lws_context_creation_info info;
	int opts = 0;
	char interface_name[128] = "";
	const char *iface = NULL;
	int debug_level = 7;

	memset(&info, 0, sizeof info);
	info.port = NOTIFICATION_SERVER_PORT;

	/* tell the library what debug level to emit and to send it to syslog */
	lws_set_log_level(debug_level, lwsl_emit_syslog);

	#ifndef TEST_NOTIFICATION_SERVER
	if (server != NULL) {
		http_server = server;
		ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, http_server, "Initialization of libwebsocket");
	}
	#endif
	lwsl_notice("libwebsockets test server - "
			"(C) Copyright 2010-2013 Andy Green <andy@warmcat.com> - "
						    "licensed under LGPL2.1\n");
	max_poll_elements = getdtablesize();
	pollfds = malloc(max_poll_elements * sizeof (struct pollfd));
	fd_lookup = malloc(max_poll_elements * sizeof (int));
	if (pollfds == NULL || fd_lookup == NULL) {
		lwsl_err("Out of memory pollfds=%d\n", max_poll_elements);
		return -1;
	}

	info.iface = iface;
	info.protocols = protocols;

	//snprintf(cert_path, sizeof(cert_path), "%s/libwebsockets-test-server.pem", resource_path);
	//snprintf(key_path, sizeof(cert_path), "%s/libwebsockets-test-server.key.pem", resource_path);

	//info.ssl_cert_filepath = cert_path;
	//info.ssl_private_key_filepath = key_path;

	info.gid = -1;
	info.uid = -1;
	info.options = opts;

	/* create server */
	context = libwebsocket_create_context(&info);
	if (context == NULL) {
		lwsl_err("libwebsocket init failed\n");
		return -1;
	}
}

void notification_close()
{
	libwebsocket_context_destroy(context);

	lwsl_notice("libwebsockets-test-server exited cleanly\n");
}

/**
 * \brief send notification if any
 * \return < 0 on error
 */
int notification_handle()
{
	static struct timeval tv;
	static unsigned int olds = 0;
	int n = 0;

	gettimeofday(&tv, NULL);

	/*
	 * This provokes the LWS_CALLBACK_SERVER_WRITEABLE for every
	 * live websocket connection using the DUMB_INCREMENT protocol,
	 * as soon as it can take more packets (usually immediately)
	 */

	if (((unsigned int)tv.tv_sec - olds) > 0) {
		libwebsocket_callback_on_writable_all_protocol(&protocols[PROTOCOL_NOTIFICATION]);
		olds = tv.tv_sec;
	}


	/*
	 * this represents an existing server's single poll action
	 * which also includes libwebsocket sockets
	 */

	n = poll(pollfds, count_pollfds, 50);
	if (n < 0)
		return n;


	if (n) {
		for (n = 0; n < count_pollfds; n++) {
			if (pollfds[n].revents) {
				/*
				 * returns immediately if the fd does not
				 * match anything under libwebsockets
				 * control
				 */
				if (libwebsocket_service_fd(context, &pollfds[n]) < 0) {
					return 1;
				}
			}
		}
	}
	return 0;
}

#endif


#ifndef WITH_NOTIFICATIONS
#ifdef TEST_NOTIFICATION_SERVER
int main(int argc, char **argv)
{
//	char cert_path[1024];
//	char key_path[1024];
//	int n = 0;
//	int use_ssl = 0;
//	struct libwebsocket_context *context;
//	int opts = 0;
//	char interface_name[128] = "";
//	const char *iface = NULL;
//	unsigned int oldus = 0;
//	unsigned int olds = 0;
//	struct lws_context_creation_info info;
//
//	int debug_level = 7;
//
//	memset(&info, 0, sizeof info);
//	info.port = 8081;
//
//	/* tell the library what debug level to emit and to send it to syslog */
//	lws_set_log_level(debug_level, lwsl_emit_syslog);
//
//	lwsl_notice("libwebsockets test server - "
//			"(C) Copyright 2010-2013 Andy Green <andy@warmcat.com> - "
//						    "licensed under LGPL2.1\n");
//	max_poll_elements = getdtablesize();
//	pollfds = malloc(max_poll_elements * sizeof (struct pollfd));
//	fd_lookup = malloc(max_poll_elements * sizeof (int));
//	if (pollfds == NULL || fd_lookup == NULL) {
//		lwsl_err("Out of memory pollfds=%d\n", max_poll_elements);
//		return -1;
//	}
//
//	info.iface = iface;
//	info.protocols = protocols;
//
//	//snprintf(cert_path, sizeof(cert_path), "%s/libwebsockets-test-server.pem", resource_path);
//	//snprintf(key_path, sizeof(cert_path), "%s/libwebsockets-test-server.key.pem", resource_path);
//
//	//info.ssl_cert_filepath = cert_path;
//	//info.ssl_private_key_filepath = key_path;
//	//
//	info.gid = -1;
//	info.uid = -1;
//	info.options = opts;
//
//	/* create server */
//	context = libwebsocket_create_context(&info);
//	if (context == NULL) {
//		lwsl_err("libwebsocket init failed\n");
//		return -1;
//	}
//
//	n = 0;
//	while (n >= 0 && !force_exit) {
//		struct timeval tv;
//
//		gettimeofday(&tv, NULL);
//
//		/*
//		 * This provokes the LWS_CALLBACK_SERVER_WRITEABLE for every
//		 * live websocket connection using the DUMB_INCREMENT protocol,
//		 * as soon as it can take more packets (usually immediately)
//		 */
//
//		if (((unsigned int)tv.tv_sec - olds) > 0) {
//			libwebsocket_callback_on_writable_all_protocol(&protocols[PROTOCOL_NOTIFICATION]);
//			olds = tv.tv_sec;
//		}
//
//
//		/*
//		 * this represents an existing server's single poll action
//		 * which also includes libwebsocket sockets
//		 */
//
//		n = poll(pollfds, count_pollfds, 50);
//		if (n < 0)
//			continue;
//
//
//		if (n) {
//			for (n = 0; n < count_pollfds; n++) {
//				if (pollfds[n].revents) {
//					/*
//					* returns immediately if the fd does not
//					* match anything under libwebsockets
//					* control
//					*/
//					if (libwebsocket_service_fd(context, &pollfds[n]) < 0) {
//						break;
//					}
//				}
//			}
//		}
//	}
//
//	libwebsocket_context_destroy(context);
//
//	lwsl_notice("libwebsockets-test-server exited cleanly\n");
//
//	return 0;
	if (notification_init(NULL, NULL) == -1) {
		fprintf(stderr, "Error during initialization\n");
		return 1;
	}
	while (!force_exit) {
		notification_handle();
	}
	notification_close();
}
#endif
#endif