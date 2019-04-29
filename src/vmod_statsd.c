#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <cache/cache.h>
#include <vcl.h>

#ifndef VRT_H_INCLUDED
#include <vrt.h>
#endif

#ifndef VDEF_H_INCLUDED
#include <vdef.h>
#endif



#include "vcc_if.h"

/* Varnish < 6.2 compat */
#ifndef VPFX
  #define VPFX(a) vmod_ ## a
  #define VARGS(a) vmod_ ## a ## _arg
  #define VENUM(a) vmod_enum_ ## a
  #define VEVENT(a) a
#else
  #define VEVENT(a) VPFX(a)
#endif


// Socket related libraries
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>

#define BUF_SIZE 500


typedef struct statsdConfig {
	char *host;     // statsd host
	char *port;     // statsd port - as STRING
	char *prefix;   // prefix any key with this
	char *suffix;   // suffix any key with this
	int socket;     // open socket to the daemon
} config_t;


// ******************************
// Utility functions
// ******************************

// Unfortunately std.fileread() will append a newline, even if there is none in
// the file that was being read. So if you use that (as we suggest in the docs)
// to set prefix/suffix, you'll be in for a nasty surprise.
char *
_strip_newline( char *line ) {
    char *pos;

    if( (pos = strchr( line, '\n' )) != NULL ) {
        *pos = '\0';
    }

    if( (pos = strchr( line, '\r' )) != NULL ) {
        *pos = '\0';
    }



    return line;
}

// ******************************
// Configuration
// ******************************

static void
free_function(void *priv) {
    config_t *cfg = priv;
    if( cfg->socket > 0 ) {
        int close_ret = close( cfg->socket );
        if( close_ret != 0 ) {
            int close_error = errno;
            //VSLb(ctx->vsl, SLT_VCL_Log, "vmod-statsd: free: Error closing socket: %s (errno %d)\n",
            //                 strerror(close_error), close_error );
        }
        cfg->socket = 0;
    }
}

int
VEVENT(event_function)(VRT_CTX, struct VPFX(priv) *priv, enum vcl_event_e e) {

    if (e != VCL_EVENT_LOAD)
        return (0);

    // ******************************
    // Configuration defaults
    // ******************************

    config_t *cfg;
    cfg         = malloc(sizeof(config_t));
    cfg->host   = "localhost";
    cfg->port   = "8125";
    cfg->prefix = "";
    cfg->suffix = "";
    cfg->socket = 0;

    //VSLb(ctx->vsl, SLT_VCL_Log,"vmod-statsd: init: configuration intitialized");

    // ******************************
    // Store the config
    // ******************************

    priv->priv = cfg;
    priv->free = free_function;

	return (0);
}

/** The following may ONLY be called from VCL_init **/
VCL_VOID
VPFX(prefix)( VRT_CTX, struct VPFX(priv) *priv, const char *prefix ) {
    config_t *cfg = priv->priv;
    cfg->prefix = _strip_newline( strdup( prefix ) );
}

/** The following may ONLY be called from VCL_init **/
VCL_VOID
VPFX(suffix)( VRT_CTX, struct VPFX(priv) *priv, const char *suffix ) {

    config_t *cfg = priv->priv;
    cfg->suffix = _strip_newline( strdup( suffix ) );
}

/** The following may ONLY be called from VCL_init **/
VCL_VOID
VPFX(server)( VRT_CTX, struct VPFX(priv) *priv, VCL_STRING host, VCL_STRING port ) {

    // ******************************
    // Configuration
    // ******************************

    config_t *cfg = priv->priv;
    cfg->host   = strdup( host );
    cfg->port   = strdup( port );
}


// ******************************
// Connect to the remote socket
// ******************************

