/* Copyright (C) 2007-2008 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/
/*
 *  Android emulator control console
 *
 *  this console is enabled automatically at emulator startup, on port 5554 by default,
 *  unless some other emulator is already running. See (android_emulation_start in android_sdl.c
 *  for details)
 *
 *  you can telnet to the console, then use commands like 'help' or others to dynamically
 *  change emulator settings.
 *
 */

#include "android/sockets.h"
#include "sysemu/char.h"
#include "sysemu/sysemu.h"
#include "android/android.h"
#include "cpu.h"
#include "hw/android/goldfish/bt.h"
#include "hw/android/goldfish/device.h"
#include "hw/android/goldfish/nfc.h"
#include "hw/nfc/llcp.h"
#include "hw/nfc/re.h"
#include "hw/nfc/ndef.h"
#include "hw/nfc/nfc.h"
#include "hw/nfc/nci.h"
#include "hw/nfc/snep.h"
#include "hw/nfc/tag.h"
#include "hw/power_supply.h"
#include "android/shaper.h"
#include "modem_driver.h"
#include "android/gps.h"
#include "android/globals.h"
#include "android/utils/bufprint.h"
#include "android/utils/debug.h"
#include "android/utils/eintr_wrapper.h"
#include "android/utils/http_utils.h"
#include "android/utils/stralloc.h"
#include "android/utils/utf8_utils.h"
#include "android/config/config.h"
#include "android/tcpdump.h"
#include "net/net.h"
#include "monitor/monitor.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "android/base64.h"
#include "android/hw-events.h"
#include "android/user-events.h"
#include "android/hw-fingerprint.h"
#include "android/hw-sensors.h"
#include "android/skin/charmap.h"
#include "android/skin/keycode-buffer.h"
#include "android/display-core.h"

#if defined(CONFIG_SLIRP)
#include "libslirp.h"
#endif

extern void android_emulator_set_window_scale(double, int);

#define  DEBUG  1

#if 1
#  define  D_ACTIVE   VERBOSE_CHECK(console)
#else
#  define  D_ACTIVE   DEBUG
#endif

#if DEBUG
#  define  D(x)   do { if (D_ACTIVE) ( printf x , fflush(stdout) ); } while (0)
#else
#  define  D(x)   do{}while(0)
#endif

typedef struct ControlGlobalRec_*  ControlGlobal;

typedef struct ControlClientRec_*  ControlClient;

typedef struct {
    int           host_port;
    int           host_udp;
    unsigned int  guest_ip;
    int           guest_port;
} RedirRec, *Redir;


typedef int Socket;

typedef struct ControlClientRec_
{
    struct ControlClientRec_*  next;       /* next client in list           */
    Socket                     sock;       /* socket used for communication */
    ControlGlobal              global;
    char                       finished;
    char                       buff[ 4096 ];
    int                        buff_len;

    /**
     * Currently referred modem device. Each control client have their own
     * modem|bt device pointer to the one it's referring to. This pointer might
     * be NULL so every command handler should check its validity every time
     * referring to it. Its value is only modified by 'mux <type> <id>'.
     */
    AModem                     modem;
    ABluetooth                 bt;
} ControlClientRec;


typedef struct ControlGlobalRec_
{
    /* listening socket */
    Socket    listen_fd;

    /* the list of current clients */
    ControlClient   clients;

    /* the list of redirections currently active */
    Redir     redirs;
    int       num_redirs;
    int       max_redirs;

} ControlGlobalRec;

#ifdef CONFIG_STANDALONE_CORE
/* UI client currently attached to the core. */
ControlClient attached_ui_client = NULL;

/* User events service client. */
ControlClient user_events_client = NULL;

/* UI control service client (UI -> Core). */
ControlClient ui_core_ctl_client = NULL;

/* UI control service (UI -> Core. */
// CoreUICtl* ui_core_ctl = NULL;

/* UI control service client (Core-> UI). */
ControlClient core_ui_ctl_client = NULL;
#endif  // CONFIG_STANDALONE_CORE

static int
control_global_add_redir( ControlGlobal  global,
                          int            host_port,
                          int            host_udp,
                          unsigned int   guest_ip,
                          int            guest_port )
{
    Redir  redir;

    if (global->num_redirs >= global->max_redirs)
    {
        int  old_max = global->max_redirs;
        int  new_max = old_max + (old_max >> 1) + 4;

        Redir  new_redirs = realloc( global->redirs, new_max*sizeof(global->redirs[0]) );
        if (new_redirs == NULL)
            return -1;

        global->redirs     = new_redirs;
        global->max_redirs = new_max;
    }

    redir = &global->redirs[ global->num_redirs++ ];

    redir->host_port  = host_port;
    redir->host_udp   = host_udp;
    redir->guest_ip   = guest_ip;
    redir->guest_port = guest_port;

    return 0;
}

static int
control_global_del_redir( ControlGlobal  global,
                          int            host_port,
                          int            host_udp )
{
    int  nn;

    for (nn = 0; nn < global->num_redirs; nn++)
    {
        Redir  redir = &global->redirs[nn];

        if ( redir->host_port == host_port &&
             redir->host_udp  == host_udp  )
        {
            memmove( redir, redir + 1, ((global->num_redirs - nn)-1)*sizeof(*redir) );
            global->num_redirs -= 1;
            return 0;
        }
    }
    /* we didn't find it */
    return -1;
}

/* Detach the socket descriptor from a given ControlClient
 * and return its value. This is useful either when destroying
 * the client, or redirecting the socket to another service.
 *
 * NOTE: this does not close the socket.
 */
static int
control_client_detach( ControlClient  client )
{
    int  result;

    if (client->sock < 0)
        return -1;

    qemu_set_fd_handler( client->sock, NULL, NULL, NULL );
    result = client->sock;
    client->sock = -1;

    return result;
}

static void  control_client_read( void*  _client );  /* forward */

static void
control_client_destroy( ControlClient  client )
{
    ControlGlobal  global = client->global;
    ControlClient  *pnode = &global->clients;
    int            sock;

    D(( "destroying control client %p\n", client ));

#ifdef CONFIG_STANDALONE_CORE
    if (client == attached_ui_client) {
        attachUiProxy_destroy();
        attached_ui_client = NULL;
    }

    if (client == user_events_client) {
        userEventsImpl_destroy();
        user_events_client = NULL;
    }

    if (client == ui_core_ctl_client) {
        coreCmdImpl_destroy();
        ui_core_ctl_client = NULL;
    }

    if (client == core_ui_ctl_client) {
        uiCmdProxy_destroy();
        core_ui_ctl_client = NULL;
    }
#endif  // CONFIG_STANDALONE_CORE

    sock = control_client_detach( client );
    if (sock >= 0)
        socket_close(sock);

    for ( ;; ) {
        ControlClient  node = *pnode;
        if ( node == NULL )
            break;
        if ( node == client ) {
            *pnode     = node->next;
            node->next = NULL;
            break;
        }
        pnode = &node->next;
    }

    free( client );
}



static void  control_control_write( ControlClient  client, const char*  buff, int  len )
{
    int ret;

    if (len < 0)
        len = strlen(buff);

    while (len > 0) {
        ret = HANDLE_EINTR(socket_send( client->sock, buff, len));
        if (ret < 0) {
            if (errno != EWOULDBLOCK && errno != EAGAIN)
                return;
        } else {
            buff += ret;
            len  -= ret;
        }
    }
}

static int  control_vwrite( ControlClient  client, const char*  format, va_list args )
{
    static char  temp[1024];
    int ret = vsnprintf( temp, sizeof(temp), format, args );
    temp[ sizeof(temp)-1 ] = 0;
    control_control_write( client, temp, -1 );

    return ret;
}

static int  control_write( ControlClient  client, const char*  format, ... )
{
    int ret;
    va_list      args;
    va_start(args, format);
    ret = control_vwrite(client, format, args);
    va_end(args);

    return ret;
}


static ControlClient
control_client_create( Socket         socket,
                       ControlGlobal  global )
{
    ControlClient  client = calloc( sizeof(*client), 1 );

    if (client) {
        socket_set_nodelay( socket );
        socket_set_nonblock( socket );
        client->finished = 0;
        client->global  = global;
        client->sock    = socket;
        client->next    = global->clients;

        client->modem   = amodem_get_instance(0);
        client->bt      = abluetooth_get_instance(0);

        global->clients = client;

        qemu_set_fd_handler( socket, control_client_read, NULL, client );
    }
    return client;
}

typedef const struct CommandDefRec_  *CommandDef;

typedef struct CommandDefRec_ {
    const char*  names;
    const char*  abstract;
    const char*  description;
    void        (*descriptor)( ControlClient  client );
    int         (*handler)( ControlClient  client, char* args );
    CommandDef   subcommands;   /* if handler is NULL */

} CommandDefRec;

static const CommandDefRec   main_commands[];  /* forward */

static CommandDef
find_command( char*  input, CommandDef  commands, char*  *pend, char*  *pargs )
{
    int    nn;
    char*  args = strchr(input, ' ');

    if (args != NULL) {
        while (*args == ' ')
            args++;

        if (args[0] == 0)
            args = NULL;
    }

    for (nn = 0; commands[nn].names != NULL; nn++)
    {
        const char*  name = commands[nn].names;
        const char*  sep;

        do {
            int  len, c;

            sep = strchr( name, '|' );
            if (sep)
                len = sep - name;
            else
                len = strlen(name);

            c = input[len];
            if ( !memcmp( name, input, len ) && (c == ' ' || c == 0) ) {
                *pend  = input + len;
                *pargs = args;
                return &commands[nn];
            }

            if (sep)
                name = sep + 1;

        } while (sep != NULL && *name);
    }
    /* NOTE: don't touch *pend and *pargs if no command is found */
    return NULL;
}

static void
dump_help( ControlClient  client,
           CommandDef     cmd,
           const char*    prefix )
{
    if (cmd->description) {
        control_write( client, "%s", cmd->description );
    } else if (cmd->descriptor) {
        cmd->descriptor( client );
    } else
        control_write( client, "%s\r\n", cmd->abstract );

    if (cmd->subcommands) {
        cmd = cmd->subcommands;
        control_write( client, "\r\navailable sub-commands:\r\n" );
        for ( ; cmd->names != NULL; cmd++ ) {
            control_write( client, "   %s %-15s  %s\r\n", prefix, cmd->names, cmd->abstract );
        }
        control_write( client, "\r\n" );
    }
}

static int do_quit(ControlClient client, char* args);  // forward

static void
control_client_do_command( ControlClient  client )
{
    char*       line     = client->buff;
    char*       args     = NULL;
    CommandDef  commands = main_commands;
    char*       cmdend   = client->buff;
    CommandDef  cmd      = find_command( line, commands, &cmdend, &args );

    if (cmd == NULL) {
        size_t line_len = strlen(line);
        if (android_http_is_request_line(line, line_len)) {
            control_write( client, "KO: Forbidden HTTP request. Aborting\r\n");
            do_quit(client, NULL);
        } else  if (!android_utf8_is_valid(line, line_len)) {
            control_write( client, "KO: Forbidden binary request. Aborting\r\n");
            do_quit(client, NULL);
        } else {
            control_write( client, "KO: unknown command, try 'help'\r\n" );
        }
        return;
    }

    for (;;) {
        CommandDef  subcmd;

        if (cmd->handler) {
            if ( !cmd->handler( client, args ) ) {
                control_write( client, "OK\r\n" );
            }
            break;
        }

        /* no handler means we should have sub-commands */
        if (cmd->subcommands == NULL) {
            control_write( client, "KO: internal error: buggy command table for '%.*s'\r\n",
                           cmdend - client->buff, client->buff );
            break;
        }

        /* we need a sub-command here */
        if ( !args ) {
            dump_help( client, cmd, "" );
            control_write( client, "KO: missing sub-command\r\n" );
            break;
        }

        line     = args;
        commands = cmd->subcommands;
        subcmd   = find_command( line, commands, &cmdend, &args );
        if (subcmd == NULL) {
            dump_help( client, cmd, "" );
            control_write( client, "KO:  bad sub-command\r\n" );
            break;
        }
        cmd = subcmd;
    }
}

/* implement the 'help' command */
static int
do_help( ControlClient  client, char*  args )
{
    char*       line;
    char*       start = args;
    char*       end   = start;
    CommandDef  cmd = main_commands;

    /* without arguments, simply dump all commands */
    if (args == NULL) {
        control_write( client, "Android console command help:\r\n\r\n" );
        for ( ; cmd->names != NULL; cmd++ ) {
            control_write( client, "    %-15s  %s\r\n", cmd->names, cmd->abstract );
        }
        control_write( client, "\r\ntry 'help <command>' for command-specific help\r\n" );
        return 0;
    }

    /* with an argument, find the corresponding command */
    for (;;) {
        CommandDef  subcmd;

        line    = args;
        subcmd  = find_command( line, cmd, &end, &args );
        if (subcmd == NULL) {
            control_write( client, "try one of these instead:\r\n\r\n" );
            for ( ; cmd->names != NULL; cmd++ ) {
                control_write( client, "    %.*s %s\r\n",
                              end - start, start, cmd->names );
            }
            control_write( client, "\r\nKO: unknown command\r\n" );
            return -1;
        }

        if ( !args || !subcmd->subcommands ) {
            dump_help( client, subcmd, start );
            return 0;
        }
        cmd = subcmd->subcommands;
    }
}


static void
control_client_read_byte( ControlClient  client, unsigned char  ch )
{
    if (ch == '\r')
    {
        /* filter them out */
    }
    else if (ch == '\n')
    {
        client->buff[ client->buff_len ] = 0;
        control_client_do_command( client );
        if (client->finished)
            return;

        client->buff_len = 0;
    }
    else
    {
        if (client->buff_len >= sizeof(client->buff)-1)
            client->buff_len = 0;

        client->buff[ client->buff_len++ ] = ch;
    }
}

static void
control_client_read( void*  _client )
{
    ControlClient  client = _client;
    unsigned char  buf[4096];
    int            size;

    D(( "in control_client read: " ));
    size = socket_recv( client->sock, buf, sizeof(buf) );
    if (size < 0) {
        D(( "size < 0, exiting with %d: %s\n", errno, errno_str ));
        if (errno != EWOULDBLOCK && errno != EAGAIN)
            control_client_destroy( client );
        return;
    }

    if (size == 0) {
        /* end of connection */
        D(( "end of connection detected !!\n" ));
        control_client_destroy( client );
    }
    else {
        int  nn;
#ifdef _WIN32
#  if DEBUG
        char  temp[16];
        int   count = size > sizeof(temp)-1 ? sizeof(temp)-1 : size;
        for (nn = 0; nn < count; nn++) {
                int  c = buf[nn];
                if (c == '\n')
                        temp[nn] = '!';
            else if (c < 32)
                        temp[nn] = '.';
                else
                    temp[nn] = (char)c;
        }
        temp[nn] = 0;
        D(( "received %d bytes: %s\n", size, temp ));
#  endif
#else
        D(( "received %.*s\n", size, buf ));
#endif
        for (nn = 0; nn < size; nn++) {
            control_client_read_byte( client, buf[nn] );
            if (client->finished) {
                control_client_destroy(client);
                return;
            }
        }
    }
}


/* this function is called on each new client connection */
static void
control_global_accept( void*  _global )
{
    ControlGlobal       global = _global;
    ControlClient       client;
    Socket              fd;

    D(( "control_global_accept: just in (fd=%d)\n", global->listen_fd ));

    fd = HANDLE_EINTR(socket_accept(global->listen_fd, NULL));
    if (fd < 0) {
        D(( "problem in accept: %d: %s\n", errno, errno_str ));
        perror("accept");
        return;
    }

    socket_set_xreuseaddr( fd );

    D(( "control_global_accept: creating new client\n" ));
    client = control_client_create( fd, global );
    if (client) {
        D(( "control_global_accept: new client %p\n", client ));
        control_write( client, "Android Console: type 'help' for a list of commands\r\n" );
        control_write( client, "OK\r\n" );
    }
}


static int
control_global_init( ControlGlobal  global,
                     int            control_port )
{
    Socket  fd;
    int     ret;
    SockAddress  sockaddr;

    memset( global, 0, sizeof(*global) );

    fd = socket_create_inet( SOCKET_STREAM );
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    socket_set_xreuseaddr( fd );

    sock_address_init_inet( &sockaddr, SOCK_ADDRESS_INET_LOOPBACK, control_port );

    ret = socket_bind(fd, &sockaddr );
    if (ret < 0) {
        perror("bind");
        socket_close( fd );
        return -1;
    }

    ret = socket_listen(fd, 0);
    if (ret < 0) {
        perror("listen");
        socket_close( fd );
        return -1;
    }

    socket_set_nonblock(fd);

    global->listen_fd = fd;

    qemu_set_fd_handler( fd, control_global_accept, NULL, global );
    return 0;
}



static int
do_quit( ControlClient  client, char*  args )
{
    client->finished = 1;
    return -1;
}

/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                        N E T W O R K   S E T T I N G S                          ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static int
do_network_status( ControlClient  client, char*  args )
{
    control_write( client, "Current network status:\r\n" );

    control_write( client, "  download speed:   %8d bits/s (%.1f KB/s)\r\n",
                   (long)qemu_net_download_speed, qemu_net_download_speed/8192. );

    control_write( client, "  upload speed:     %8d bits/s (%.1f KB/s)\r\n",
                   (long)qemu_net_upload_speed, qemu_net_upload_speed/8192. );

    control_write( client, "  minimum latency:  %ld ms\r\n", qemu_net_min_latency );
    control_write( client, "  maximum latency:  %ld ms\r\n", qemu_net_max_latency );
    return 0;
}

static void
dump_network_speeds( ControlClient  client )
{
    const NetworkSpeed*  speed = android_netspeeds;
    const char* const  format = "  %-8s %s\r\n";
    for ( ; speed->name; speed++ ) {
        control_write( client, format, speed->name, speed->display );
    }
    control_write( client, format, "<num>", "selects both upload and download speed" );
    control_write( client, format, "<up>:<down>", "select individual upload/download speeds" );
}


