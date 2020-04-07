#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <termios.h>
#include <stdint.h>

#include <net/if.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include "ei.h"
#include "erl_driver.h"

#define event2sock(EV) ((int) ((long) (EV)))
#define sock2event(FD) ((ErlDrvEvent) ((long) (FD)))
#define DEFAULT_POLL_PERIOD 10

#include "mosquitto_broker_internal.h"

static char* get_s(const char* buf, int len);

static void encode_ok(ei_x_buff* x);
static void encode_error(ei_x_buff* x);

static ErlDrvBinary* ei_x_to_new_binary(ei_x_buff* x);

#include "mosquitto_embed.h"

//----------------------------------
// Defines must be in sync with mosquitto_embed.ex
//----------------------------------
#define CMD_ECHO 0
#define CMD_INIT 1
#define CMD_POLL_PERIOD 2
#define CMD_OPEN_CLIENT 3
#define CMD_SUBSCRIBE 4


typedef struct {
  char* topic;
  erlang_pid pid;
  UT_hash_handle hh;
} mosq_sub_t;

typedef struct {
  ErlDrvPort  port;
  struct mosquitto_db *db;
  mosq_sock_t *listensock;
  int listensock_count;
  int poll_period;
  struct mosquitto * mosq_context;
} mosquitto_embed_data;


// This is needed for select_stop()
static struct mosquitto_db *db;

static ErlDrvData start(ErlDrvPort port, char *buff)
{
  // char *portname = BLE_PORT;
  // int fd = -1;

  //----------------------------------
  // Create the Erlang Driver Ptr
  //----------------------------------
  mosquitto_embed_data* d = (mosquitto_embed_data*)driver_alloc(sizeof(mosquitto_embed_data));
  d->port = port;
  set_port_control_flags(port, PORT_CONTROL_FLAG_BINARY);
  //----------------------------------
  // Open the Serial Port (TTY)
  //----------------------------------
  // if((fd = open (portname, O_RDWR | O_NOCTTY)) < 0) {
  //   goto exit_on_error;
  // }


  // d->serial_port = fd;

  // Set up to receive notifications when the serial_port is ready to be "read"
  //driver_select(d->port, (ErlDrvEvent)d->serial_port, ERL_DRV_READ,1);

  return (ErlDrvData)d;

// exit_on_error:
//   //Try to gracefully cleanup
//   driver_free(d);

//   if (fd != -1)
//   {
//     close(fd);
//   }
//   return ERL_DRV_ERROR_GENERAL;
}

static void stop(ErlDrvData handle)
{
  mosquitto_embed_data* d = (mosquitto_embed_data*)handle;
  // fprintf(stderr, "\r\nstop\r\n");
  // driver_select(d->port, (ErlDrvEvent)d->serial_port, ERL_DRV_READ, 0);

  for(int i=0; i < d->listensock_count; i++)
  {
    driver_select(d->port, sock2event(d->listensock[i]), ERL_DRV_READ,0);
  }
  mosquitto_deinit();
  driver_free((char*)handle);
}

static int cmd_echo(char *s, mosquitto_embed_data* data, ei_x_buff* x)
{
  // mosquitto_embed_data* d = (mosquitto_embed_data*)handle;
  driver_output(data->port, s, strlen(s));
  encode_ok(x);
  // encode_error(x, conn);
  return 0;
}

static char* argc0 = "mosquitto";

static void args_to_argv(char * args,  int* argc, char*** argv)
{
  
  int count = 1;

  fprintf(stderr, "args_to_argv %s\r\n", args);

  if(args == NULL)
  {
    *argc=1;
    char **v = (char**)driver_alloc(sizeof(char *));
    v[0] = argc0;
    *argv = v;
    return;
  }

  for(int i =0; args[i] != '\0'; i++)
  {
    fprintf(stderr, "args_to_argv %d\r\n", i);
    if(args[i] == ' ')
    {
      count = count + 1;
    }
  }
  
  int size = count * sizeof(char *);
  char **v = (char**)driver_alloc(size);
  fprintf(stderr, "driver_alloc %d\r\n", size);

  // Note the args is already duplicated using get_s() so it is safe
  // here to chop the string up
  int j = 0;
  v[j] = argc0;
  j++;
  for(int i =0; args[i] != '\0'; i++)
  {
    if(i == 0)
    {
      v[j] = &args[i];
    }
    else if(args[i] == ' ')
    {
      args[i] = '\0';
      v[j++] = &args[i+1];
    }
  }

  *argc = count;
  *argv = v;
}

static int my_context = 42;

static void subscribe_callback(
  struct mosquitto_db *db, 
  struct mosquitto *context, 
  const char *topic, 
  struct mosquitto_msg_store *msg_store, 
  mosq_user_context_t user_context)
{
  fprintf(stderr, "subscribe_callback\r\n");
  fprintf(stderr, "%s %s %i\r\n", topic, (char*)UHPA_ACCESS_PAYLOAD(msg_store), *(int*)user_context);
}