int
_connect_to_statsd( struct VPFX(priv) *priv , const struct vrt_ctx * ctx) {
    config_t *cfg = priv->priv;

    // Grab 2 structs for the connection
    struct addrinfo *statsd; /* will allocated by getaddrinfo */
    struct addrinfo *hints;
    hints = malloc(sizeof(struct addrinfo));

    if (hints == NULL) {
        fprintf( stderr, ""  );
        VSLb(ctx->vsl, SLT_VCL_Log,"vmod-statsd: malloc failed for hints addrinfo struct");
        return -1;
    }

    // Hints can be full of garbage, and it's lead to segfaults inside
    // Varnish. See this issue: https://github.com/jib/libvmod-statsd/pull/5/files
    // As a way to fix that, we zero out the memory, as also pointed out
    // in the freeaddrinfo manpage: http://linux.die.net/man/3/freeaddrinfo
    memset( hints, 0, sizeof(struct addrinfo) );

    // what type of socket is the statsd endpoint?
    hints->ai_family   = AF_UNSPEC;
    hints->ai_socktype = SOCK_DGRAM;
    hints->ai_protocol = IPPROTO_UDP;
    hints->ai_flags    = 0;


    // using getaddrinfo lets us use a hostname, rather than an
    // ip address.
    int err;
    if( (err = getaddrinfo( cfg->host, cfg->port, hints, &statsd )) != 0 ) {

        VSLb(ctx->vsl, SLT_VCL_Log,"vmod-statsd: getaddrinfo on %s:%s failed: %s", cfg->host, cfg->port, gai_strerror(err));
        freeaddrinfo( hints );
        return -1;
    }


    // ******************************
    // Store the open connection
    // ******************************

    // getaddrinfo() may return more than one address structure
    // but since this is UDP, we can't verify the connection
    // anyway, so we will just use the first one
    cfg->socket = socket( statsd->ai_family, statsd->ai_socktype,
                       statsd->ai_protocol );

    if( cfg->socket == -1 ) {
        VSLb(ctx->vsl, SLT_VCL_Log,"vmod-statsd: socket creation failed");
        close( cfg->socket );
        freeaddrinfo( statsd );
        freeaddrinfo( hints );
        return -1;
    }


    // connection failed.. for some reason...
    if( connect( cfg->socket, statsd->ai_addr, statsd->ai_addrlen ) == -1 ) {
        VSLb(ctx->vsl, SLT_VCL_Log,"vmod-statsd: socket conection failed");
        close( cfg->socket );

        freeaddrinfo( statsd );
        freeaddrinfo( hints );
        return -1;
    }


    // now that we have an outgoing socket, we don't need this
    // anymore.
    freeaddrinfo( statsd );
    freeaddrinfo( hints );


    VSLb(ctx->vsl, SLT_VCL_Log,"vmod-statsd: statsd server: %s:%s (fd: %d)", cfg->host, cfg->port, cfg->socket );
    return cfg->socket;
}