static int
do_network_speed( ControlClient  client, char*  args )
{
    if ( !args ) {
        control_write( client, "KO: missing <speed> argument, see 'help network speed'\r\n" );
        return -1;
    }
    if ( android_parse_network_speed( args ) < 0 ) {
        control_write( client, "KO: invalid <speed> argument, see 'help network speed' for valid values\r\n" );
        return -1;
    }

    netshaper_set_rate( slirp_shaper_in,  qemu_net_download_speed );
    netshaper_set_rate( slirp_shaper_out, qemu_net_upload_speed );

    if (client->modem) {
        amodem_set_data_network_type( client->modem,
                                    android_parse_network_type( args ) );
    }
    return 0;
}

static void
describe_network_speed( ControlClient  client )
{
    control_write( client,
                   "'network speed <speed>' allows you to dynamically change the speed of the emulated\r\n"
                   "network on the device, where <speed> is one of the following:\r\n\r\n" );
    dump_network_speeds( client );
}

static int
do_network_delay( ControlClient  client, char*  args )
{
    if ( !args ) {
        control_write( client, "KO: missing <delay> argument, see 'help network delay'\r\n" );
        return -1;
    }
    if ( android_parse_network_latency( args ) < 0 ) {
        control_write( client, "KO: invalid <delay> argument, see 'help network delay' for valid values\r\n" );
        return -1;
    }
    netdelay_set_latency( slirp_delay_in, qemu_net_min_latency, qemu_net_max_latency );
    return 0;
}

static void
describe_network_delay( ControlClient  client )
{
    control_write( client,
                   "'network delay <latency>' allows you to dynamically change the latency of the emulated\r\n"
                   "network on the device, where <latency> is one of the following:\r\n\r\n" );
    /* XXX: TODO */
}

static int
do_network_capture_start( ControlClient  client, char*  args )
{
    if ( !args ) {
        control_write( client, "KO: missing <file> argument, see 'help network capture start'\r\n" );
        return -1;
    }
    if ( qemu_tcpdump_start(args) < 0) {
        control_write( client, "KO: could not start capture: %s", strerror(errno) );
        return -1;
    }
    return 0;
}

static int
do_network_capture_stop( ControlClient  client, char*  args )
{
    /* no need to return an error here */
    qemu_tcpdump_stop();
    return 0;
}

static const CommandDefRec  network_capture_commands[] =
{
    { "start", "start network capture",
      "'network capture start <file>' starts a new capture of network packets\r\n"
      "into a specific <file>. This will stop any capture already in progress.\r\n"
      "the capture file can later be analyzed by tools like WireShark. It uses\r\n"
      "the libpcap file format.\r\n\r\n"
      "you can stop the capture anytime with 'network capture stop'\r\n", NULL,
      do_network_capture_start, NULL },

    { "stop", "stop network capture",
      "'network capture stop' stops a currently running packet capture, if any.\r\n"
      "you can start one with 'network capture start <file>'\r\n", NULL,
      do_network_capture_stop, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

static const CommandDefRec  network_commands[] =
{
    { "status", "dump network status", NULL, NULL,
       do_network_status, NULL },

    { "speed", "change network speed", NULL, describe_network_speed,
      do_network_speed, NULL },

    { "delay", "change network latency", NULL, describe_network_delay,
       do_network_delay, NULL },

    { "capture", "dump network packets to file",
      "allows to start/stop capture of network packets to a file for later analysis\r\n", NULL,
      NULL, network_capture_commands },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                       P O R T   R E D I R E C T I O N S                         ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static int
do_redir_list( ControlClient  client, char*  args )
{
    ControlGlobal  global = client->global;

    if (global->num_redirs == 0)
        control_write( client, "no active redirections\r\n" );
    else {
        int  nn;
        for (nn = 0; nn < global->num_redirs; nn++) {
            Redir  redir = &global->redirs[nn];
            control_write( client, "%s:%-5d => %-5d\r\n",
                          redir->host_udp ? "udp" : "tcp",
                          redir->host_port,
                          redir->guest_port );
        }
    }
    return 0;
}

/* parse a protocol:port specification */
static int
redir_parse_proto_port( char*  args, int  *pport, int  *pproto )
{
    int  proto = -1;
    int  len   = 0;
    char*  end;

    if ( !memcmp( args, "tcp:", 4 ) ) {
        proto = 0;
        len   = 4;
    }
    else if ( !memcmp( args, "udp:", 4 ) ) {
        proto = 1;
        len   = 4;
    }
    else
        return 0;

    args   += len;
    *pproto = proto;
    *pport  = strtol( args, &end, 10 );
    if (end == args)
        return 0;

    len += end - args;
    return len;
}

static int
redir_parse_guest_port( char*  arg, int  *pport )
{
    char*  end;

    *pport = strtoul( arg, &end, 10 );
    if (end == arg)
        return 0;

    return end - arg;
}

static Redir
redir_find( ControlGlobal  global, int  port, int  isudp )
{
    int  nn;

    for (nn = 0; nn < global->num_redirs; nn++) {
        Redir  redir = &global->redirs[nn];

        if (redir->host_port == port && redir->host_udp == isudp)
            return redir;
    }
    return NULL;
}


static int
do_redir_add( ControlClient  client, char*  args )
{
    int       len, host_proto, host_port, guest_port;
    uint32_t  guest_ip;
    Redir     redir;

    if ( !args )
        goto BadFormat;

    if (!slirp_is_inited()) {
        control_write( client, "KO: network emulation disabled\r\n");
        return -1;
    }

    len = redir_parse_proto_port( args, &host_port, &host_proto );
    if (len == 0 || args[len] != ':')
        goto BadFormat;

    args += len + 1;
    len = redir_parse_guest_port( args, &guest_port );
    if (len == 0 || args[len] != 0)
        goto BadFormat;

    redir = redir_find( client->global, host_port, host_proto );
    if ( redir != NULL ) {
        control_write( client, "KO: host port already active, use 'redir del' to remove first\r\n" );
        return -1;
    }

    if (inet_strtoip("10.0.2.15", &guest_ip) < 0) {
        control_write( client, "KO: unexpected internal failure when resolving 10.0.2.15\r\n" );
        return -1;
    }

    D(("pattern hport=%d gport=%d proto=%d\n", host_port, guest_port, host_proto ));
    if ( control_global_add_redir( client->global, host_port, host_proto,
                                   guest_ip, guest_port ) < 0 )
    {
        control_write( client, "KO: not enough memory to allocate redirection\r\n" );
        return -1;
    }

    if (slirp_redir(host_proto, host_port, guest_ip, guest_port) < 0) {
        control_write( client, "KO: can't setup redirection, port probably used by another program on host\r\n" );
        control_global_del_redir( client->global, host_port, host_proto );
        return -1;
    }

    return 0;

BadFormat:
    control_write( client, "KO: bad redirection format, try (tcp|udp):hostport:guestport\r\n", -1 );
    return -1;
}


static int
do_redir_del( ControlClient  client, char*  args )
{
    int    len, proto, port;
    Redir  redir;

    if ( !args )
        goto BadFormat;
    len = redir_parse_proto_port( args, &port, &proto );
    if ( len == 0 || args[len] != 0 )
        goto BadFormat;

    redir = redir_find( client->global, port, proto );
    if (redir == NULL) {
        control_write( client, "KO: can't remove unknown redirection (%s:%d)\r\n",
                        proto ? "udp" : "tcp", port );
        return -1;
    }

    slirp_unredir( redir->host_udp, redir->host_port );
    control_global_del_redir( client->global, port, proto );\

    return 0;

BadFormat:
    control_write( client, "KO: bad redirection format, try (tcp|udp):hostport\r\n" );
    return -1;
}

static const CommandDefRec  redir_commands[] =
{
    { "list", "list current redirections",
    "list current port redirections. use 'redir add' and 'redir del' to add and remove them\r\n", NULL,
    do_redir_list, NULL },

    { "add",  "add new redirection",
    "add a new port redirection, arguments must be:\r\n\r\n"
            "  redir add <protocol>:<host-port>:<guest-port>\r\n\r\n"
            "where:   <protocol>     is either 'tcp' or 'udp'\r\n"
            "         <host-port>    a number indicating which port on the host to open\r\n"
            "         <guest-port>   a number indicating which port to route to on the device\r\n"
            "\r\nas an example, 'redir  tcp:5000:6000' will allow any packets sent to\r\n"
            "the host's TCP port 5000 to be routed to TCP port 6000 of the emulated device\r\n", NULL,
    do_redir_add, NULL },

    { "del",  "remove existing redirection",
    "remove a port redirecion that was created with 'redir add', arguments must be:\r\n\r\n"
            "  redir  del <protocol>:<host-port>\r\n\r\n"
            "see the 'help redir add' for the meaning of <protocol> and <host-port>\r\n", NULL,
    do_redir_del, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};



/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                          C D M A   M O D E M                                    ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static const struct {
    const char *            name;
    const char *            display;
    ACdmaSubscriptionSource source;
} _cdma_subscription_sources[] = {
    { "nv",            "Read subscription from non-volatile RAM", A_SUBSCRIPTION_NVRAM },
    { "ruim",          "Read subscription from RUIM", A_SUBSCRIPTION_RUIM },
};

static void
dump_subscription_sources( ControlClient client )
{
    int i;
    for (i = 0;
         i < sizeof(_cdma_subscription_sources) / sizeof(_cdma_subscription_sources[0]);
         i++) {
        control_write( client, "    %s: %s\r\n",
                       _cdma_subscription_sources[i].name,
                       _cdma_subscription_sources[i].display );
    }
}

static void
describe_subscription_source( ControlClient client )
{
    control_write( client,
                   "'cdma ssource <ssource>' allows you to specify where to read the subscription from\r\n" );
    dump_subscription_sources( client );
}

static int
do_cdma_ssource( ControlClient  client, char*  args )
{
    int nn;
    if (!args) {
        control_write( client, "KO: missing argument, try 'cdma ssource <source>'\r\n" );
        return -1;
    }

    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }

    for (nn = 0; ; nn++) {
        const char*         name    = _cdma_subscription_sources[nn].name;
        ACdmaSubscriptionSource ssource = _cdma_subscription_sources[nn].source;

        if (!name)
            break;

        if (!strcasecmp( args, name )) {
            amodem_set_cdma_subscription_source( client->modem, ssource );
            return 0;
        }
    }
    control_write( client, "KO: Don't know source %s\r\n", args );
    return -1;
}

static int
do_cdma_prl_version( ControlClient client, char * args )
{
    int version = 0;
    char *endptr;

    if (!args) {
        control_write( client, "KO: missing argument, try 'cdma prl_version <version>'\r\n");
        return -1;
    }

    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }

    version = strtol(args, &endptr, 0);
    if (endptr != args) {
        amodem_set_cdma_prl_version( client->modem, version );
    }
    return 0;
}
/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                           G S M   M O D E M                                     ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static const struct {
    const char*         name;
    const char*         display;
    ARegistrationState  state;
} _gsm_states[] = {
    { "unregistered",  "no network available", A_REGISTRATION_UNREGISTERED },
    { "home",          "on local network, non-roaming", A_REGISTRATION_HOME },
    { "roaming",       "on roaming network", A_REGISTRATION_ROAMING },
    { "searching",     "searching networks", A_REGISTRATION_SEARCHING },
    { "denied",        "emergency calls only", A_REGISTRATION_DENIED },
    { "off",           "same as 'unregistered'", A_REGISTRATION_UNREGISTERED },
    { "on",            "same as 'home'", A_REGISTRATION_HOME },
    { NULL, NULL, A_REGISTRATION_UNREGISTERED }
};

static const char*
gsm_state_to_string( ARegistrationState  state )
{
    int  nn;
    for (nn = 0; _gsm_states[nn].name != NULL; nn++) {
        if (state == _gsm_states[nn].state)
            return _gsm_states[nn].name;
    }
    return "<unknown>";
}

static int
do_gsm_status( ControlClient  client, char*  args )
{
    if (args) {
        control_write( client, "KO: no argument required\r\n" );
        return -1;
    }
    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }
    control_write( client, "gsm voice state: %s\r\n",
                   gsm_state_to_string(
                       amodem_get_voice_registration(client->modem) ) );
    control_write( client, "gsm data state:  %s\r\n",
                   gsm_state_to_string(
                       amodem_get_data_registration(client->modem) ) );
    return 0;
}


static void
help_gsm_data( ControlClient  client )
{
    int  nn;
    control_write( client,
            "the 'gsm data <state>' allows you to change the state of your GPRS connection\r\n"
            "valid values for <state> are the following:\r\n\r\n" );
    for (nn = 0; ; nn++) {
        const char*         name    = _gsm_states[nn].name;
        const char*         display = _gsm_states[nn].display;

        if (!name)
            break;

        control_write( client, "  %-15s %s\r\n", name, display );
    }
    control_write( client, "\r\n" );
}


static int
do_gsm_data( ControlClient  client, char*  args )
{
    int  nn;

    if (!args) {
        control_write( client, "KO: missing argument, try 'gsm data <state>'\r\n" );
        return -1;
    }

    for (nn = 0; ; nn++) {
        const char*         name    = _gsm_states[nn].name;
        ARegistrationState  state   = _gsm_states[nn].state;

        if (!name)
            break;

        if ( !strcmp( args, name ) ) {
            if (!client->modem) {
                control_write( client, "KO: modem emulation not running\r\n" );
                return -1;
            }
            amodem_set_data_registration( client->modem, state );
            qemu_net_disable = (state != A_REGISTRATION_HOME    &&
                                state != A_REGISTRATION_ROAMING );
            return 0;
        }
    }
    control_write( client, "KO: bad GSM data state name, try 'help gsm data' for list of valid values\r\n" );
    return -1;
}

static void
help_gsm_voice( ControlClient  client )
{
    int  nn;
    control_write( client,
            "the 'gsm voice <state>' allows you to change the state of your GPRS connection\r\n"
            "valid values for <state> are the following:\r\n\r\n" );
    for (nn = 0; ; nn++) {
        const char*         name    = _gsm_states[nn].name;
        const char*         display = _gsm_states[nn].display;

        if (!name)
            break;

        control_write( client, "  %-15s %s\r\n", name, display );
    }
    control_write( client, "\r\n" );
}


static int
do_gsm_voice( ControlClient  client, char*  args )
{
    int  nn;

    if (!args) {
        control_write( client, "KO: missing argument, try 'gsm voice <state>'\r\n" );
        return -1;
    }

    for (nn = 0; ; nn++) {
        const char*         name    = _gsm_states[nn].name;
        ARegistrationState  state   = _gsm_states[nn].state;

        if (!name)
            break;

        if ( !strcmp( args, name ) ) {
            if (!client->modem) {
                control_write( client, "KO: modem emulation not running\r\n" );
                return -1;
            }
            amodem_set_voice_registration( client->modem, state );
            return 0;
        }
    }
    control_write( client, "KO: bad GSM data state name, try 'help gsm voice' for list of valid values\r\n" );
    return -1;
}

static int
do_gsm_location( ControlClient  client, char*  args )
{
    int lac, ci;

    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }

    if (!args) {
        amodem_get_gsm_location( client->modem, &lac, &ci );
        control_write( client, "lac: %d\r\nci: %d\r\n", lac, ci );
        return 0;
    }

    if (sscanf(args, "%u %u", &lac, &ci) != 2) {
        control_write( client, "KO: missing argument, try 'gsm location <lac> <ci>'\r\n" );
        return -1;
    }

    if ((lac > 0xFFFF) || (ci > 0xFFFFFFF)) {
        control_write( client, "KO: invalid value\r\n" );
        return -1;
    }

    amodem_set_gsm_location( client->modem, lac, ci );
    return 0;
}

static int
gsm_check_number( char*  args )
{
    int  nn;

    for (nn = 0; args[nn] != 0; nn++) {
        int  c = args[nn];
        if ( !isdigit(c) && c != '+' && c != '#' ) {
            return -1;
        }
    }
    if (nn == 0)
        return -1;

    return 0;
}

static int
do_stk_pdu( ControlClient  client, char*  args  )
{
    if (!args) {
        control_write( client, "KO: missing argument, try 'stk pdu <hexstring>'\r\n" );
        return -1;
    }

    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }

    amodem_send_stk_unsol_proactive_command( client->modem, args );
    return 0;
}

static int
do_stk_setupcall( ControlClient  client, char*  args )
{
    if (!args) {
        control_write( client, "KO: missing argument, try 'stk setupcall <phonenumber>'\r\n" );
        return -1;
    }
    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }
    if ( amodem_add_outbound_call( client->modem, args ) < 0 ) {
        control_write( client, "KO: there are too many calls\r\n" );
        return -1;
    }
    return 0;
}

static int
do_stk_last_response( ControlClient  client, char*  args  )
{
    if (args) {
        control_write( client, "KO: unexpected argument(s) after 'lastapdu'\r\n" );
        return -1;
    }

    const char* pdu = amodem_get_last_stk_response( client->modem );

    if (!pdu || !strlen( pdu )) {
        control_write( client, "KO: Last STK response is unavailable\r\n" );
        return -1;
    }

    control_write( client, "%s\r\n", pdu );
    return 0;
}

static int
do_stk_last_envelope( ControlClient  client, char*  args  )
{
    if (args) {
        control_write( client, "KO: unexpected argument(s) after 'lastapdu'\r\n" );
        return -1;
    }

    const char* pdu = amodem_get_last_stk_envelope( client->modem );

    if (!pdu || !strlen( pdu )) {
        control_write( client, "KO: Last STK envelope is unavailable\r\n" );
        return -1;
    }

    control_write( client, "%s\r\n", pdu );
    return 0;
}