static int cmd_init(char *args, mosquitto_embed_data* d, ei_x_buff* x)
{
  int argc;
  char **argv;

  fprintf(stderr, "\r\ninit\r\n");

  args_to_argv(args, &argc, &argv);

  fprintf(stderr, "args_to_argv\r\n");
  mosquitto_init(argc, argv);
  fprintf(stderr, "int_db\r\n");
  db = mosquitto__get_db();
  fprintf(stderr, "mosquitto__get_listensock\r\n");
  mosquitto__get_listensock(&(d->listensock), &(d->listensock_count));

  d->db = db;

  fprintf(stderr, "driver_select\r\n");
  for(int i=0; i < d->listensock_count; i++)
  {
    driver_select(d->port, sock2event(d->listensock[i]), ERL_DRV_READ,1);
  }

  db->start_time = mosquitto_time();
#ifdef WITH_PERSISTENCE
  int_db->last_backup = mosquitto_time();
#endif
  d->poll_period = DEFAULT_POLL_PERIOD;
  driver_set_timer(d->port, d->poll_period);

  d->mosq_context = mosquitto_plugin__create_context(db, "erlclient");
  
  encode_ok(x);
  return 0;
}

static int cmd_subscribe(char *topic, erlang_pid* pid, mosquitto_embed_data* d, ei_x_buff* x)
{
  if(d->mosq_context != NULL)
  {
    mosquitto_plugin__subscribe(db, d->mosq_context, topic, subscribe_callback, &my_context);
  }
  else
  {
    fprintf(stderr, "No Context\r\n");
  }
}

static ErlDrvSSizeT call(ErlDrvData drv_data, unsigned int command, char *buf, ErlDrvSizeT len, char **rbuf, ErlDrvSizeT rlen,
                 unsigned int *flags)
{
  int nlen = -1;
  int r = -1;
  ei_x_buff x;
  mosquitto_embed_data* data = (mosquitto_embed_data*)drv_data;
  char* s;
  int term_size;
  int term_type;
  int index = 0;
  erlang_pid pid;

  ei_x_new_with_version(&x);
  switch (command) 
  {
    case CMD_ECHO: 
        s = get_s(buf, len);
        r = cmd_echo(s, data, &x);  
        driver_free(s);
        break;
    case CMD_INIT:
        s = get_s(buf, len);
        r = cmd_init(s, data, &x);
        driver_free(s);
        break;
    case CMD_SUBSCRIBE:
        if (ei_decode_tuple_header(buf, &index, &term_size) < 0 || term_size != 2)
        {
            fprintf(stderr, "Expecting {topic, pid} tuple\r\n");
            encode_error(&x);
            goto exit_on_error;
        }
        if (ei_get_type(buf, &index, &term_type, &term_size) < 0 || term_type != ERL_BINARY_EXT)
        {
            fprintf(stderr, "Expecting topic as Binary\r\n");
            encode_error(&x);
            goto exit_on_error;
        }
        s = driver_alloc(term_size+1);
        long topic_size;
        if (ei_decode_binary(buf, &index, s, &topic_size) < 0)
        {
            fprintf(stderr, "Cannot decode Topic\r\n");
            encode_error(&x);
            goto exit_on_error;
        }
        if (ei_decode_pid(buf, &index, &pid) < 0)
        {
            fprintf(stderr, "Cannot decode PID\r\n");
            encode_error(&x);
            goto exit_on_error;
        }   
        r = cmd_subscribe(s, &pid, data, &x);
        break;
      default:
        break;
  }

    // if (rlen < len) {
    //     *rbuf = (void *)driver_alloc(len);
    // }
    // (void)memcpy(*rbuf, buf, len);
    // return (ErlDrvSSizeT)(len);
exit_on_error:
  nlen = x.index;
	if (nlen > rlen) {
	    *rbuf =driver_alloc(nlen);
	}
	memcpy(*rbuf,x.buff,nlen);
	ei_x_free(&x);
  return nlen;
}

// static ErlDrvSSizeT call(ErlDrvData drv_data, unsigned int command, char *buf, ErlDrvSizeT len, char **rbuf, ErlDrvSizeT rlen,
//                  unsigned int *flags)
// {
//     if (rlen < len) {
//         *rbuf = (void *)driver_alloc(len);
//     }
//     (void)memcpy(*rbuf, buf, len);
//     return (ErlDrvSSizeT)(len);
// }

static void handle_erl_msg(ErlDrvData handle, char *buff, 
                   ErlDrvSizeT bufflen)
{
  mosquitto_embed_data* d = (mosquitto_embed_data*)handle;

  // echo back
  driver_output(d->port, buff, bufflen);

  // EXTERN int erl_drv_output_term(ErlDrvTermData port,
	// 		       ErlDrvTermData* data,
	// 		       int len);
  // driver_output(d->port, "yes", 3);
}
static void on_write_block(struct mosquitto * mosq_context, mosq_sock_t sock, mosq_user_context_t context)
{
  mosquitto_embed_data *d = (mosquitto_embed_data*)context;
  fprintf(stderr, "on_write_block\r\n");
  driver_select(d->port, sock2event(sock), ERL_DRV_WRITE, 1);
}