int
_send_to_statsd( struct VPFX(priv) *priv, const char *key, const char *val, VRT_CTX) {
    config_t *cfg = priv->priv;


    // If you are using some empty key, bail - this can happen if you use
    // say: statsd.incr( req.http.x-does-not-exist ). Rather than getting
    // and empty string, we get a null pointer.
    if( key == NULL || val == NULL ) {
        VSLb( ctx->vsl, SLT_VCL_Log,  "vmod-statsd: Key or value is NULL pointer - ignoring" );
        return -1;
    }


    // Enough room for the key/val plus prefix/suffix plus newline plus a null byte.
    char stat[ strlen(key) + strlen(val) +
               strlen(cfg->prefix) + strlen(cfg->suffix) + 1 ];

    strncpy( stat, cfg->prefix, strlen(cfg->prefix) + 1 );
    strncat( stat, key,         strlen(key)         + 1 );
    strncat( stat, cfg->suffix, strlen(cfg->suffix) + 1 );
    strncat( stat, val,         strlen(val)         + 1 );


    // Newer versions of statsd allow multiple metrics in a single packet, delimited
    // by newlines. That unfortunately means that if we end our message with a new
    // line, statsd will interpret this as an empty second metric and log a 'bad line'.
    // This is true in at least version 0.5.0 and to avoid that, we don't send the
    // newline. Makes debugging using nc -klu 8125 a bit more tricky, but works with
    // modern statsds.
    //strncat( stat, "\n",        1                       );

    //VSLb(ctx->vsl, SLT_VCL_Log, "vmod-statsd: send: %s:%s %s\n", cfg->host, cfg->port, stat );

    // ******************************
    // Sanity checks
    // ******************************


    int len = strlen( stat );


    // +1 for the null byte
    if( len + 1 >= BUF_SIZE ) {
        VSLb( ctx->vsl, SLT_VCL_Log, "vmod-statsd: Message length %d > max length %d - ignoring",
            len, BUF_SIZE );
        return -1;
    }

    // ******************************
    // Send the packet
    // ******************************


    // we may not have connected yet - in that case, do it now
    int sock = cfg->socket > 0 ? cfg->socket : _connect_to_statsd( priv, ctx );

    // If we didn't get a socket, don't bother trying to send
    if( sock == -1 ) {
        VSLb( ctx->vsl, SLT_VCL_Log, "vmod-statsd: Could not get socket for %s", stat );
        return -1;
    }

    // Send the stat
    int sent = write( sock, stat, len );

    VSLb(ctx->vsl, SLT_VCL_Log, "vmod-statsd: Sent %d of %d bytes to FD %d", sent, len, sock );

    // An error occurred - unset the socket so that the next write may try again
    if( sent != len ) {
        int write_error = errno;
        VSLb(ctx->vsl, SLT_VCL_Log, "vmod-statsd: Could not write stat '%s': %s (errno %d)",
                         stat, strerror(write_error), write_error );

        // if the write_error is not due to a bad file descriptor, try to close the socket first
        if( write_error != 9 ) {
            int close_ret_val = close( sock );
            if( close_ret_val != 0 ) {
                int close_error = errno;
                VSLb(ctx->vsl, SLT_VCL_Log, "vmod-statsd: Error closing socket: %s (errno %d)",
                                 strerror(close_error), close_error );
            }
        }
        // reset the socket
        cfg->socket = 0;
        VSLb(ctx->vsl, SLT_VCL_Log, "vmod-statsd: Socket closed/reset" );

        return -1;
    }

    return 0;
}


VCL_VOID
VPFX(incr)( VRT_CTX, struct VPFX(priv) *priv, VCL_STRING key ) {
    VSLb(ctx->vsl, SLT_VCL_Log, "vmod-statsd: incr: %s", key );

    // Incremenet is straight forward - just add the count + type
    _send_to_statsd( priv, key, ":1|c" , ctx);
}

VCL_VOID
VPFX(timing)( VRT_CTX, struct VPFX(priv) *priv, const char *key, VCL_INT num ) {
    VSLb(ctx->vsl, SLT_VCL_Log, "vmod-statsd: timing: %s = %d", key, num );

    // Get the buffer ready. 10 for the maximum lenghth of an int and +5 for metadata
    char val[ 15 ];

    // looks like glork:320|ms
    snprintf( val, sizeof(val), ":%d|ms", num );

    _send_to_statsd( priv, key, val , ctx);
}

VCL_VOID
VPFX(counter)( VRT_CTX, struct VPFX(priv) *priv, const char *key, VCL_INT num ) {
    VSLb(ctx->vsl, SLT_VCL_Log, "vmod-statsd: counter: %s = %d", key, num );

    // Get the buffer ready. 10 for the maximum lenghth of an int and +5 for metadata
    char val[ 15 ];

    // looks like: gorets:42|c
    snprintf( val, sizeof(val), ":%d|c", num );

    _send_to_statsd( priv, key, val , ctx);
}

VCL_VOID
VPFX(gauge)( VRT_CTX, struct VPFX(priv) *priv, const char *key, VCL_INT num ) {
    VSLb(ctx->vsl, SLT_VCL_Log, "vmod-statsd: gauge: %s = %d", key, num );

    // Get the buffer ready. 10 for the maximum lenghth of an int and +5 for metadata
    char val[ 15 ];

    // looks like: gaugor:333|g
    snprintf( val, sizeof(val), ":%d|g", num );

    _send_to_statsd( priv, key, val, ctx );
}