static int
do_gsm_call( ControlClient  client, char*  args )
{
    enum { NUMBER = 0, NUMBER_PRESENTATION, NAME, NAME_PRESENTATION, NUM_CALL_PARAMS };
    int     top_param = 0;
    char*   params[ NUM_CALL_PARAMS ];
    char*   number = "";
    int     number_presentation = 0;
    char*   name = "";
    int     name_presentation = 0;
    char*   end;

    /* check that we have a valid input */
    if (!args) {
        control_write( client, "KO: missing argument, try 'gsm call <phonenumber>[,<numPresentation>[,<name>[,<namePresentation]]]'\r\n" );
        return -1;
    }

    params[ top_param ] = args;
    while (end = strchr(params[ top_param ], ',')) {
        *end = '\0';
        if (++top_param >= NUM_CALL_PARAMS) {
            break;
        }
        params[ top_param ] = end + 1;
    }

    number = params[NUMBER];
    if (!number || gsm_check_number(number)) {
        control_write( client, "KO: bad phone number format, use digits, # and + only\r\n" );
        return -1;
    }

    if (top_param >= NUMBER_PRESENTATION && params[NUMBER_PRESENTATION]) {
        number_presentation = strtol( params[NUMBER_PRESENTATION], &end, 10 );
        if (*end) {
            control_write( client, "KO: argument '%s' is not a number\r\n", end );
            return -1;
        }

        // see CLI validity in TS 27.007 Clause 7.18.
        if (number_presentation < 0 || number_presentation > 4) {
            control_write( client, "KO: number presentation should be ranged between 0 and 4\r\n" );
            return -1;
        }
    }

    // see TS 23.096 Figure 3a, name is unavailable if number is not available.
    if (number_presentation == 0) {
        // If name presentation is not restricted, then check if name characters exist or not.
        if (top_param >= NAME_PRESENTATION && params[NAME_PRESENTATION]) {
            name_presentation = strtol( params[NAME_PRESENTATION], &end, 10 );
            if (*end) {
                control_write( client, "KO: argument '%s' is not a number\r\n", end );
                return -1;
            }

            // see CNI validity in TS 27.007 Clause 7.30.
            if (name_presentation < 0 || name_presentation > 2) {
                control_write( client, "KO: name presentation should be ranged between 0 and 2\r\n" );
                return -1;
            }
        }

        if (name_presentation == 0 && top_param >= NAME && params[NAME]) {
            name = params[NAME];
        }
    }

    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }
    amodem_add_inbound_call( client->modem, number, number_presentation, name, name_presentation );

    return 0;
}

static int
do_gsm_cancel( ControlClient  client, char*  args )
{
    if (!args) {
        control_write( client, "KO: missing argument, try 'gsm call <phonenumber>'\r\n" );
        return -1;
    }
    if (gsm_check_number(args)) {
        control_write( client, "KO: bad phone number format, use digits, # and + only\r\n" );
        return -1;
    }
    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }
    if ( amodem_disconnect_call( client->modem, args ) < 0 ) {
        control_write( client, "KO: could not cancel this number\r\n" );
        return -1;
    }
    return 0;
}


static const char*
call_state_to_string( ACallState  state )
{
    switch (state) {
        case A_CALL_ACTIVE:   return "active";
        case A_CALL_HELD:     return "held";
        case A_CALL_ALERTING: return "ringing";
        case A_CALL_WAITING:  return "waiting";
        case A_CALL_INCOMING: return "incoming";
        default: return "unknown";
    }
}

static int
do_gsm_list( ControlClient  client, char*  args )
{
    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }

    /* check that we have a phone number made of digits */
    int   count = amodem_get_call_count( client->modem );
    int   nn;
    for (nn = 0; nn < count; nn++) {
        ACall        call = amodem_get_call( client->modem, nn );
        const char*  dir;

        if (call == NULL)
            continue;

        if (call->dir == A_CALL_OUTBOUND)
            dir = "outbound to ";
         else
            dir = "inbound from";

        control_write( client, "%s %-10s : %s\r\n", dir,
                       call->number, call_state_to_string(call->state) );
    }
    return 0;
}

static int
do_gsm_busy( ControlClient  client, char*  args )
{
    ACall  call;

    if (!args) {
        control_write( client, "KO: missing argument, try 'gsm busy <phonenumber>'\r\n" );
        return -1;
    }

    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }

    call = amodem_find_call_by_number( client->modem, args );
    if (call == NULL || call->dir != A_CALL_OUTBOUND) {
        control_write( client, "KO: no current outbound call to number '%s' (call %p)\r\n", args, call );
        return -1;
    }
    if ( amodem_remote_call_busy( client->modem, args ) < 0 ) {
        control_write( client, "KO: could not cancel this number\r\n" );
        return -1;
    }
    return 0;
}

static int
do_gsm_hold( ControlClient  client, char*  args )
{
    ACall  call;

    if (!args) {
        control_write( client, "KO: missing argument, try 'gsm out hold <phonenumber>'\r\n" );
        return -1;
    }

    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }

    call = amodem_find_call_by_number( client->modem, args );
    if (call == NULL) {
        control_write( client, "KO: no current call to/from number '%s'\r\n", args );
        return -1;
    }
    if ( amodem_update_call( client->modem, args, A_CALL_HELD ) < 0 ) {
        control_write( client, "KO: could put this call on hold\r\n" );
        return -1;
    }
    return 0;
}


static int
do_gsm_accept( ControlClient  client, char*  args )
{
    ACall  call;

    if (!args) {
        control_write( client, "KO: missing argument, try 'gsm accept <phonenumber>'\r\n" );
        return -1;
    }

    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }

    call = amodem_find_call_by_number( client->modem, args );
    if (call == NULL) {
        control_write( client, "KO: no current call to/from number '%s'\r\n", args );
        return -1;
    }
    if ( amodem_update_call( client->modem, args, A_CALL_ACTIVE ) < 0 ) {
        control_write( client, "KO: could not activate this call\r\n" );
        return -1;
    }
    return 0;
}

static int
do_gsm_clear( ControlClient client, char* args)
{
    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }
    if ( amodem_clear_call( client->modem ) < 0 ) {
        control_write( client, "KO: could not clear up modem\r\n" );
        return -1;
    }
    return 0;
}

static int
do_gsm_signal( ControlClient  client, char*  args )
{
      enum { SIGNAL_RSSI = 0, SIGNAL_BER, NUM_SIGNAL_PARAMS };
      char*   p = args;
      int     top_param = -1;
      int     params[ NUM_SIGNAL_PARAMS ];

      static  int  last_ber = 99;
      int     rssi, ber;

      if (!client->modem) {
          control_write( client, "KO: modem emulation not running\r\n" );
          return -1;
      }

      if (!p) {
          amodem_get_signal_strength( client->modem, &rssi, &ber );
          control_write( client, "rssi = %d\r\nber = %d\r\n", rssi, ber );
          return 0;
      }

      /* tokenize */
      while (*p) {
          char*   end;
          int  val = strtol( p, &end, 10 );

          if (end == p) {
              control_write( client, "KO: argument '%s' is not a number\n", p );
              return -1;
          }

          params[++top_param] = val;
          if (top_param + 1 == NUM_SIGNAL_PARAMS)
              break;

          p = end;
          while (*p && (p[0] == ' ' || p[0] == '\t'))
              p += 1;
      }

      /* sanity check */
      if (top_param < SIGNAL_RSSI) {
          control_write( client, "KO: not enough arguments: see 'help gsm signal' for details\r\n" );
          return -1;
      }

      rssi = params[SIGNAL_RSSI];
      if ((rssi < 0 || rssi > 31) && rssi != 99) {
          control_write( client, "KO: invalid RSSI - must be 0..31 or 99\r\n");
          return -1;
      }

      /* check ber is 0..7 or 99 */
      if (top_param >= SIGNAL_BER) {
          ber = params[SIGNAL_BER];
          if ((ber < 0 || ber > 7) && ber != 99) {
              control_write( client, "KO: invalid BER - must be 0..7 or 99\r\n");
              return -1;
          }
          last_ber = ber;
      }

      amodem_set_signal_strength( client->modem, rssi, last_ber );

      return 0;
  }

static int
do_gsm_lte_signal( ControlClient  client, char*  args )
{
      enum { LTE_SIGNAL_RXLEV = 0, LTE_SIGNAL_RSRP, LTE_SIGNAL_RSSNR, NUM_LTE_SIGNAL_PARAMS };
      char*   p = args;
      int     top_param = -1;
      int     params[ NUM_LTE_SIGNAL_PARAMS ];
      int     rxlev, rsrp, rssnr;

      if (!client->modem) {
          control_write( client, "KO: modem emulation not running\r\n" );
          return -1;
      }

      if (!p) {
          amodem_get_lte_signal_strength( client->modem, &rxlev, &rsrp, &rssnr );
          control_write( client, "rxlev = %d\r\nrsrp = %d\r\nrssnr = %d\r\n", rxlev, rsrp, rssnr );
          return 0;
      }

      /* tokenize */
      while (*p) {
          char*   end;
          int  val = strtol( p, &end, 10 );

          if (end == p) {
              control_write( client, "KO: argument '%s' is not a number\n", p );
              return -1;
          }

          params[++top_param] = val;
          if (top_param + 1 == NUM_LTE_SIGNAL_PARAMS)
              break;

          p = end;
          while (*p && (p[0] == ' ' || p[0] == '\t'))
              p += 1;
      }

      /* sanity check */
      if (top_param < LTE_SIGNAL_RSSNR) {
          control_write( client, "KO: not enough arguments: see 'help gsm lte_signal' for details\r\n" );
          return -1;
      }

      rxlev = params[LTE_SIGNAL_RXLEV];
      if ((rxlev < 0 || rxlev > 63) && rxlev != 99) {
          control_write( client, "KO: invalid rxlev - must be 0..63 or 99\r\n");
          return -1;
      }

      rsrp = params[LTE_SIGNAL_RSRP];
      if ((rsrp < 44 || rsrp > 140) && rsrp != 65535) {
          control_write( client, "KO: invalid rsrp - must be 44..140 or 65535\r\n");
          return -1;
      }

      rssnr = params[LTE_SIGNAL_RSSNR];
      if ((rssnr < -200 || rssnr > 300) && rssnr != 65535) {
          control_write( client, "KO: invalid rssnr - must be -200..300 or 65535\r\n");
          return -1;
      }

      amodem_set_lte_signal_strength( client->modem, rxlev, rsrp, rssnr );

      return 0;
}

static void
do_gsm_report_creg( ControlClient  client, char*  args)
{
    ARegistrationUnsolMode creg = amodem_get_voice_unsol_mode(client->modem);

    control_write( client, "+CREG: %d\r\n", creg);
}

static int
do_gsm_report( ControlClient  client, char*  args )
{
    char* field;

    if (args) {
        field = strsep(&args, " ");
    } else {
        field = NULL;
    }

    do {
        if (!field || !strcmp(field, "creg")) {
            do_gsm_report_creg(client, args);
            if (field) {
                break;
            }
        }
    } while (field);

    return 0;
}

static int
do_gsm_enable_disable( ControlClient  client, char  *args, bool enable )
{
    if (strcmp( args, "hold" ) == 0)
        return amodem_set_feature(client->modem, A_MODEM_FEATURE_HOLD, enable);

    control_write( client, "KO: '%s' cannot be enabled or disabled.\r\n", args );
    return -1;
}

static int
do_gsm_enable( ControlClient  client, char  *args )
{
    if (!args) {
        control_write( client, "KO: missing argument, try 'gsm enable <feature>'\r\n" );
        return -1;
    }

    return do_gsm_enable_disable(client, args, true);
}

static int
do_gsm_disable( ControlClient  client, char  *args )
{
    if (!args) {
        control_write( client, "KO: missing argument, try 'gsm disable <feature>'\r\n" );
        return -1;
    }

    return do_gsm_enable_disable(client, args, false);
}

#if 0
static const CommandDefRec  gsm_in_commands[] =
{
    { "new", "create a new 'waiting' inbound call",
    "'gsm in create <phonenumber>' creates a new inbound phone call, placed in\r\n"
    "the 'waiting' state by default, until the system answers/holds/closes it\r\n", NULL
    do_gsm_in_create, NULL },

    { "hold", "change the state of an oubtound call to 'held'",
    "change the state of an outbound call to 'held'. this is only possible\r\n"
    "if the call in the 'waiting' or 'active' state\r\n", NULL,
    do_gsm_out_hold, NULL },

    { "accept", "change the state of an outbound call to 'active'",
    "change the state of an outbound call to 'active'. this is only possible\r\n"
    "if the call is in the 'waiting' or 'held' state\r\n", NULL,
    do_gsm_out_accept, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};
#endif


static const CommandDefRec  cdma_commands[] =
{
    { "ssource", "Set the current CDMA subscription source",
      NULL, describe_subscription_source,
      do_cdma_ssource, NULL },
    { "prl_version", "Dump the current PRL version",
      NULL, NULL,
      do_cdma_prl_version, NULL },
    { NULL, NULL, NULL, NULL, NULL, NULL }
};

static const CommandDefRec  gsm_commands[] =
{
    { "list", "list current phone calls",
    "'gsm list' lists all inbound and outbound calls and their state\r\n", NULL,
    do_gsm_list, NULL },

    { "call", "create inbound phone call",
    "'gsm call <phonenumber>[,<numPresentation>[,<name>[,<namePresentation]]]' allows you to simulate a new inbound call\r\n"
    "phonenumber is the inbound call number\r\n"
    "numPresentation range is 0..4\r\n"
    "name is the inbound call name\r\n"
    "namePresentation range is 0..2\r\n",
    NULL, do_gsm_call, NULL },

    { "busy", "close waiting outbound call as busy",
    "'gsm busy <remoteNumber>' closes an outbound call, reporting\r\n"
    "the remote phone as busy. only possible if the call is 'waiting'.\r\n", NULL,
    do_gsm_busy, NULL },

    { "hold", "change the state of an oubtound call to 'held'",
    "'gsm hold <remoteNumber>' change the state of a call to 'held'. this is only possible\r\n"
    "if the call in the 'waiting' or 'active' state\r\n", NULL,
    do_gsm_hold, NULL },

    { "accept", "change the state of an outbound call to 'active'",
    "'gsm accept <remoteNumber>' change the state of a call to 'active'. this is only possible\r\n"
    "if the call is in the 'waiting' or 'held' state\r\n", NULL,
    do_gsm_accept, NULL },

    { "clear", "clear current phone calls",
    "'gsm clear' cleans up all inbound and outbound calls\r\n", NULL,
    do_gsm_clear, NULL },

    { "cancel", "disconnect an inbound or outbound phone call",
    "'gsm cancel <phonenumber>' allows you to simulate the end of an inbound or outbound call\r\n", NULL,
    do_gsm_cancel, NULL },

    { "data", "modify data connection state", NULL, help_gsm_data,
    do_gsm_data, NULL },

    { "voice", "modify voice connection state", NULL, help_gsm_voice,
    do_gsm_voice, NULL },

    { "status", "display GSM status",
    "'gsm status' displays the current state of the GSM emulation\r\n", NULL,
    do_gsm_status, NULL },

    { "signal", "set sets the rssi and ber",
    "'gsm signal [<rssi> [<ber>]]' changes the reported strength and error rate on next (15s) update.\r\n"
    "rssi range is 0..31 and 99 for unknown\r\n"
    "ber range is 0..7 percent and 99 for unknown\r\n",
    NULL, do_gsm_signal, NULL },

    { "lte_signal", "set sets the LTE rxlev, rsrp and rssnr",
    "'gsm lte_signal [<rxlev> <rsrp> <rssnr>]' changes the reported LTE rxlev, rsrp and rssnr.\r\n"
    "rxlev range is 0..63 and 99 for unknown\r\n"
    "rsrp range is 44..140 dBm and 65535 for invalid\r\n"
    "rssnr range is -200..300 dB and 65535 for invalid\r\n",
    NULL, do_gsm_lte_signal, NULL },

    { "location", "set lac/ci",
    "'gsm location [<lac> <ci>]' sets or gets the location area code and cell identification.\r\n"
    "lac range is 0..65535 and ci range is 0..268435455\r\n",
    NULL, do_gsm_location, NULL},

    { "report", "report Modem status",
    "'gsm report'      report all known fields\r\n"
    "'gsm report creg' report CREG field\r\n",
    NULL, do_gsm_report, NULL},

    { "enable", "enable selected modem feature",
    "'gsm enable hold' enable the hold feature\r\n",
    NULL, do_gsm_enable, NULL},

    { "disable", "disable selected modem feature",
    "'gsm disable hold' disable the hold feature\r\n",
    NULL, do_gsm_disable, NULL},

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                           S M S   C O M M A N D                                 ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static int
do_sms_send( ControlClient  client, char*  args )
{
    char*          p;
    int            textlen;
    SmsAddressRec  sender;
    SmsPDU*        pdus;
    int            nn;

    /* check that we have a phone number made of digits */
    if (!args) {
    MissingArgument:
        control_write( client, "KO: missing argument, try 'sms send <phonenumber> <text message>'\r\n" );
        return -1;
    }
    p = strchr( args, ' ' );
    if (!p) {
        goto MissingArgument;
    }

    if ( sms_address_from_str( &sender, args, p - args ) < 0 ) {
        control_write( client, "KO: bad phone number format, must be [+](0-9)*\r\n" );
        return -1;
    }


    /* un-escape message text into proper utf-8 (conversion happens in-site) */
    p      += 1;
    textlen = strlen(p);
    textlen = sms_utf8_from_message_str( p, textlen, (unsigned char*)p, textlen );
    if (textlen < 0) {
        control_write( client, "message must be utf8 and can use the following escapes:\r\n"
                       "    \\n      for a newline\r\n"
                       "    \\xNN    where NN are two hexadecimal numbers\r\n"
                       "    \\uNNNN  where NNNN are four hexadecimal numbers\r\n"
                       "    \\\\     to send a '\\' character\r\n\r\n"
                       "    anything else is an error\r\n"
                       "KO: badly formatted text\r\n" );
        return -1;
    }

    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }

    /* create a list of SMS PDUs, then send them */
    pdus = smspdu_create_deliver_utf8( (cbytes_t)p, textlen, &sender, NULL );
    if (pdus == NULL) {
        control_write( client, "KO: internal error when creating SMS-DELIVER PDUs\n" );
        return -1;
    }

    for (nn = 0; pdus[nn] != NULL; nn++)
        amodem_receive_sms( client->modem, pdus[nn] );

    smspdu_free_list( pdus );
    return 0;
}

static int
do_sms_sendpdu( ControlClient  client, char*  args )
{
    SmsPDU  pdu;

    /* check that we have a phone number made of digits */
    if (!args) {
        control_write( client, "KO: missing argument, try 'sms sendpdu <hexstring>'\r\n" );
        return -1;
    }

    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }

    pdu = smspdu_create_from_hex( args, strlen(args) );
    if (pdu == NULL) {
        control_write( client, "KO: badly formatted <hexstring>\r\n" );
        return -1;
    }

    amodem_receive_sms( client->modem, pdu );
    smspdu_free( pdu );
    return 0;
}