static void on_socket_accept(struct mosquitto * mosq_context, mosq_sock_t sock, void* context)
{
  mosquitto_embed_data *d = (mosquitto_embed_data*)context;
  fprintf(stderr, "on_socket_accept\r\n");

  mosquitto__on_write_block(mosq_context, on_write_block, d);

  driver_select(d->port, sock2event(sock), ERL_DRV_READ, 1);
  driver_select(d->port, sock2event(sock), ERL_DRV_WRITE, 1);
}

static void handle_socket_input(ErlDrvData handle, ErlDrvEvent event)
{
  mosquitto_embed_data *d = (mosquitto_embed_data*)handle;
  // fprintf(stderr, "handle_socket_input\r\n");

  mosquitto__readsock(d->db,event2sock(event), on_socket_accept, d);
  
  mosquitto__loop_step(db);

  driver_set_timer(d->port, d->poll_period);
  // mosquitto__writesock(d->db,event2sock(event));
}

static void handle_socket_output(ErlDrvData handle, ErlDrvEvent event)
{
  mosquitto_embed_data *d = (mosquitto_embed_data*)handle;
  // fprintf(stderr, "handle_socket_output\r\n");

  // Disable socket notfications here as mosquitto__writesock() might need to enable them
  driver_select(d->port, sock2event(event), ERL_DRV_WRITE, 0);
}

/* Handling of timeout in driver */
static void timeout(ErlDrvData drv_data)
{
  mosquitto_embed_data *d = (mosquitto_embed_data*)drv_data;

  mosquitto__loop_step(db);

  driver_set_timer(d->port, d->poll_period);
}

static void process_exit(ErlDrvData handle, ErlDrvMonitor *monitor)
{
  
}

/* Called on behalf of driver_select when
  it is safe to release 'event'. A typical
  unix driver would call close(event) */
static void stop_select(ErlDrvEvent event, void* reserved)
{
  mosquitto__closesock(db, event2sock(event));
}

static char* get_s(const char* buf, int len)
{
    char* result;
    if (len < 1 || len > 10000) return NULL;
    result = driver_alloc(len+1);
    memcpy(result, buf, len);
    result[len] = '\0';
    return result;
}

static void encode_ok(ei_x_buff* x)
{
    const char* k_ok = "ok";
    ei_x_encode_atom(x, k_ok);
}

static void encode_error(ei_x_buff* x)
{
    const char* k_error = "error";
    ei_x_encode_atom(x, k_error);
}

static ErlDrvBinary* ei_x_to_new_binary(ei_x_buff* x)
{
  ErlDrvBinary* bin = driver_alloc_binary(x->index);
  if (bin != NULL)
	  memcpy(&bin->orig_bytes[0], x->buff, x->index);
  return bin;
}

#define DRV_FLAGS (ERL_DRV_FLAG_USE_PORT_LOCKING | ERL_DRV_FLAG_SOFT_BUSY)

ErlDrvEntry mosquitto_embed_driver_entry = {
    NULL,                         /* F_PTR init, called when driver is loaded */
    start,                        /* L_PTR start, called when port is opened */
    stop,                         /* F_PTR stop, called when port is closed */
    handle_erl_msg,               /* F_PTR output, called when erlang has sent */
    handle_socket_input,          /* F_PTR ready_input, called when input descriptor ready */
    handle_socket_output,         /* F_PTR ready_output, called when output descriptor ready */
    "mosquitto_embed",            /* char *driver_name, the argument to open_port */
    NULL,                         /* F_PTR finish, called when unloaded */
    NULL,                         /* void *handle, Reserved by VM */
    NULL,                         /* F_PTR control, port_command callback */
    timeout,                      /* F_PTR timeout, Handling of timeout in driver */
    NULL,                         /* F_PTR outputv,  called when we have output from erlang
				                            to the port */
    NULL,                         /* F_PTR ready_async, only for async drivers */
    NULL,                         /* F_PTR flush, called when port is about 
                                    to be closed, but there is data in driver 
                                    queue */
    call,                         /* F_PTR call, much like control, sync call
                                     to driver */
    NULL,                         /* F_PTR event, called when an event selected 
                                     by driver_event() occurs. */
    ERL_DRV_EXTENDED_MARKER,      /* int extended marker, Should always be 
                                     set to indicate driver versioning */
    ERL_DRV_EXTENDED_MAJOR_VERSION, /* int major_version, should always be 
                                       set to this value */
    ERL_DRV_EXTENDED_MINOR_VERSION, /* int minor_version, should always be 
                                       set to this value */
    DRV_FLAGS,                      /* int driver_flags, see documentation */
    NULL,                           /* void *handle2, reserved for VM use */
    NULL,                           /* F_PTR process_exit, called when a 
                                       monitored process dies */
    stop_select                     /* F_PTR stop_select, called to close an 
                                       event object */
};

DRIVER_INIT(mosquitto_embed) /* must match name in driver_entry */
{
    return &mosquitto_embed_driver_entry;
}