static int
do_sms_smsc( ControlClient  client, char*  args )
{
    int           ret = 0;

    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }

    if (!args) {
        // Get
        SmsAddress pSmscRec;
        char       smsc[32] = {0};

        pSmscRec = amodem_get_smsc_address( client->modem );
        ret = sms_address_to_str( pSmscRec, smsc, sizeof(smsc) - 1);
        if (!ret) {
            control_write( client, "KO: SMSC address unvailable\r\n" );
            return -1;
        }

        control_write( client, "\"%s\",%u\r\n", smsc, pSmscRec->toa );
        return 0;
    }

    // Set
    if (amodem_set_smsc_address( client->modem, args, 0 )) {
        control_write( client, "KO: Failed to set SMSC address\r\n" );
        return -1;
    }

    return 0;
}

static const CommandDefRec  sms_commands[] =
{
    { "send", "send inbound SMS text message",
    "'sms send <phonenumber> <message>' allows you to simulate a new inbound sms message\r\n", NULL,
    do_sms_send, NULL },

    { "pdu", "send inbound SMS PDU",
    "'sms pdu <hexstring>' allows you to simulate a new inbound sms PDU\r\n"
    "(used internally when one emulator sends SMS messages to another instance).\r\n"
    "you probably don't want to play with this at all\r\n", NULL,
    do_sms_sendpdu, NULL },

    { "smsc", "get/set smsc address",
    "'sms smsc <smscaddress>' allows you to simulate set smsc address\r\n"
    "'sms smsc' allows you to simulate get smsc address\r\n", NULL,
    do_sms_smsc, NULL},

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

static const CommandDefRec stk_commands[] =
{
    { "pdu", "issue stk proactive command",
    "'stk pdu <hexstring>' allows you to issue stk PDU to simulate an unsolicted proactive command \r\n", NULL,
    do_stk_pdu, NULL },

    { "setupcall", "create an outbound phone call from stk directly",
    "'stk setupcall <phonenumber>' allows you to simulate a new outbound call dialed out from stk directly\r\n"
    "phonenumber is the outbound call number\r\n",
    NULL, do_stk_setupcall, NULL },

    { "lastresponse", "peek the last terminal response PDU from MT",
    "'stk lastresponse' the last terminal response PDU will be dumpped in hex string \r\n", NULL,
    do_stk_last_response, NULL },

    { "lastenvelope", "peek the last envelope PDU from MT",
    "'stk lastenvelope' the last envelope PDU will be dumpped in hex string \r\n", NULL,
    do_stk_last_envelope, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

static void
do_control_write(void* data, const char* string)
{
    control_write((ControlClient)data, string);
}

static int
do_power_display( ControlClient client, char*  args )
{
    goldfish_battery_display(do_control_write, client);
    return 0;
}

static int
do_ac_state( ControlClient  client, char*  args )
{
    if (args) {
        if (strcasecmp(args, "on") == 0) {
            goldfish_battery_set_prop(1, POWER_SUPPLY_PROP_ONLINE, 1);
            return 0;
        }
        if (strcasecmp(args, "off") == 0) {
            goldfish_battery_set_prop(1, POWER_SUPPLY_PROP_ONLINE, 0);
            return 0;
        }
    }

    control_write( client, "KO: Usage: \"ac on\" or \"ac off\"\n" );
    return -1;
}

static int
do_battery_status( ControlClient  client, char*  args )
{
    if (args) {
        if (strcasecmp(args, "unknown") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_STATUS_UNKNOWN);
            return 0;
        }
        if (strcasecmp(args, "charging") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_STATUS_CHARGING);
            return 0;
        }
        if (strcasecmp(args, "discharging") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_STATUS_DISCHARGING);
            return 0;
        }
        if (strcasecmp(args, "not-charging") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_STATUS_NOT_CHARGING);
            return 0;
        }
        if (strcasecmp(args, "full") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_STATUS_FULL);
            return 0;
        }
    }

    control_write( client, "KO: Usage: \"status unknown|charging|discharging|not-charging|full\"\n" );
    return -1;
}

static int
do_battery_present( ControlClient  client, char*  args )
{
    if (args) {
        if (strcasecmp(args, "true") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_PRESENT, 1);
            return 0;
        }
        if (strcasecmp(args, "false") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_PRESENT, 0);
            return 0;
        }
    }

    control_write( client, "KO: Usage: \"present true\" or \"present false\"\n" );
    return -1;
}

static int
do_battery_health( ControlClient  client, char*  args )
{
    if (args) {
        if (strcasecmp(args, "unknown") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_HEALTH, POWER_SUPPLY_HEALTH_UNKNOWN);
            return 0;
        }
        if (strcasecmp(args, "good") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_HEALTH, POWER_SUPPLY_HEALTH_GOOD);
            return 0;
        }
        if (strcasecmp(args, "overheat") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_HEALTH, POWER_SUPPLY_HEALTH_OVERHEAT);
            return 0;
        }
        if (strcasecmp(args, "dead") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_HEALTH, POWER_SUPPLY_HEALTH_DEAD);
            return 0;
        }
        if (strcasecmp(args, "overvoltage") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_HEALTH, POWER_SUPPLY_HEALTH_OVERVOLTAGE);
            return 0;
        }
        if (strcasecmp(args, "failure") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_HEALTH, POWER_SUPPLY_HEALTH_UNSPEC_FAILURE);
            return 0;
        }
    }

    control_write( client, "KO: Usage: \"health unknown|good|overheat|dead|overvoltage|failure\"\n" );
    return -1;
}

static int
do_battery_capacity( ControlClient  client, char*  args )
{
    if (args) {
        int capacity;

        if (sscanf(args, "%d", &capacity) == 1 && capacity >= 0 && capacity <= 100) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_CAPACITY, capacity);
            return 0;
        }
    }

    control_write( client, "KO: Usage: \"capacity <percentage>\"\n" );
    return -1;
}


static const CommandDefRec  power_commands[] =
{
    { "display", "display battery and charger state",
    "display battery and charger state\r\n", NULL,
    do_power_display, NULL },

    { "ac", "set AC charging state",
    "'ac on|off' allows you to set the AC charging state to on or off\r\n", NULL,
    do_ac_state, NULL },

    { "status", "set battery status",
    "'status unknown|charging|discharging|not-charging|full' allows you to set battery status\r\n", NULL,
    do_battery_status, NULL },

    { "present", "set battery present state",
    "'present true|false' allows you to set battery present state to true or false\r\n", NULL,
    do_battery_present, NULL },

    { "health", "set battery health state",
    "'health unknown|good|overheat|dead|overvoltage|failure' allows you to set battery health state\r\n", NULL,
    do_battery_health, NULL },

    { "capacity", "set battery capacity state",
    "'capacity <percentage>' allows you to set battery capacity to a value 0 - 100\r\n", NULL,
    do_battery_capacity, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                         E  V  E  N  T   C O M M A N D S                         ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/


static int
do_event_send( ControlClient  client, char*  args )
{
    char*   p;

    if (!args) {
        control_write( client, "KO: Usage: event send <type>:<code>:<value> ...\r\n" );
        return -1;
    }

    p = args;
    while (*p) {
        char*  q;
        char   temp[128];
        int    type, code, value, ret;

        p += strspn( p, " \t" );  /* skip spaces */
        if (*p == 0)
            break;

        q  = p + strcspn( p, " \t" );

        if (q == p)
            break;

        snprintf(temp, sizeof temp, "%.*s", (int)(intptr_t)(q-p), p);
        ret = android_event_from_str( temp, &type, &code, &value );
        if (ret < 0) {
            if (ret == -1) {
                control_write( client,
                               "KO: invalid event type in '%.*s', try 'event list types' for valid values\r\n",
                               q-p, p );
            } else if (ret == -2) {
                control_write( client,
                               "KO: invalid event code in '%.*s', try 'event list codes <type>' for valid values\r\n",
                               q-p, p );
            } else {
                control_write( client,
                               "KO: invalid event value in '%.*s', must be an integer\r\n",
                               q-p, p);
            }
            return -1;
        }

        user_event_generic( type, code, value );
        p = q;
    }
    return 0;
}

static int
do_event_types( ControlClient  client, char*  args )
{
    int  count = android_event_get_type_count();
    int  nn;

    control_write( client, "event <type> can be an integer or one of the following aliases\r\n" );
    for (nn = 0; nn < count; nn++) {
        char  tmp[16];
        char* p = tmp;
        char* end = p + sizeof(tmp);
        int   count2 = android_event_get_code_count( nn );;

        p = android_event_bufprint_type_str( p, end, nn );

        control_write( client, "    %-8s", tmp );
        if (count2 > 0)
            control_write( client, "  (%d code aliases)", count2 );

        control_write( client, "\r\n" );
    }
    return 0;
}

static int
do_event_codes( ControlClient  client, char*  args )
{
    int  count;
    int  nn, type, dummy;

    if (!args) {
        control_write( client, "KO: argument missing, try 'event codes <type>'\r\n" );
        return -1;
    }

    if ( android_event_from_str( args, &type, &dummy, &dummy ) < 0 ) {
        control_write( client, "KO: bad argument, see 'event types' for valid values\r\n" );
        return -1;
    }

    count = android_event_get_code_count( type );
    if (count == 0) {
        control_write( client, "no code aliases defined for this type\r\n" );
    } else {
        control_write( client, "type '%s' accepts the following <code> aliases:\r\n",
                       args );
        for (nn = 0; nn < count; nn++) {
            char  temp[20], *p = temp, *end = p + sizeof(temp);
            android_event_bufprint_code_str( p, end, type, nn );
            control_write( client, "    %-12s\r\n", temp );
        }
    }

    return 0;
}

static __inline__ int
utf8_next( unsigned char* *pp, unsigned char*  end )
{
    unsigned char*  p      = *pp;
    int             result = -1;

    if (p < end) {
        int  c= *p++;
        if (c >= 128) {
            if ((c & 0xe0) == 0xc0)
                c &= 0x1f;
            else if ((c & 0xf0) == 0xe0)
                c &= 0x0f;
            else
                c &= 0x07;

            while (p < end && (p[0] & 0xc0) == 0x80) {
                c = (c << 6) | (p[0] & 0x3f);
            }
        }
        result = c;
        *pp    = p;
    }
    return result;
}

static int
do_event_text( ControlClient  client, char*  args )
{
    SkinKeycodeBuffer keycodes;
    unsigned char*  p   = (unsigned char*) args;
    unsigned char*  end = p + strlen(args);
    int             textlen;
    const SkinCharmap* charmap;

    if (!args) {
        control_write( client, "KO: argument missing, try 'event text <message>'\r\n" );
        return -1;
    }

    /* Get active charmap. */
    charmap = skin_charmap_get();
    if (charmap == NULL) {
        control_write( client, "KO: no character map active in current device layout/config\r\n" );
        return -1;
    }

    skin_keycode_buffer_init(&keycodes, &user_event_keycodes);

    /* un-secape message text into proper utf-8 (conversion happens in-site) */
    textlen = strlen((char*)p);
    textlen = sms_utf8_from_message_str( args, textlen, (unsigned char*)p, textlen );
    if (textlen < 0) {
        control_write( client, "message must be utf8 and can use the following escapes:\r\n"
                       "    \\n      for a newline\r\n"
                       "    \\xNN    where NN are two hexadecimal numbers\r\n"
                       "    \\uNNNN  where NNNN are four hexadecimal numbers\r\n"
                       "    \\\\     to send a '\\' character\r\n\r\n"
                       "    anything else is an error\r\n"
                       "KO: badly formatted text\r\n" );
        return -1;
    }

    end = p + textlen;
    while (p < end) {
        int  c = utf8_next( &p, end );
        if (c <= 0)
            break;

        skin_charmap_reverse_map_unicode( NULL, (unsigned)c, 1, &keycodes );
        skin_charmap_reverse_map_unicode( NULL, (unsigned)c, 0, &keycodes );
        skin_keycode_buffer_flush( &keycodes );
    }

    return 0;
}

static const CommandDefRec  event_commands[] =
{
    { "send", "send a series of events to the kernel",
    "'event send <type>:<code>:<value> ...' allows your to send one or more hardware events\r\n"
    "to the Android kernel. you can use text names or integers for <type> and <code>\r\n", NULL,
    do_event_send, NULL },

    { "types", "list all <type> aliases",
    "'event types' list all <type> string aliases supported by the 'event' subcommands\r\n",
    NULL, do_event_types, NULL },

    { "codes", "list all <code> aliases for a given <type>",
    "'event codes <type>' lists all <code> string aliases for a given event <type>\r\n",
    NULL, do_event_codes, NULL },

    { "text", "simulate keystrokes from a given text",
    "'event text <message>' allows you to simulate keypresses to generate a given text\r\n"
    "message. <message> must be an utf-8 string. Unicode points will be reverse-mapped\r\n"
    "according to the current device keyboard. unsupported characters will be discarded\r\n"
    "silently\r\n", NULL, do_event_text, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};


/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                      S N A P S H O T   C O M M A N D S                          ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static int
control_write_out_cb(void* opaque, const char* str, int strsize)
{
    ControlClient client = opaque;
    control_control_write(client, str, strsize);
    return strsize;
}

static int
control_write_err_cb(void* opaque, const char* str, int strsize)
{
    int ret = 0;
    ControlClient client = opaque;
    ret += control_write(client, "KO: ");
    control_control_write(client, str, strsize);
    return ret + strsize;
}

static int
do_snapshot_list( ControlClient  client, char*  args )
{
    int64_t ret;
    Monitor *out = monitor_fake_new(client, control_write_out_cb);
    Monitor *err = monitor_fake_new(client, control_write_err_cb);
    do_info_snapshots(out, err);
    ret = monitor_fake_get_bytes(err);
    monitor_fake_free(err);
    monitor_fake_free(out);

    return ret > 0;
}

static int
do_snapshot_save( ControlClient  client, char*  args )
{
    int64_t ret;

    if (args == NULL) {
        control_write(client, "KO: argument missing, try 'avd snapshot save <name>'\r\n");
        return -1;
    }

    Monitor *err = monitor_fake_new(client, control_write_err_cb);
    do_savevm(err, args);
    ret = monitor_fake_get_bytes(err);
    monitor_fake_free(err);

    return ret > 0; // no output on error channel indicates success
}

static int
do_snapshot_load( ControlClient  client, char*  args )
{
    int64_t ret;

    if (args == NULL) {
        control_write(client, "KO: argument missing, try 'avd snapshot load <name>'\r\n");
        return -1;
    }

    Monitor *err = monitor_fake_new(client, control_write_err_cb);
    do_loadvm(err, args);
    ret = monitor_fake_get_bytes(err);
    monitor_fake_free(err);

    return ret > 0;
}

static int
do_snapshot_del( ControlClient  client, char*  args )
{
    int64_t ret;

    if (args == NULL) {
        control_write(client, "KO: argument missing, try 'avd snapshot del <name>'\r\n");
        return -1;
    }

    Monitor *err = monitor_fake_new(client, control_write_err_cb);
    do_delvm(err, args);
    ret = monitor_fake_get_bytes(err);
    monitor_fake_free(err);

    return ret > 0;
}

static const CommandDefRec  snapshot_commands[] =
{
    { "list", "list available state snapshots",
    "'avd snapshot list' will show a list of all state snapshots that can be loaded\r\n",
    NULL, do_snapshot_list, NULL },

    { "save", "save state snapshot",
    "'avd snapshot save <name>' will save the current (run-time) state to a snapshot with the given name\r\n",
    NULL, do_snapshot_save, NULL },

    { "load", "load state snapshot",
    "'avd snapshot load <name>' will load the state snapshot of the given name\r\n",
    NULL, do_snapshot_load, NULL },

    { "del", "delete state snapshot",
    "'avd snapshot del <name>' will delete the state snapshot with the given name\r\n",
    NULL, do_snapshot_del, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};



/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                               V M   C O M M A N D S                             ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static int
do_avd_stop( ControlClient  client, char*  args )
{
    if (!vm_running) {
        control_write( client, "KO: virtual device already stopped\r\n" );
        return -1;
    }
    vm_stop(EXCP_INTERRUPT);
    return 0;
}

static int
do_avd_start( ControlClient  client, char*  args )
{
    if (vm_running) {
        control_write( client, "KO: virtual device already running\r\n" );
        return -1;
    }
    vm_start();
    return 0;
}

static int
do_avd_status( ControlClient  client, char*  args )
{
    control_write( client, "virtual device is %s\r\n", vm_running ? "running" : "stopped" );
    return 0;
}

static int
do_avd_name( ControlClient  client, char*  args )
{
    control_write( client, "%s\r\n", android_hw->avd_name);
    return 0;
}

static const CommandDefRec  vm_commands[] =
{
    { "stop", "stop the virtual device",
    "'avd stop' stops the virtual device immediately, use 'avd start' to continue execution\r\n",
    NULL, do_avd_stop, NULL },

    { "start", "start/restart the virtual device",
    "'avd start' will start or continue the virtual device, use 'avd stop' to stop it\r\n",
    NULL, do_avd_start, NULL },

    { "status", "query virtual device status",
    "'avd status' will indicate whether the virtual device is running or not\r\n",
    NULL, do_avd_status, NULL },

    { "name", "query virtual device name",
    "'avd name' will return the name of this virtual device\r\n",
    NULL, do_avd_name, NULL },

    { "snapshot", "state snapshot commands",
    "allows you to save and restore the virtual device state in snapshots\r\n",
    NULL, NULL, snapshot_commands },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                             G E O   C O M M A N D S                             ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static int
do_geo_nmea( ControlClient  client, char*  args )
{
    if (!args) {
        control_write( client, "KO: NMEA sentence missing, try 'help geo nmea'\r\n" );
        return -1;
    }
    if (!android_gps_cs) {
        control_write( client, "KO: no GPS emulation in this virtual device\r\n" );
        return -1;
    }
    android_gps_send_nmea( args );
    return 0;
}

static int
do_geo_fix( ControlClient  client, char*  args )
{
    // GEO_SAT2 provides bug backwards compatibility.
    enum { GEO_LONG = 0, GEO_LAT, GEO_ALT, GEO_SAT, GEO_SAT2, NUM_GEO_PARAMS };
    char*   p = args;
    int     top_param = -1;
    double  params[ NUM_GEO_PARAMS ];
    int     n_satellites = 1;

    if (!p)
        p = "";

    /* tokenize */
    while (*p) {
        char*   end;
        double  val = strtod( p, &end );

        if (end == p) {
            control_write( client, "KO: argument '%s' is not a number\n", p );
            return -1;
        }

        params[++top_param] = val;
        if (top_param + 1 == NUM_GEO_PARAMS)
            break;

        p = end;
        while (*p && (p[0] == ' ' || p[0] == '\t'))
            p += 1;
    }

    /* sanity check */
    if (top_param < GEO_LAT) {
        control_write( client, "KO: not enough arguments: see 'help geo fix' for details\r\n" );
        return -1;
    }

    /* check number of satellites, must be integer between 1 and 12 */
    if (top_param >= GEO_SAT) {
        int sat_index = (top_param >= GEO_SAT2) ? GEO_SAT2 : GEO_SAT;
        n_satellites = (int) params[sat_index];
        if (n_satellites != params[sat_index]
            || n_satellites < 1 || n_satellites > 12) {
            control_write( client, "KO: invalid number of satellites. Must be an integer between 1 and 12\r\n");
            return -1;
        }
    }

    /* generate an NMEA sentence for this fix */
    {
        STRALLOC_DEFINE(s);
        double   val;
        int      deg, min;
        char     hemi;
        int      hh = 0, mm = 0, ss = 0;

        /* format overview:
         *    time of fix      123519     12:35:19 UTC
         *    latitude         4807.038   48 degrees, 07.038 minutes
         *    north/south      N or S
         *    longitude        01131.000  11 degrees, 31. minutes
         *    east/west        E or W
         *    fix quality      1          standard GPS fix
         *    satellites       1 to 12    number of satellites being tracked
         *    HDOP             <dontcare> horizontal dilution
         *    altitude         546.       altitude above sea-level
         *    altitude units   M          to indicate meters
         *    diff             <dontcare> height of sea-level above ellipsoid
         *    diff units       M          to indicate meters (should be <dontcare>)
         *    dgps age         <dontcare> time in seconds since last DGPS fix
         *    dgps sid         <dontcare> DGPS station id
         */

        // Get the current time as hh:mm:ss
        struct timeval tm;

        if (0 == gettimeofday(&tm, NULL)) {
            // tm.tv_sec is elapsed seconds since epoch (UTC, which is what we want)
            hh = (int) (tm.tv_sec / (60 * 60)) % 24;
            mm = (int) (tm.tv_sec /  60      ) % 60;
            ss = (int) (tm.tv_sec            ) % 60;
        }

        stralloc_add_format( s, "$GPGGA,%02d%02d%02d", hh, mm, ss);

        /* then the latitude */
        hemi = 'N';
        val  = params[GEO_LAT];
        if (val < 0) {
            hemi = 'S';
            val  = -val;
        }
        deg = (int) val;
        val = 60*(val - deg);
        min = (int) val;
        val = 10000*(val - min);
        stralloc_add_format( s, ",%02d%02d.%04d,%c", deg, min, (int)val, hemi );

        /* the longitude */
        hemi = 'E';
        val  = params[GEO_LONG];
        if (val < 0) {
            hemi = 'W';
            val  = -val;
        }
        deg = (int) val;
        val = 60*(val - deg);
        min = (int) val;
        val = 10000*(val - min);
        stralloc_add_format( s, ",%02d%02d.%04d,%c", deg, min, (int)val, hemi );

        /* bogus fix quality, satellite count and dilution */
        stralloc_add_format( s, ",1,%02d,", n_satellites );

        /* optional altitude + bogus diff */
        if (top_param >= GEO_ALT) {
            stralloc_add_format( s, ",%.1g,M,0.,M", params[GEO_ALT] );
        } else {
            stralloc_add_str( s, ",,,," );
        }
        /* bogus rest and checksum */
        stralloc_add_str( s, ",,,*47" );

        /* send it, then free */
        android_gps_send_nmea( stralloc_cstr(s) );
        stralloc_reset( s );
    }
    return 0;
}

static const CommandDefRec  geo_commands[] =
{
    { "nmea", "send a GPS NMEA sentence",
    "'geo nema <sentence>' sends an NMEA 0183 sentence to the emulated device, as\r\n"
    "if it came from an emulated GPS modem. <sentence> must begin with '$GP'. Only\r\n"
    "'$GPGGA' and '$GPRCM' sentences are supported at the moment.\r\n",
    NULL, do_geo_nmea, NULL },

    { "fix", "send a simple GPS fix",
    "'geo fix <longitude> <latitude> [<altitude> [<satellites>]]'\r\n"
    " allows you to send a simple GPS fix to the emulated system.\r\n"
    " The parameters are:\r\n\r\n"
    "  <longitude>   longitude, in decimal degrees\r\n"
    "  <latitude>    latitude, in decimal degrees\r\n"
    "  <altitude>    optional altitude in meters\r\n"
    "  <satellites>  number of satellites being tracked (1-12)\r\n"
    "\r\n",
    NULL, do_geo_fix, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};


/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                        S E N S O R S  C O M M A N D S                           ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

/* For sensors user prompt string size.*/
#define SENSORS_INFO_SIZE 150

/* Get sensor data - (a,b,c) from sensor name */
static int
do_sensors_get( ControlClient client, char* args )
{
    if (! args) {
        control_write( client, "KO: Usage: \"get <sensorname>\"\n" );
        return -1;
    }

    int status = SENSOR_STATUS_UNKNOWN;
    char sensor[strlen(args) + 1];
    if (1 != sscanf( args, "%s", &sensor[0] ))
        goto SENSOR_STATUS_ERROR;

    int sensor_id = android_sensors_get_id_from_name( sensor );
    char buffer[SENSORS_INFO_SIZE] = { 0 };
    float a, b, c;

    if (sensor_id < 0) {
        status = sensor_id;
        goto SENSOR_STATUS_ERROR;
    } else {
        status = android_sensors_get( sensor_id, &a, &b, &c );
        if (status != SENSOR_STATUS_OK)
            goto SENSOR_STATUS_ERROR;
        snprintf( buffer, sizeof(buffer),
                "%s = %g:%g:%g\r\n", sensor, a, b, c );
        do_control_write( client, buffer );
        return 0;
    }

SENSOR_STATUS_ERROR:
    switch(status) {
    case SENSOR_STATUS_NO_SERVICE:
        snprintf( buffer, sizeof(buffer), "KO: No sensor service found!\r\n" );
        break;
    case SENSOR_STATUS_DISABLED:
        snprintf( buffer, sizeof(buffer), "KO: '%s' sensor is disabled.\r\n", sensor );
        break;
    case SENSOR_STATUS_UNKNOWN:
        snprintf( buffer, sizeof(buffer),
                "KO: unknown sensor name: %s, run 'sensor status' to get available sensors.\r\n", sensor );
        break;
    default:
        snprintf( buffer, sizeof(buffer), "KO: '%s' sensor: exception happens.\r\n", sensor );
    }
    do_control_write( client, buffer );
    return -1;
}

/* set sensor data - (a,b,c) from sensor name */
static int
do_sensors_set( ControlClient client, char* args )
{
    if (! args) {
        control_write( client, "KO: Usage: \"set <sensorname> <value-a>[:<value-b>[:<value-c>]]\"\n" );
        return -1;
    }

    int status;
    char* sensor;
    char* value;
    char* args_dup = strdup( args );
    if (args_dup == NULL) {
        control_write( client, "KO: Memory allocation failed.\n" );
        return -1;
    }
    char* p = args_dup;

    /* Parsing the args to get sensor name string */
    while (*p && isspace(*p)) p++;
    if (*p == 0)
        goto INPUT_ERROR;
    sensor = p;

    /* Parsing the args to get value string */
    while (*p && (! isspace(*p))) p++;
    if (*p == 0 || *(p + 1) == 0/* make sure value isn't NULL */)
        goto INPUT_ERROR;
    *p = 0;
    value = p + 1;

    if (! (strlen(sensor) && strlen(value)))
        goto INPUT_ERROR;

    int sensor_id = android_sensors_get_id_from_name( sensor );
    char buffer[SENSORS_INFO_SIZE] = { 0 };

    if (sensor_id < 0) {
        status = sensor_id;
        goto SENSOR_STATUS_ERROR;
    } else {
        float fvalues[3];
        status = android_sensors_get( sensor_id, &fvalues[0], &fvalues[1], &fvalues[2] );
        if (status != SENSOR_STATUS_OK)
            goto SENSOR_STATUS_ERROR;

        /* Parsing the value part to get the sensor values(a, b, c) */
        int i;
        char* pnext;
        char* pend = value + strlen(value);
        for (i = 0; i < 3; i++, value = pnext + 1) {
            pnext=strchr( value, ':' );
            if (pnext) {
                *pnext = 0;
            } else {
                pnext = pend;
            }

            if (pnext > value) {
                if (1 != sscanf( value,"%g", &fvalues[i] ))
                    goto INPUT_ERROR;
            }
        }

        status = android_sensors_set( sensor_id, fvalues[0], fvalues[1], fvalues[2] );
        if (status != SENSOR_STATUS_OK)
            goto SENSOR_STATUS_ERROR;

        free( args_dup );
        return 0;
    }

SENSOR_STATUS_ERROR:
    switch(status) {
    case SENSOR_STATUS_NO_SERVICE:
        snprintf( buffer, sizeof(buffer), "KO: No sensor service found!\r\n" );
        break;
    case SENSOR_STATUS_DISABLED:
        snprintf( buffer, sizeof(buffer), "KO: '%s' sensor is disabled.\r\n", sensor );
        break;
    case SENSOR_STATUS_UNKNOWN:
        snprintf( buffer, sizeof(buffer),
                "KO: unknown sensor name: %s, run 'sensor status' to get available sensors.\r\n", sensor );
        break;
    default:
        snprintf( buffer, sizeof(buffer), "KO: '%s' sensor: exception happens.\r\n", sensor );
    }
    do_control_write( client, buffer );
    free( args_dup );
    return -1;

INPUT_ERROR:
    control_write( client, "KO: Usage: \"set <sensorname> <value-a>[:<value-b>[:<value-c>]]\"\n" );
    free( args_dup );
    return -1;
}

/* get all available sensor names and enable status respectively. */
static int
do_sensors_status( ControlClient client, char* args )
{
    uint8_t id, status;
    char buffer[SENSORS_INFO_SIZE] = { 0 };

    for(id = 0; id < MAX_SENSORS; id++) {
        status = android_sensors_get_sensor_status( id );
        snprintf( buffer, sizeof(buffer), "%s: %s\n",
                android_sensors_get_name_from_id(id), (status ? "enabled.":"disabled.") );
        control_write( client, buffer );
    }

    return 0;
}

/* Sensor commands for get/set sensor values and get available sensor names. */
static const CommandDefRec sensor_commands[] =
{
    { "status", "list all sensors and their status.",
      "'status': list all sensors and their status.\r\n",
      NULL, do_sensors_status, NULL },

    { "get", "get sensor values",
      "'get <sensorname>' returns the values of a given sensor.\r\n",
      NULL, do_sensors_get, NULL },

    { "set", "set sensor values",
      "'set <sensorname> <value-a>[:<value-b>[:<value-c>]]' set the values of a given sensor.\r\n",
      NULL, do_sensors_set, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                        F I N G E R P R I N T  C O M M A N D S                   ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static int
do_fingerprint_touch(ControlClient client, char* args )
{
    if (args) {
        char *endptr;
        int fingerid = strtol(args, &endptr, 0);
        if (endptr != args) {
            android_hw_fingerprint_touch(fingerid);
            return 0;
        }
        control_write(client, "KO: invalid fingerid\r\n");
        return -1;
    }
    control_write(client, "KO: missing fingerid\r\n");
    return -1;
}

static int
do_fingerprint_remove(ControlClient client, char* args )
{
    android_hw_fingerprint_remove();
    return 0;
}

static const CommandDefRec fingerprint_commands[] =
{
    { "touch", "touch finger print sensor with <fingerid>",
      "'touch <fingerid>' touch finger print sensor with <fingerid>.\r\n",
      NULL, do_fingerprint_touch, NULL },
    { "remove", "remove finger from the fingerprint sensor",
      "'remove' remove finger from the fingerprint sensor.\r\n",
      NULL, do_fingerprint_remove, NULL },
    { NULL, NULL, NULL, NULL, NULL, NULL }
};

/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****             T E L E P H O N Y   O P E R A T O R   C O M M A N D S               ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static int
do_operator_dumpall( ControlClient client, char* args )
{
    int oper_index = 0, name_index, pos = 0, n;
    char replybuf[64];

    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }

    for (; oper_index < A_OPERATOR_MAX; oper_index++) {
        for (name_index = 0; name_index < A_NAME_MAX; name_index++) {
            n = amodem_get_operator_name_ex(client->modem,
                                            oper_index, name_index,
                                            replybuf + pos, sizeof(replybuf) - pos);
            if (n) {
                --n;
            }
            pos += n;
            replybuf[pos++] = ',';
        }
        replybuf[pos - 1] = '\n';
    }
    replybuf[pos] = '\0';

    control_write(client, replybuf);

    return 0;
}

static int
do_operator_get( ControlClient client, char* args )
{
    if (!args) {
        control_write(client, "KO: Usage: operator get <operator index>\n");
        return -1;
    }

    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }

    int oper_index = A_OPERATOR_MAX;
    if ((sscanf(args, "%u", &oper_index) != 1) ||
        (oper_index >= A_OPERATOR_MAX)) {
        control_write(client, "KO: invalid operator index\n");
        return -1;
    }

    int name_index = 0, pos = 0, n;
    char replybuf[64];
    for (; name_index < A_NAME_MAX; name_index++) {
        n = amodem_get_operator_name_ex(client->modem,
                                        oper_index, name_index,
                                        replybuf + pos, sizeof(replybuf) - pos);
        if (n) {
            --n;
        }
        pos += n;
        replybuf[pos++] = ',';
    }
    replybuf[pos - 1] = '\n';
    replybuf[pos] = '\0';

    control_write(client, replybuf);

    return 0;
}

static int
do_operator_set( ControlClient client, char* args )
{
    char* args_dup = NULL;

    if (!args) {
USAGE:
        control_write(client, "Usage: operator set <operator index> <long name>[,<short name>[,<mcc mnc>]]\n");
FREE_BUF:
        if (args_dup) free(args_dup);
        return -1;
    }

    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }

    args_dup = strdup(args);
    if (args_dup == NULL) {
        control_write( client, "KO: Memory allocation failed.\n" );
        return -1;
    }

    char* p = args_dup;
    /* Skip leading white spaces. */
    while (*p && isspace(*p)) p++;
    if (!*p) goto USAGE;

    int oper_index = 0;
    if ((sscanf(args, "%u", &oper_index) != 1)
        || (oper_index >= A_OPERATOR_MAX)) {
        control_write(client, "KO: invalid operator index\n");
        goto FREE_BUF;
    }

    /* Skip operator index. */
    while (*p && !isspace(*p)) p++;
    if (!*p) goto USAGE;
    /* Skip white spaces. */
    while (*p && isspace(*p)) p++;
    if (!*p) goto USAGE;

    char* longName = p;
    char* shortName = NULL;
    char* mccMnc = NULL;

    p = strchr(p, ',');
    if (p) {
      *p = '\0';

      shortName = ++p;
      p = strchr(p, ',');
      if (p) {
        *p = '\0';
	mccMnc = ++p;
      }
    }

    amodem_set_operator_name_ex(client->modem, oper_index, A_NAME_LONG, longName, -1);
    if (shortName) {
      amodem_set_operator_name_ex(client->modem, oper_index, A_NAME_SHORT, shortName, -1);
    }
    if (mccMnc) {
      amodem_set_operator_name_ex(client->modem, oper_index, A_NAME_NUMERIC, mccMnc, -1);
    }

    // Notify device through amodem_unsol(...)
    amodem_set_voice_registration(client->modem, amodem_get_voice_registration(client->modem));

    do_operator_dumpall(client, NULL);

    if (args_dup) {
      free(args_dup);
    }
    return 0;
}

static const CommandDefRec  operator_commands[] =
{
    { "dumpall", "dump all operators info",
      "'dumpall': dump all operators and their long/short names, mcc and mnc.\r\n",
      NULL, do_operator_dumpall, NULL },

    { "get", "get operator info by index",
      "'get <operator index>' get the values of specified operator.\r\n",
      NULL, do_operator_get, NULL},

    { "set", "set operator info by index",
      "'set <operator index> <long name>[,<short name>[,<mcc mnc>]]' set the values of specified operator.\r\n",
      NULL, do_operator_set, NULL},

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                            M U X   C O M M A N D S                              ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static int
do_mux_modem( ControlClient client, char* args )
{
    if (!args) {
        if (!client->modem) {
            control_write( client, "KO: modem emulation not running\r\n" );
            return -1;
        }

        control_write( client, "%d\n", amodem_get_instance_id(client->modem) );
        return 0;
    }

    unsigned int instance_id = 0;
    if ((sscanf(args, "%u", &instance_id) != 1)
            || (instance_id >= amodem_num_devices)) {
        control_write(client, "Usage: mux modem [<instance index>]\n");
        return -1;
    }

    AModem modem = amodem_get_instance(instance_id);
    if (!modem) {
        /**
         * Just give a warning message here and still allows selecting an
         * invalid modem. Because when it comes to inter-emulator communication
         * and the selection fails here and fallback to original modem, it will
         * send wrong command to wrong modem and causes unexpected behaviour.
         */
        control_write(client, "WARNING: Modem[%u] is not enabled\n", instance_id);
    }

    client->modem = modem;
    return 0;
}

static int
do_mux_bt_list( ControlClient client )
{
    int id;
    ABluetooth bt;

    id = 0;
    while ((bt = abluetooth_get_instance(id++)) != NULL) {
        struct bt_device_s *dev;
        char buf[BDADDR_BUF_LEN];

        dev = abluetooth_get_bt_device(bt);
        if (dev) {
            ba_to_str(buf, &dev->bd_addr);
        }
        control_write(client, "%c %s\n",
                      (client->bt == bt ? '*' : 'L'),
                      (dev ? buf : "(null)"));
    }

    return 0;
}

static int
do_mux_bt_set( ControlClient client, char* args )
{
    ABluetooth bt;
    bdaddr_t addr;

    bt = NULL;
    if (!ba_from_str(&addr, args)) {
        ABluetooth candidate;
        int id = 0;

        while ((candidate = abluetooth_get_instance(id++)) != NULL) {
            struct bt_device_s *dev;

            dev = abluetooth_get_bt_device(candidate);
            if (dev && !bacmp(&addr, &dev->bd_addr)) {
                bt = candidate;
                break;
            }
        }
    }

    if (!bt) {
        control_write(client, "WARNING: device '%s' is not available\n", args);
    }

    client->bt = bt;
    return do_mux_bt_list(client);
}

static int
do_mux_bt( ControlClient client, char* args )
{
    return args ? do_mux_bt_set(client, args) : do_mux_bt_list(client);
}

static const CommandDefRec  mux_commands[] =
{
    { "modem", "select active modem device",
      "'modem <instance index>': select active modem device by instance for further config.\r\n"
      "'modem': display current active modem device.\r\n", NULL,
      do_mux_modem, NULL},

    { "bt", "select active bluetooth device",
      "'bt <instance index>': select active bluetooth device by instance for further config.\r\n"
      "'bt': display current active bluetooth device.\r\n", NULL,
      do_mux_bt, NULL},

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                           C B S   C O M M A N D                                 ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static int
do_cbs_sendpdu( ControlClient  client, char*  args )
{
    SmsPDU pdu;

    /* check that we have a phone number made of digits */
    if (!args) {
        control_write( client, "KO: missing argument, try 'cbs pdu <hexstring>'\r\n" );
        return -1;
    }

    if (!client->modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }

    pdu = cbspdu_create_from_hex( args, strlen(args) );
    if (pdu == NULL) {
        control_write( client, "KO: badly formatted <hexstring>\r\n" );
        return -1;
    }

    amodem_receive_cbs( client->modem, pdu );
    smspdu_free( pdu );
    return 0;
}

static const CommandDefRec  cbs_commands[] =
{
    { "pdu", "send inbound Cell Broadcast PDU",
    "'cbs pdu <hexstring>' allows you to simulate a new inbound Cell Broadcast PDU\r\n"
    "you probably love to play with this ;)\r\n", NULL,
    do_cbs_sendpdu, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                         R F K I L L    C O M M A N D S                          ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static const char* rfkill_type_names[RFKILL_TYPE_MAX] = {
    /* see hw/goldfish_bt.h, enum RfkillTypes */
    "wlan", "bluetooth", "uwb", "wimax", "wwan"
};

static RfkillTypes
get_rfkill_type_by_name( const char*  name, size_t  len )
{
    int type;

    for (type = 0; type < RFKILL_TYPE_MAX; type++) {
        if (!strncmp(name, rfkill_type_names[type], len)) {
            return type;
        }
    }

    return RFKILL_TYPE_MAX;
}

static int
do_rfkill_state( ControlClient  client, char*  args )
{
    char buf[32];
    const char *name;
    uint32_t blocking, hw_block, mask;
    int type, state;

    blocking = android_rfkill_get_blocking();
    hw_block = android_rfkill_get_hardware_block();

    if (args) {
        type = get_rfkill_type_by_name(args, strlen(args));
        if (type == RFKILL_TYPE_MAX) {
            control_write( client, "KO: unknown <type>\r\n" );
            return -1;
        }

        mask = RFKILL_TYPE_BIT(type);
        state = hw_block & mask ? 2 : (blocking & mask ? 0 : 1);
        snprintf(buf, sizeof buf, "%d\r\n", state);
        control_write( client, buf );
        return 0;
    }

    for (type = 0; type < RFKILL_TYPE_MAX; type++) {
        name = rfkill_type_names[type];
        mask = RFKILL_TYPE_BIT(type);
        state = hw_block & mask ? 2 : (blocking & mask ? 0 : 1);
        snprintf(buf, sizeof buf, "%s: %d\r\n", name, state);
        control_write( client, buf );
    }

    return 0;
}

static int
do_rfkill_block( ControlClient  client, char*  args )
{
    char *p;
    int type = RFKILL_TYPE_MAX;
    uint32_t hw_block;

    if (args) {
        p = strchr(args, ' ');
        int len = p ? (p - args) : strlen(args);
        type = get_rfkill_type_by_name(args, len);
    }

    if (type == RFKILL_TYPE_MAX) {
        control_write( client, "KO: unknown <type>\r\n" );
        return -1;
    }

    hw_block = android_rfkill_get_hardware_block();

    if (p) {
        while (*p && isspace(*p)) p++;

        if (!strcmp(p, "on")) {
            hw_block |= RFKILL_TYPE_BIT(type);
        } else if (!strcmp(p, "off")) {
            hw_block &= ~RFKILL_TYPE_BIT(type);
        } else {
            control_write( client, "KO: unknown <value>\r\n" );
            return -1;
        }
    } else {
        hw_block |= RFKILL_TYPE_BIT(type);
    }

    android_rfkill_set_hardware_block(hw_block);

    return 0;
}

static const CommandDefRec  rfkill_commands[] =
{
    { "state", "get current blocking state",
      "'rfkill state [<type>]' echo blocking status of all or specified <type>. '0' as\r\n"
      "software blocked, '1' as unblocked, '2' as hardware blocked.\r\n", NULL,
      do_rfkill_state, NULL },

    { "block", "set hardware block",
      "'rfkill block <type>[ <value>]' turn on/off hardware block on specified <type>.\r\n", NULL,
      do_rfkill_block, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                            N F C    C O M M A N D S                             ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

struct nfc_ndef_record_param {
    unsigned long flags;
    enum ndef_tnf tnf;
    const char* type;
    const char* id;
    const char* payload;
};

#define NFC_NDEF_PARAM_RECORD_INIT(_rec) \
    _rec = { \
        .flags = 0, \
        .tnf = 0, \
        .type = NULL, \
        .id = NULL, \
        .payload = NULL \
    }

ssize_t
build_ndef_msg(ControlClient client,
               const struct nfc_ndef_record_param* record, size_t nrecords,
               uint8_t* buf, size_t len)
{
    size_t off;
    size_t i;

    assert(record || !nrecords);
    assert(buf || !len);

    off = 0;

    for (i = 0; i < nrecords; ++i, ++record) {
        size_t idlen;
        uint8_t flags;
        struct ndef_rec* ndef;
        ssize_t res;

        idlen = strlen(record->id);

        flags = record->flags |
                ( NDEF_FLAG_MB * (!i) ) |
                ( NDEF_FLAG_ME * (i+1 == nrecords) ) |
                ( NDEF_FLAG_IL * (!!idlen) );

        ndef = (struct ndef_rec*)(buf + off);
        off += ndef_create_rec(ndef, flags, record->tnf, 0, 0, 0);

        /* decode type */
        res = decode_base64(record->type, strlen(record->type),
                            buf+off, len-off);
        if (res < 0) {
            return -1;
        }
        ndef_rec_set_type_len(ndef, res);
        off += res;

        if (flags & NDEF_FLAG_IL) {
            /* decode id */
            res = decode_base64(record->id, strlen(record->id),
                                buf+off, len-off);
            if (res < 0) {
                return -1;
            }
            ndef_rec_set_id_len(ndef, res);
            off += res;
        }

        /* decode payload */
        res = decode_base64(record->payload, strlen(record->payload),
                            buf+off, len-off);
        if (res < 0) {
            return -1;
        } else if ((res > 255) && (flags & NDEF_FLAG_SR)) {
            control_write(client,
                          "KO: NDEF flag SR set for long payload of %zu bytes",
                          res);
            return -1;
        }
        ndef_rec_set_payload_len(ndef, res);
        off += res;
    }
    return off;
}

struct nfc_snep_param {
    ControlClient client;
    long dsap;
    long ssap;
    size_t nrecords;
    struct nfc_ndef_record_param record[4];
};

#define NFC_SNEP_PARAM_INIT(_client) \
    { \
        .client = (_client), \
        .dsap = LLCP_SAP_LM, \
        .ssap = LLCP_SAP_LM, \
        .nrecords = 0, \
        .record = { \
            NFC_NDEF_PARAM_RECORD_INIT([0]), \
            NFC_NDEF_PARAM_RECORD_INIT([1]), \
            NFC_NDEF_PARAM_RECORD_INIT([2]), \
            NFC_NDEF_PARAM_RECORD_INIT([3]) \
        } \
    }

static ssize_t
create_snep_cp(void *data, size_t len, struct snep* snep)
{
    const struct nfc_snep_param* param;
    ssize_t res;

    param = data;
    assert(param);

    res = build_ndef_msg(param->client, param->record, param->nrecords,
                         snep->info, len-sizeof(*snep));
    if (res < 0) {
        return -1;
    }
    return snep_create_req_put(snep, res);
}

static ssize_t
nfc_send_snep_put_cb(void* data,
                     struct nfc_device* nfc,
                     size_t maxlen, union nci_packet* ntf)
{
    struct nfc_snep_param* param;
    ssize_t res;

    param = data;
    assert(param);

    if (!nfc->active_re) {
        control_write(param->client, "KO: no active remote endpoint\n");
        return -1;
    }
    if ((param->dsap < 0) && (param->ssap < 0)) {
        param->dsap = nfc->active_re->last_dsap;
        param->ssap = nfc->active_re->last_ssap;
    }
    res = nfc_re_send_snep_put(nfc->active_re, param->dsap, param->ssap,
                               create_snep_cp, data);
    if (res < 0) {
        control_write(param->client, "KO: 'snep put' failed\r\n");
        return -1;
    }
    return res;
}

static ssize_t
nfc_recv_process_ndef_cb(void* data, size_t len, const struct ndef_rec* ndef)
{
    const struct nfc_snep_param* param;
    ssize_t remain;
    char base64[3][512];

    param = data;
    assert(param);

    remain = len;

    control_write(param->client, "[");

    while (remain) {
        size_t tlen, plen, ilen, reclen;

        if (remain < sizeof(*ndef)) {
            return -1; /* too short */
        }
        tlen = encode_base64(ndef_rec_const_type(ndef),
                             ndef_rec_type_len(ndef),
                             base64[0], sizeof(base64[0]));
        ilen = encode_base64(ndef_rec_const_id(ndef), ndef_rec_id_len(ndef),
                             base64[1], sizeof(base64[1]));
        plen = encode_base64(ndef_rec_const_payload(ndef),
                             ndef_rec_payload_len(ndef),
                             base64[2], sizeof(base64[2]));

        /* print NDEF message in JSON format */
        control_write(param->client,
                      "{\"tnf\": %d,"
                      " \"type\": \"%.*s\","
                      " \"id\": \"%.*s\","
                      " \"payload\": \"%.*s\"}",
                      ndef->flags & NDEF_TNF_BITS,
                      tlen, base64[0], ilen, base64[1], plen, base64[2]);

        /* advance record */
        reclen = ndef_rec_len(ndef);
        remain -= reclen;
        ndef = (const struct ndef_rec*)(((const unsigned char*)ndef) + reclen);
        if (remain) {
          control_write(param->client, ","); /* more to come */
        }
    }
    control_write(param->client, "]\r\n");
    return 0;
}

static ssize_t
nfc_recv_snep_put_cb(void* data,  struct nfc_device* nfc)
{
    struct nfc_snep_param* param;
    ssize_t res;

    param = data;
    assert(param);

    if (!nfc->active_re) {
        control_write(param->client, "KO: no active remote endpoint\r\n");
        return -1;
    }
    if ((param->dsap < 0) && (param->ssap < 0)) {
        param->dsap = nfc->active_re->last_dsap;
        param->ssap = nfc->active_re->last_ssap;
    }
    res = nfc_re_recv_snep_put(nfc->active_re, param->dsap, param->ssap,
                               nfc_recv_process_ndef_cb, data);
    if (res < 0) {
        control_write(param->client, "KO: 'snep put' failed\r\n");
        return -1;
    }
    return 0;
}

static const char*
lex_token(ControlClient client, const char* field, const char* delim, char** args)
{
    const char *tok;

    assert(args);

    tok = strsep(args, delim);
    if (!tok) {
        control_write(client, "KO: no token %s given\r\n", field);
        return NULL;
    }
    return tok;
}

static int
parse_token_l(ControlClient client, const char* field, const char* delim,
              char** args, long* val)
{
    const char* tok;

    assert(val);

    tok = lex_token(client, field, delim, args);
    if (!tok) {
        return -1;
    }
    errno = 0;
    *val = strtol(tok, NULL, 0);
    if (errno) {
        control_write(client,
                      "KO: invalid value '%s' for token %s, error %d(%s)\r\n",
                      tok, field, errno, strerror(errno));
        return -1;
    }
    return 0;
}

static int
parse_token_ul(ControlClient client, const char* field, const char* delim,
               char** args, unsigned long* val)
{
    const char* tok;

    assert(val);

    tok = lex_token(client, field, delim, args);
    if (!tok) {
        return -1;
    }
    errno = 0;
    *val = strtoul(tok, NULL, 0);
    if (errno) {
        control_write(client,
                      "KO: invalid value '%s' for token %s, error %d(%s)\r\n",
                      tok, field, errno, strerror(errno));
        return -1;
    }
    return 0;
}

static int
parse_token_s(ControlClient client, const char* field, const char* delim,
              char** args, const char** val, int allow_empty)
{
    // TODO: we could add support for escaped characters, if necessary

    assert(val);

    *val = lex_token(client, field, delim, args);
    if (!*val) {
        return -1;
    }
    if (!allow_empty && !(*val)[0]) {
        control_write(client, "KO: empty token %s\r\n", field);
        return -1;
    }
    return 0;
}

static int
parse_sap(ControlClient client, const char* field,
          char** args, long* sap, int can_autodetect)
{
    assert(args);
    assert(sap);

    if (parse_token_l(client, field, " ", args, sap) < 0) {
        return -1;
    }
    if (((*sap == -1) && !can_autodetect) ||
         (*sap < -1) || !(*sap < LLCP_NUMBER_OF_SAPS)) {
        control_write(client, "KO: invalid %s '%ld'\r\n",
                      field, *sap);
        return -1;
    }
    return 0;
}

/* Each record is given by its flag bits, TNF value, type,
 * payload, and id. Id is optional. Type, payload, and id
 * are given in base64url encoding.
 */
static int
parse_ndef_rec(ControlClient client, char** args,
               struct nfc_ndef_record_param* record)
{
    const char* p;
    unsigned long tnf;

    assert(args);
    assert(record);

    /* read opening bracket */
    p = strsep(args, "[");
    if (!p) {
        control_write(client, "KO: no NDEF record given\r\n");
        return -1;
    }
    /* read flags */
    if (parse_token_ul(client, "NDEF flags", " ,", args, &record->flags) < 0) {
        return -1;
    }
    if (record->flags & ~NDEF_FLAG_BITS) {
        control_write(client, "KO: invalid NDEF flags '%u'\r\n",
                      record->flags);
        return -1;
    }
    /* read TNF */
    if (parse_token_ul(client, "NDEF TNF", " ,", args, &tnf) < 0) {
        return -1;
    }
    if (!(tnf < NDEF_NUMBER_OF_TNFS)) {
        control_write(client, "KO: invalid NDEF TNF '%u'\r\n",
                      record->tnf);
        return -1;
    }
    record->tnf = tnf;
    /* read type */
    if (parse_token_s(client, "NDEF type", " ,", args, &record->type, 0) < 0) {
        return -1;
    }
    /* read id; might by empty */
    if (parse_token_s(client, "NDEF id", " ,", args, &record->id, 1) < 0) {
        return -1;
    }
    /* read payload */
    if (parse_token_s(client, "NDEF payload", "]", args, &record->payload, 0) < 0) {
        return -1;
    }
    return 0;
}

static ssize_t
parse_ndef_msg(ControlClient client, char** args, size_t nrecs,
               struct nfc_ndef_record_param* rec)
{
    size_t i;

    assert(args);

    for (i = 0; i < nrecs && *args && strlen(*args); ++i) {
        if (parse_ndef_rec(client, args, rec+i) < 0) {
          return -1;
        }
    }
    if (*args && strlen(*args)) {
        control_write(client,
                      "KO: invalid characters near EOL: %s\r\n",
                      *args);
        return -1;
    }
    return i;
}

static int
parse_re_index(ControlClient client, char** args, unsigned long nres,
               unsigned long* i)
{
    assert(i);

    if (parse_token_ul(client, "remote endpoint", " ", args, i) < 0) {
        return -1;
    }
    if (!(*i < nres)) {
        control_write(client, "KO: unknown remote endpoint %lu\r\n", *i);
        return -1;
    }
    return 0;
}

static int
parse_nci_ntf_type(ControlClient client, char** args, unsigned long* ntype)
{
    assert(ntype);

    if (parse_token_ul(client, "discover notification type", " ", args, ntype) < 0) {
        return -1;
    }
    if (!(*ntype < NUMBER_OF_NCI_NOTIFICATION_TYPES)) {
        control_write(client, "KO: unknown discover notification type %lu\r\n", *ntype);
        return -1;
    }
    return 0;
}

static int
parse_rf_index(ControlClient client, char** args, long* rf)
{
    assert(rf);

    if (parse_token_l(client, "rf index", " ", args, rf) < 0) {
        return -1;
    }
    if (*rf < -1 || *rf >= NUMBER_OF_SUPPORTED_NCI_RF_INTERFACES) {
        control_write(client, "KO: unknown rf index %lu\r\n", *rf);
        return -1;
    }
    return 0;
}

static int
parse_nci_deactivate_ntf_type(ControlClient client, char** args, unsigned long* dtype)
{
    assert(dtype);

    if (parse_token_ul(client, "deactivate notification type", " ", args, dtype) < 0) {
        return -1;
    }
    if (!(*dtype < NUMBER_OF_NCI_RF_DEACT_TYPE)) {
        control_write(client, "KO: unknown deactivate notification type %lu\r\n", *dtype);
        return -1;
    }
    return 0;
}

static int
parse_nci_deactivate_ntf_reason(ControlClient client, char** args, unsigned long* dreason)
{
    assert(dreason);

    if (parse_token_ul(client, "deactivate notification reason", " ", args, dreason) < 0) {
        return -1;
    }
    if (!(*dreason < NUMBER_OF_NCI_RF_DEACT_REASON)) {
        control_write(client, "KO: unknown deactivate notification reason %lu\r\n", *dreason);
        return -1;
    }
    return 0;
}

static int
do_nfc_snep( ControlClient  client, char*  args )
{
    char *p;

    if (!args) {
        control_write(client, "KO: no arguments given\r\n");
        return -1;
    }

    p = strsep(&args, " ");
    if (!p) {
        control_write(client, "KO: no operation given\r\n");
        return -1;
    }
    if (!strcmp(p, "put")) {
        ssize_t nrecords;
        struct nfc_snep_param param = NFC_SNEP_PARAM_INIT(client);

        /* read DSAP */
        if (parse_sap(client, "DSAP", &args, &param.dsap, 1) < 0) {
            return -1;
        }
        /* read SSAP */
        if (parse_sap(client, "SSAP", &args, &param.ssap, 1) < 0) {
            return -1;
        }
        /* The emulator supports up to 4 records per NDEF
         * message. If no records are given, the emulator
         * will print the current content of the LLCP data-
         * link buffer.
         */
        nrecords = parse_ndef_msg(client, &args, ARRAY_SIZE(param.record),
                                  param.record);
        if (nrecords < 0) {
            return -1;
        }
        param.nrecords = nrecords;
        if (param.nrecords) {
            /* put SNEP request onto SNEP server */
            if (goldfish_nfc_send_dta(nfc_send_snep_put_cb, &param) < 0) {
                /* error message generated in create function */
                return -1;
            }
        } else {
            /* put SNEP request onto SNEP server */
            if (goldfish_nfc_recv_dta(nfc_recv_snep_put_cb, &param) < 0) {
                /* error message generated in create function */
                return -1;
            }
        }
    } else {
        control_write(client, "KO: invalid operation '%s'\r\n", p);
        return -1;
    }

    return 0;
}

struct nfc_ntf_param {
    ControlClient client;
    struct nfc_re* re;
    unsigned long ntype;
    long rf;
    unsigned long dreason;
    unsigned long dtype;
};

#define NFC_NTF_PARAM_INIT(_client) \
    { \
      .client = (_client), \
      .re = NULL, \
      .ntype = 0, \
      .rf = -1, \
      .dreason = 0, \
      .dtype = 0 \
    }

static ssize_t
nfc_rf_discovery_ntf_cb(void* data,
                        struct nfc_device* nfc, size_t maxlen,
                        union nci_packet* ntf)
{
    ssize_t res;
    const struct nfc_ntf_param* param = data;
    res = nfc_create_rf_discovery_ntf(param->re, param->ntype, nfc, ntf);
    if (res < 0) {
        control_write(param->client, "KO: rf_discover_ntf failed\r\n");
        return -1;
    }
    return res;
}

static ssize_t
nfc_rf_intf_activated_ntf_cb(void* data,
                             struct nfc_device* nfc, size_t maxlen,
                             union nci_packet* ntf)
{
    size_t res;
    struct nfc_ntf_param* param = data;
    if (!param->re) {
        if (!nfc->active_re) {
            control_write(param->client, "KO: no active remote-endpoint\n");
            return -1;
        }
        param->re = nfc->active_re;
    }
    nfc_clear_re(param->re);
    if (nfc->active_rf) {
        // Already select an active rf interface,so do nothing.
    } else if (param->rf == -1) {
        // Auto select active rf interface based on remote-endpoint protocol and mode.
        nfc->active_rf = nfc_find_rf_by_protocol_and_mode(nfc,
                                                          param->re->rfproto,
                                                          param->re->mode);
        if (!nfc->active_rf) {
            control_write(param->client, "KO: no active rf interface\r\n");
            return -1;
        }
    } else {
        nfc->active_rf = nfc->rf + param->rf;
    }

    res = nfc_create_rf_intf_activated_ntf(param->re, nfc, ntf);
    if (res < 0) {
        control_write(param->client, "KO: rf_intf_activated_ntf failed\r\n");
        return -1;
    }
    return res;
}

static ssize_t
nfc_rf_intf_deactivate_ntf_cb(void* data,
                              struct nfc_device* nfc, size_t maxlen,
                              union nci_packet* ntf)
{
    ssize_t res;
    struct nfc_ntf_param* param = data;

    assert(data);
    assert(nfc);

    res = nfc_create_deactivate_ntf(param->dtype, param->dreason, ntf);
    if (res < 0) {
        control_write(param->client, "KO: rf_intf_deactivate_ntf failed\r\n");
        return -1;
    }
    return res;
}

static int
do_nfc_nci( ControlClient  client, char*  args )
{
    char *p;

    if (!args) {
        control_write(client, "KO: no arguments given\r\n");
        return -1;
    }

    /* read notification type */
    p = strsep(&args, " ");
    if (!p) {
        control_write(client, "KO: no operation given\r\n");
        return -1;
    }
    if (!strcmp(p, "rf_discover_ntf")) {
        unsigned long i;
        struct nfc_ntf_param param = NFC_NTF_PARAM_INIT(client);
        /* read remote-endpoint index */
        if (parse_re_index(client, &args, ARRAY_SIZE(nfc_res), &i) < 0) {
            return -1;
        }
        param.re = nfc_res + i;

        /* read discover notification type */
        if (parse_nci_ntf_type(client, &args, &param.ntype) < 0) {
            return -1;
        }

        /* generate RF_DISCOVER_NTF */
        if (goldfish_nfc_send_ntf(nfc_rf_discovery_ntf_cb, &param) < 0) {
            /* error message generated in create function */
            return -1;
        }
    } else if (!strcmp(p, "rf_intf_activated_ntf")) {
        struct nfc_ntf_param param = NFC_NTF_PARAM_INIT(client);
        if (args && *args) {
            unsigned long i;
            /* read remote-endpoint index */
            if (parse_re_index(client, &args, ARRAY_SIZE(nfc_res), &i) < 0) {
                return -1;
            }
            param.re = nfc_res + i;

            if (args && *args) {
                /* read rf interface index */
                if (parse_rf_index(client, &args, &param.rf) < 0) {
                    return -1;
                }
            } else {
                param.rf = -1;
            }
        } else {
            param.re = NULL;
            param.rf = -1;
        }
        /* generate RF_INTF_ACTIVATED_NTF; if param.re == NULL,
         * active RE will be used */
        if (goldfish_nfc_send_ntf(nfc_rf_intf_activated_ntf_cb, &param) < 0) {
            /* error message generated in create function */
            return -1;
        }
    } else if (!strcmp(p, "rf_intf_deactivate_ntf")) {
        struct nfc_ntf_param param = NFC_NTF_PARAM_INIT(client);
        if (args && *args) {
            /* read deactivate ntf type */
            if (parse_nci_deactivate_ntf_type(client, &args, &param.dtype) < 0) {
                return -1;
            }
            /* read deactivate ntf reason */
            if (parse_nci_deactivate_ntf_reason(client, &args, &param.dreason) < 0) {
                return -1;
            }
        } else {
            param.dtype = NCI_RF_DEACT_DISCOVERY;
            param.dreason = NCI_RF_DEACT_RF_LINK_LOSS;
        }
        if (goldfish_nfc_send_ntf(nfc_rf_intf_deactivate_ntf_cb, &param) < 0) {
            /* error message generated in create function */
            return -1;
        }
    } else {
        control_write(client, "KO: invalid operation '%s'\r\n", p);
        return -1;
    }

    return 0;
}

struct nfc_llcp_param {
    ControlClient client;
    long dsap;
    long ssap;
};

#define NFC_LLCP_PARAM_INIT(_client) \
    { \
        .client = (_client), \
        .dsap = 0, \
        .ssap = 0 \
    }

static ssize_t
nfc_llcp_connect_cb(void* data, struct nfc_device* nfc, size_t maxlen,
                    union nci_packet* packet)
{
    struct nfc_llcp_param* param = data;
    ssize_t res;

    if (!nfc->active_re) {
        control_write(param->client, "KO: no active remote endpoint\n");
        return -1;
    }
    if ((param->dsap < 0) && (param->ssap < 0)) {
        param->dsap = nfc->active_re->last_dsap;
        param->ssap = nfc->active_re->last_ssap;
    }
    if (!param->dsap) {
        control_write(param->client, "KO: DSAP is 0\r\n");
        return -1;
    }
    if (!param->ssap) {
        control_write(param->client, "KO: SSAP is 0\r\n");
        return -1;
    }
    res = nfc_re_send_llcp_connect(nfc->active_re, param->dsap, param->ssap);
    if (res < 0) {
        control_write(param->client, "KO: LLCP connect failed\r\n");
        return -1;
    }
    return 0;
}

static int
do_nfc_llcp( ControlClient  client, char*  args )
{
    char *p;

    if (!args) {
        control_write(client, "KO: no arguments given\r\n");
        return -1;
    }

    p = strsep(&args, " ");
    if (!p) {
        control_write(client, "KO: no operation given\r\n");
        return -1;
    }
    if (!strcmp(p, "connect")) {
        struct nfc_llcp_param param = NFC_LLCP_PARAM_INIT(client);

        /* read DSAP */
        if (parse_sap(client, "DSAP", &args, &param.dsap, 1) < 0) {
            return -1;
        }
        /* read SSAP */
        if (parse_sap(client, "SSAP", &args, &param.ssap, 1) < 0) {
            return -1;
        }
        if (goldfish_nfc_send_dta(nfc_llcp_connect_cb, &param) < 0) {
            /* error message generated in create function */
            return -1;
        }
    } else {
        control_write(client, "KO: invalid operation '%s'\r\n", p);
        return -1;
    }

    return 0;
}

static int
do_nfc_tag( ControlClient client, char*  args )
{
    char *p;

    if (!args) {
        control_write(client, "KO: no arguments given\r\n");
        return -1;
    }

    p = strsep(&args, " ");
    if (!p) {
        control_write(client, "KO: no operation given\r\n");
        return -1;
    }
    if (!strcmp(p, "set")) {
        unsigned long i;
        ssize_t res;
        ssize_t nrecords;
        struct nfc_ndef_record_param record[4];
        struct nfc_re* re;
        uint8_t buf[MAXIMUM_SUPPORTED_TAG_SIZE];

        /* read remote-endpoint index */
        if (parse_re_index(client, &args, ARRAY_SIZE(nfc_res), &i) < 0) {
            return -1;
        }
        re = nfc_res + i;

        if (!re->tag) {
            control_write(client, "KO: remote endpoint is not a tag\r\n");
            return -1;
        }

        nrecords = parse_ndef_msg(client, &args, ARRAY_SIZE(record), record);
        if (nrecords < 0) {
            return -1;
        }

        res = build_ndef_msg(client, record, nrecords, buf, ARRAY_SIZE(buf));
        if (res < 0) {
            return -1;
        }

        if (nfc_tag_set_data(re->tag, buf, res) < 0) {
            return -1;
        }
    } else if (!strcmp(p, "clear")) {
        unsigned long i;
        struct nfc_re* re;

        /* read remote-endpoint index */
        if (parse_re_index(client, &args, ARRAY_SIZE(nfc_res), &i) < 0) {
            return -1;
        }
        re = nfc_res + i;

        if (nfc_tag_set_data(re->tag, NULL, 0) < 0) {
            return -1;
        }
    } else if (!strcmp(p, "format")) {
        unsigned long i;
        struct nfc_re* re;

        /* read remote-endpoint index */
        if (parse_re_index(client, &args, ARRAY_SIZE(nfc_res), &i) < 0) {
            return -1;
        }
        re = nfc_res + i;

        if (nfc_tag_format(re->tag) < 0) {
            return -1;
        }
    }

    return 0;
}
static const CommandDefRec  nfc_commands[] =
{
    { "nci", "send NCI notification",
      "'nfc nci rf_discover_ntf <i> <type>' send RC_DISCOVER_NTF for Remote Endpoint <i> with notification type <type>\r\n"
      "'nfc nci rf_intf_activated_ntf' send RC_DISCOVER_NTF for selected Remote Endpoint\r\n"
      "'nfc nci rf_intf_activated_ntf <i>' send RC_DISCOVER_NTF for Remote Endpoint <i>, auto detect rf\r\n"
      "'nfc nci rf_intf_activated_ntf <i> -1' send RC_DISCOVER_NTF for Remote Endpoint <i>, auto detect rf\r\n"
      "'nfc nci rf_intf_activated_ntf <i> <j>' send RC_DISCOVER_NTF for Remote Endpoint <i> & RF interface <j>\r\n"
      "'nfc nci rf_intf_deactivate_ntf' send RC_DEACTIVATE_NTF with default deactivate type & reason\r\n"
      "'nfc nci rf_intf_deactivate_ntf <type> <reason>' send RC_DEACTIVATE_NTF with deactivate type <type> & reason <reason>\r\n",
      NULL,
      do_nfc_nci, NULL },

    { "snep", "put and read NDEF messages",
      "'nfc snep put <dsap> <ssap> <[<flags>,<tnf>,<type>,<id>,<payload>]>' sends NDEF records of the given parameters\r\n"
      "'nfc snep put <dsap> <ssap> <[<flags>,<tnf>,<type>,,<payload>]>' sends NDEF records of the given parameters without ID field\r\n",
      NULL,
      do_nfc_snep, NULL },

    { "llcp", "internal LLCP handling",
      "'nfc llcp connect <dsap> <ssap>' connects active Remote Endpoint's SSAP to host's SSAP\r\n",
      NULL,
      do_nfc_llcp, NULL },

    { "tag", "data handling",
      "'nfc tag set <i> <[<flags>,<tnf>,<type>,,<payload>]>' set NDEF data to Remote Endpoint <i>\r\n"
      "'nfc tag clear <i>' clear tag data of Remote Endpoint <i>\r\n"
      "'nfc tag format <i>' format tag data of Remote Endpoint <i>\r\n",
      NULL,
      do_nfc_tag, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                         M O D E M   C O M M A N D                               ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static void
help_modem_tech( ControlClient  client )
{
    int  nn;
    control_write( client,
            "'modem tech': allows you to display the current state of emulator modem.\r\n"
            "'modem tech <technology>': allows you to change the technology of emulator modem.\r\n"
            "'modem tech <technology> <mask>': allows you to change the technology and preferred mask of emulator modem.\r\n\r\n"
            "valid values for <technology> are the following:\r\n\r\n" );

    for (nn = 0; ; nn++) {
        const char* name = android_get_modem_tech_name(nn);

        if (!name) {
            break;
        }

        control_write(client, "  %s\r\n", name);
    }

    control_write(client, "\r\nvalid values for <mask> are the following:\r\n\r\n");

    for (nn = 0; ; nn++) {
        const char* name = android_get_modem_preferred_mask_name(nn);

        if (!name) {
            break;
        }

        control_write(client, "  %s\r\n", name);
    }
}

static int do_modem_tech_query( ControlClient client, char* args )
{
    AModemPreferredMask mask = amodem_get_preferred_mask(client->modem);
    AModemTech technology = amodem_get_technology(client->modem);

    control_write(client, "%s %s\r\n", android_get_modem_tech_name(technology),
                                       android_get_modem_preferred_mask_name(mask));
    return 0;
}

static int
do_modem_tech( ControlClient client, char* args )
{
    char* pnext  = NULL;
    AModemTech tech = A_TECH_UNKNOWN;
    AModemPreferredMask mask = A_PREFERRED_MASK_UNKNOWN;

    if (!client->modem) {
        control_write(client, "KO: modem emulation not running\r\n");
        return -1;
    }

    if (!args) {
        return do_modem_tech_query(client, args);
    }

    // Parse <technology>
    pnext = strchr(args, ' ');
    if (pnext != NULL) {
        *pnext++ = '\0';
        while (*pnext && isspace(*pnext)) pnext++;
    }

    tech = android_parse_modem_tech(args);

    if (tech == A_TECH_UNKNOWN) {
        control_write(client, "KO: bad modem technology name, try 'help modem tech' for list of valid values\r\n");
        return -1;
    }

    // Parse <mask>
    if (pnext && *pnext) {
        mask = android_parse_modem_preferred_mask(pnext);
    }

    if (amodem_set_technology(client->modem, tech, mask)) {
        control_write(client, "KO: unable to set modem technology to '%s'\r\n", args);
        return -1;
    }

    return 0;
}

static int
do_modem_radio( ControlClient client, char *args )
{
    if (!client->modem) {
        control_write(client, "KO: modem emulation not running\r\n");
        return -1;
    }

    if (!args) {
        ARadioState state = amodem_get_radio_state(client->modem);
        control_write(client, "%s\r\n",
                      state == A_RADIO_STATE_OFF ? "disabled" : "enabled");
        return 0;
    }

    if (!strcmp(args, "enable")) {
        amodem_set_radio_state(client->modem, A_RADIO_STATE_ON);
        return 0;
    }

    if (!strcmp(args, "disable")) {
        amodem_set_radio_state(client->modem, A_RADIO_STATE_OFF);
        return 0;
    }

    control_write(client, "KO: invalid argument, try 'modem radio [enable|disable]'\r\n");
    return -1;
}

static int do_modem_dtmf( ControlClient client, char* args )
{
    if (!client->modem) {
        control_write(client, "KO: modem emulation not running\r\n");
        return -1;
    }

    if (!args) {
      char tone = amodem_get_last_dialed_tone(client->modem);
      control_write(client, "%c\r\n", tone);
      return 0;
    }

    if (!strcmp(args, "reset")) {
      amodem_reset_last_dialed_tone(client->modem);
      return 0;
    }

    control_write(client,
                  "KO: invalid argument, try 'modem dtmf [reset]'\r\n");
    return -1;
}

static const CommandDefRec  modem_commands[] =
{
    { "tech", "query/switch modem technology",
      NULL, help_modem_tech,
      do_modem_tech, NULL },

    { "radio", "query/switch radio state",
      "'modem radio': allows you to display the current radio state of emulator modem.\r\n"
      "'modem radio enable': allows you to enable the radio of emulator modem.\r\n"
      "'modem radio disable': allows you to disable the radio of emulator modem.\r\n",
      NULL, do_modem_radio, NULL },

    { "dtmf", "get or reset the last dialed tone",
      NULL, NULL, do_modem_dtmf, NULL},

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                      B L U E T O O T H   C O M M A N D S                        ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static int
validate_bt_args_local( ControlClient         client,
                        char                 *args,
                        struct bt_device_s  **dev )
{
    if (!client->bt) {
        control_write(client, "KO: bluetooth emulation not running\r\n");
        return -1;
    }

    *dev = abluetooth_get_bt_device(client->bt);
    if (!*dev) {
        control_write(client, "KO: local device is not configurable\r\n");
        return -1;
    }

    return 0;
}

static int
validate_bt_args_bdaddr( ControlClient        client,
                         char                *args,
                         struct bt_device_s **dev,
                         bdaddr_t            *addr,
                         const bdaddr_t      *default_addr )
{
    char *p;

    if (validate_bt_args_local(client, args, dev) < 0) {
        return -1;
    }

    if (!args || !(p = strtok(args, " "))) {
        if (!default_addr) {
            control_write(client, "KO: missing bluetooth address\r\n");
            return -1;
        }
        bacpy(addr, default_addr);
    } else if (ba_from_str(addr, p)) {
        control_write(client, "KO: invalid bluetooth address\r\n");
        return -1;
    }

    return 0;
}

static int
validate_bt_args_device( ControlClient         client,
                         char                 *args,
                         struct bt_device_s  **local,
                         struct bt_device_s  **dev,
                         bdaddr_t             *addr,
                         const bdaddr_t       *default_addr,
                         int                   accept_all )
{
    if (validate_bt_args_bdaddr(client, args, local, addr, default_addr) < 0) {
        return -1;
    }

    if ((accept_all && !bacmp(addr, BDADDR_ALL)) ||
        !bacmp(addr, BDADDR_LOCAL) ||
        !bacmp(addr, &(*local)->bd_addr)) {
        *dev = *local;
        return 0;
    }

    *dev = bt_scatternet_find_slave((*local)->net, addr);
    if (!*dev) {
        control_write(client, "KO: device not found\r\n");
        return -1;
    }

    return 0;
}

static int
do_bt_remote_add( ControlClient client, char* args )
{
    struct bt_device_s *dev;
    bdaddr_t addr;
    char buf[BDADDR_BUF_LEN];

    if (validate_bt_args_local(client, args, &dev) < 0) {
        return -1;
    }

    // Create device within dev->net;
    dev = bt_remote_device_new(dev->net);
    if (!dev) {
        control_write(client, "KO: failed to create remote device\r\n");
        return -1;
    }

    ba_to_str(buf, &dev->bd_addr);
    control_write(client, "%s\r\n", buf);
    return 0;
}

static int
do_bt_remote_remove_device( ControlClient        client,
                            struct bt_device_s  *dev,
                            int                  fatal )
{
    char buf[BDADDR_BUF_LEN];

    if (bt_device_get_property(dev, "is_remote", NULL, 0) < 0) {
        if (fatal) {
            control_write(client, "KO: not a remote device\r\n");
            return -1;
        }
        return 0;
    }

    ba_to_str(buf, &dev->bd_addr);
    dev->handle_destroy(dev);
    control_write(client, "%s\r\n", buf);

    return 0;
}

static void
do_bt_remote_remove_scatternet( ControlClient            client,
                                struct bt_scatternet_s  *net )
{
    struct bt_device_s *dev, *next;

    dev = net->slave;
    while (dev) {
        next = dev->next;
        do_bt_remote_remove_device(client, dev, 0);
        dev = next;
    }
}

static int
do_bt_remote_remove( ControlClient client, char* args )
{
    struct bt_device_s *local, *dev;
    bdaddr_t addr;

    if (validate_bt_args_device(client, args, &local, &dev,
                   &addr, NULL, 1) < 0) {
        return -1;
    }

    if (!bacmp(&addr, BDADDR_ALL)) {
        // Remove all remote devices of current scatter net.
        do_bt_remote_remove_scatternet(client, local->net);
        return 0;
    }

    // Remove only one device.
    return do_bt_remote_remove_device(client, dev, 1);
}

static const CommandDefRec bt_remote_commands[] =
{
    { "add", "add virtual Bluetooth remote device",
    "'bt remote add':\r\n"
    "Add a remote device to the scatternet where the local device lives and return\r\n"
    "the address of the newly created device.\r\n",
    NULL, do_bt_remote_add, NULL },

    { "remove", "remove virtual Bluetooth remote device",
    "'bt remove <bd_addr>':\r\n"
    "Remove the remote device(s) specified by <bd_addr>.  Use Bluetooth ALL address\r\n"
    "ff:ff:ff:ff:ff:ff to remove all remote devices the scatternet.\r\n",
    NULL, do_bt_remote_remove, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

static void
do_bt_list_device( ControlClient        client,
                   struct bt_device_s  *local,
                   struct bt_device_s  *dev )
{
    char buf[BDADDR_BUF_LEN], type;

    if (dev == local) {
        type = '*';
    } else if (bt_device_get_property(dev, "is_remote", NULL, 0) >= 0) {
        type = 'R';
    } else {
        type = 'L';
    }
    ba_to_str(buf, &dev->bd_addr);
    control_write(client, "%c %s\r\n", type, buf);
}

static int
do_bt_list( ControlClient client, char* args )
{
    struct bt_device_s *local, *dev;
    bdaddr_t addr;

    if (validate_bt_args_device(client, args, &local, &dev,
                     &addr, BDADDR_ALL, 1) < 0) {
        return -1;
    }

    if (!bacmp(&addr, BDADDR_ALL)) {
        dev = local->net->slave;
        while (dev) {
           do_bt_list_device(client, local, dev);
           dev = dev->next;
        }

        return 0;
    }

    do_bt_list_device(client, local, dev);
    return 0;
}

static int
enum_prop_callback(void *opaque, const char *prop, const char *value)
{
    control_write((ControlClient)opaque, "%s: %s\r\n", prop, value);
    return 0;
}

static int
do_bt_property( ControlClient client, char* args )
{
    struct bt_device_s *local, *dev;
    bdaddr_t addr;
    char *p, *v;

    if (validate_bt_args_device(client, args, &local, &dev,
                    &addr, BDADDR_LOCAL, 0) < 0) {
        return -1;
    }

    p = strtok(NULL, " ");
    if (!p) {
        return bt_device_enumerate_properties(dev, enum_prop_callback, client);
    }

    v = strtok(NULL, " ");
    if (!v) {
        char buf[1024];
        if (bt_device_get_property(dev, p, buf, sizeof buf) < 0) {
            control_write(client, "KO: invalid property '%s'\r\n", p);
            return -1;
        }

        control_write(client, "%s: %s\r\n", p, buf);
        return 0;
    }

    if (bt_device_set_property(dev, p, v) < 0) {
        control_write(client, "KO: invalid property '%s' or value '%s'\r\n", p, v);
        return -1;
    }

    return 0;
}

static const CommandDefRec bt_commands[] =
{
    { "list", "list scatternet devices",
    "'bt list [<bd_addr>]':\r\n"
    "List a device within the same scatternet with current local device. If <bd_addr>\r\n"
    "is omitted, Bluetooth ALL address ff:ff:ff:ff:ff:ff is assumed.\r\n",
    NULL, do_bt_list, NULL },

    { "property", "get/set device property",
    "'bt property [<bd_addr> [<prop> [value]]]':\r\n"
    "Set property <prop> on device <bd_addr> to <value>.\r\n"
    "If <value> is omitted, show the property <prop> of device <bd_addr>.\r\n"
    "If both <prop> and <value> are omitted, enumerate all properties of device\r\n"
    "<bd_addr>.  If <bd_addr> is also omitted, Bluetooth LOCAL address\r\n"
    "ff:ff:ff:00:00:00 is assumed.\r\n",
    NULL, do_bt_property, NULL },

    { "remote", "manage Bluetooth virtual remote devices", NULL,
    NULL, NULL, bt_remote_commands },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                           M A I N   C O M M A N D S                             ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static int
do_window_scale( ControlClient  client, char*  args )
{
    double  scale;
    int     is_dpi = 0;
    char*   end;

    if (!args) {
        control_write( client, "KO: argument missing, try 'window scale <scale>'\r\n" );
        return -1;
    }

    scale = strtol( args, &end, 10 );
    if (end > args && !memcmp( end, "dpi", 4 )) {
        is_dpi = 1;
    }
    else {
        scale = strtod( args, &end );
        if (end == args || end[0]) {
            control_write( client, "KO: argument <scale> must be a real number, or an integer followed by 'dpi'\r\n" );
            return -1;
        }
    }

    android_emulator_set_window_scale(scale, is_dpi);
    return 0;
}

static const CommandDefRec  window_commands[] =
{
    { "scale", "change the window scale",
    "'window scale <scale>' allows you to change the scale of the emulator window at runtime\r\n"
    "<scale> must be either a real number between 0.1 and 3.0, or an integer followed by\r\n"
    "the 'dpi' prefix (as in '120dpi')\r\n",
    NULL, do_window_scale, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                           Q E M U   C O M M A N D S                             ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static int
do_qemu_monitor( ControlClient client, char* args )
{
    control_write(client, "KO: QEMU support no longer available\r\n");
    return -1;
}

#ifdef CONFIG_STANDALONE_CORE
/* UI settings, passed to the core via -ui-settings command line parameter. */
extern char* android_op_ui_settings;

static int
do_attach_ui( ControlClient client, char* args )
{
    // Make sure that there are no UI already attached to this console.
    if (attached_ui_client != NULL) {
        control_write( client, "KO: Another UI is attached to this core!\r\n" );
        control_client_destroy(client);
        return -1;
    }

    if (!attachUiProxy_create(client->sock)) {
        char reply_buf[4096];
        attached_ui_client = client;
        // Reply "OK" with the saved -ui-settings property.
        snprintf(reply_buf, sizeof(reply_buf), "OK: %s\r\n", android_op_ui_settings);
        control_write( client, reply_buf);
    } else {
        control_write( client, "KO\r\n" );
        control_client_destroy(client);
        return -1;
    }

    return 0;
}

void
destroy_attach_ui_client(void)
{
    if (attached_ui_client != NULL) {
        control_client_destroy(attached_ui_client);
    }
}

static int
do_create_framebuffer_service( ControlClient client, char* args )
{
    ProxyFramebuffer* core_fb;
    const char* protocol = "-raw";   // Default framebuffer exchange protocol.
    char reply_buf[64];

    // Protocol type is defined by the arguments passed with the stream switch
    // command.
    if (args != NULL && *args != '\0') {
        size_t token_len;
        const char* param_end = strchr(args, ' ');
        if (param_end == NULL) {
            param_end = args + strlen(args);
        }
        token_len = param_end - args;
        protocol = args;

        // Make sure that this is one of the supported protocols.
        if (strncmp(protocol, "-raw", token_len) &&
            strncmp(protocol, "-shared", token_len)) {
            derror("Invalid framebuffer parameter %s\n", protocol);
            control_write( client, "KO: Invalid parameter\r\n" );
            control_client_destroy(client);
            return -1;
        }
    }

    core_fb = proxyFb_create(client->sock, protocol);
    if (core_fb == NULL) {
        control_write( client, "KO\r\n" );
        control_client_destroy(client);
        return -1;
    }

    // Reply "OK" with the framebuffer's bits per pixel
    snprintf(reply_buf, sizeof(reply_buf), "OK: -bitsperpixel=%d\r\n",
             proxyFb_get_bits_per_pixel(core_fb));
    control_write( client, reply_buf);
    return 0;
}

static int
do_create_user_events_service( ControlClient client, char* args )
{
    // Make sure that there are no user events client already existing.
    if (user_events_client != NULL) {
        control_write( client, "KO: Another user events service is already existing!\r\n" );
        control_client_destroy(client);
        return -1;
    }

    if (!userEventsImpl_create(client->sock)) {
        char reply_buf[4096];
        user_events_client = client;
        snprintf(reply_buf, sizeof(reply_buf), "OK\r\n");
        control_write( client, reply_buf);
    } else {
        control_write( client, "KO\r\n" );
        control_client_destroy(client);
        return -1;
    }

    return 0;
}

void
destroy_user_events_client(void)
{
    if (user_events_client != NULL) {
        control_client_destroy(user_events_client);
    }
}

static int
do_create_ui_core_ctl_service( ControlClient client, char* args )
{
    // Make sure that there are no ui control client already existing.
    if (ui_core_ctl_client != NULL) {
        control_write( client, "KO: Another UI control service is already existing!\r\n" );
        control_client_destroy(client);
        return -1;
    }

    if (!coreCmdImpl_create(client->sock)) {
        char reply_buf[4096];
        ui_core_ctl_client = client;
        snprintf(reply_buf, sizeof(reply_buf), "OK\r\n");
        control_write( client, reply_buf);
    } else {
        control_write( client, "KO\r\n" );
        control_client_destroy(client);
        return -1;
    }

    return 0;
}

void
destroy_ui_core_ctl_client(void)
{
    if (ui_core_ctl_client != NULL) {
        control_client_destroy(ui_core_ctl_client);
    }
}

void
destroy_corecmd_client(void)
{
    if (ui_core_ctl_client != NULL) {
        control_client_destroy(ui_core_ctl_client);
    }
}

static int
do_create_core_ui_ctl_service( ControlClient client, char* args )
{
    // Make sure that there are no ui control client already existing.
    if (core_ui_ctl_client != NULL) {
        control_write( client, "KO: Another UI control service is already existing!\r\n" );
        control_client_destroy(client);
        return -1;
    }

    if (!uiCmdProxy_create(client->sock)) {
        char reply_buf[4096];
        core_ui_ctl_client = client;
        snprintf(reply_buf, sizeof(reply_buf), "OK\r\n");
        control_write( client, reply_buf);
    } else {
        control_write( client, "KO\r\n" );
        control_client_destroy(client);
        return -1;
    }

    return 0;
}

void
destroy_core_ui_ctl_client(void)
{
    if (core_ui_ctl_client != NULL) {
        control_client_destroy(core_ui_ctl_client);
    }
}

void
destroy_uicmd_client(void)
{
    if (core_ui_ctl_client != NULL) {
        control_client_destroy(core_ui_ctl_client);
    }
}

#endif  // CONFIG_STANDALONE_CORE

static const CommandDefRec  qemu_commands[] =
{
    { "monitor", "enter QEMU monitor",
    "Enter the QEMU virtual machine monitor\r\n",
    NULL, do_qemu_monitor, NULL },

#ifdef CONFIG_STANDALONE_CORE
    { "attach-UI", "attach UI to the core",
    "Attach UI to the core\r\n",
    NULL, do_attach_ui, NULL },

    { "framebuffer", "create framebuffer service",
    "Create framebuffer service\r\n",
    NULL, do_create_framebuffer_service, NULL },

    { "user-events", "create user events service",
    "Create user events service\r\n",
    NULL, do_create_user_events_service, NULL },

    { "ui-core-control", "create UI control service",
    "Create UI control service\r\n",
    NULL, do_create_ui_core_ctl_service, NULL },

    { "core-ui-control", "create UI control service",
    "Create UI control service\r\n",
    NULL, do_create_core_ui_ctl_service, NULL },
#endif  // CONFIG_STANDALONE_CORE

    { NULL, NULL, NULL, NULL, NULL, NULL }
};


/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                           M A I N   C O M M A N D S                             ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static int
do_kill( ControlClient  client, char*  args )
{
    control_write( client, "OK: killing emulator, bye bye\r\n" );
    exit(0);
}

static const CommandDefRec   main_commands[] =
{
    { "help|h|?", "print a list of commands", NULL, NULL, do_help, NULL },

    { "event", "simulate hardware events",
    "allows you to send fake hardware events to the kernel\r\n", NULL,
    NULL, event_commands },

    { "geo", "Geo-location commands",
      "allows you to change Geo-related settings, or to send GPS NMEA sentences\r\n", NULL,
      NULL, geo_commands },

    { "gsm", "GSM related commands",
      "allows you to change GSM-related settings, or to make a new inbound phone call\r\n", NULL,
      NULL, gsm_commands },

    { "cdma", "CDMA related commands",
      "allows you to change CDMA-related settings\r\n", NULL,
      NULL, cdma_commands },

    { "kill", "kill the emulator instance", NULL, NULL,
      do_kill, NULL },

    { "network", "manage network settings",
      "allows you to manage the settings related to the network data connection of the\r\n"
      "emulated device.\r\n", NULL,
      NULL, network_commands },

    { "power", "power related commands",
      "allows to change battery and AC power status\r\n", NULL,
      NULL, power_commands },

    { "quit|exit", "quit control session", NULL, NULL,
      do_quit, NULL },

    { "redir",    "manage port redirections",
      "allows you to add, list and remove UDP and/or PORT redirection from the host to the device\r\n"
      "as an example, 'redir  tcp:5000:6000' will route any packet sent to the host's TCP port 5000\r\n"
      "to TCP port 6000 of the emulated device\r\n", NULL,
      NULL, redir_commands },

    { "sms", "SMS related commands",
      "allows you to simulate an inbound SMS\r\n", NULL,
      NULL, sms_commands },

    { "avd", "control virtual device execution",
    "allows you to control (e.g. start/stop) the execution of the virtual device\r\n", NULL,
    NULL, vm_commands },

    { "window", "manage emulator window",
    "allows you to modify the emulator window\r\n", NULL,
    NULL, window_commands },

    { "qemu", "QEMU-specific commands",
    "allows to connect to the QEMU virtual machine monitor\r\n", NULL,
    NULL, qemu_commands },

    { "sensor", "manage emulator sensors",
      "allows you to request the emulator sensors\r\n", NULL,
      NULL, sensor_commands },

    { "finger", "manage emulator finger print",
      "allows you to touch the emulator finger print sensor\r\n", NULL,
      NULL, fingerprint_commands},

    { "operator", "manage telephony operator info",
      "allows you to modify/retrieve telephony operator info\r\n", NULL,
      NULL, operator_commands },

    { "stk", "STK related commands",
      "allows you to simulate an inbound STK proactive command\r\n", NULL,
      NULL, stk_commands },

    { "mux", "device multiplexing management",
      "allows to select the active device of its kind for console control\r\n", NULL,
      NULL, mux_commands },

    { "cbs", "Cell Broadcast related commands",
      "allows you to simulate an inbound CBS\r\n", NULL,
      NULL, cbs_commands },

    { "rfkill", "RFKILL related commands",
      "allows you to modify/retrieve RFKILL status, hardware blocking\r\n", NULL,
      NULL, rfkill_commands },

    { "nfc", "NFC related commands",
      "allows you to modify/retrieve NFC states and send notifications\r\n", NULL,
      NULL, nfc_commands },

    { "modem", "Modem related commands",
      "allows you to modify/retrieve modem info\r\n", NULL,
      NULL, modem_commands },

    { "bt", "Bluetooth related commands",
      "allows you to retrieve BT status or add/remove remote devices\r\n", NULL,
      NULL, bt_commands },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};


static ControlGlobalRec  _g_global;

int
control_console_start( int  port )
{
    return control_global_init( &_g_global, port );
}
