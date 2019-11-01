#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include "restworker.h"
#include "hash.h"
#include "list.h"
#include "n_buf.h"
#include "string1.h"
#include "json.h"
#include "trace.h"

#define RESTWORKER_OK 200
#define RESTWORKER_BAD_REQUEST 400
#define RESTWORKER_NOT_FOUND 404
#define RESTWORKER_METHOD_NOT_ALLOWED 405
#define RESTWORKER_REQUEST_TIMEOUT 408
#define RESTWORKER_INTERVAL_SERVER_ERROR 500
#define RESTWORKER_NOT_IMPLEMENTED 501

static const char *restworker_strstatus(int status)
{
  switch (status) {
  case RESTWORKER_OK:
    return "OK";
  case RESTWORKER_BAD_REQUEST:
    return "Bad Request";
  case RESTWORKER_NOT_FOUND:
    return "Not Found";
  case RESTWORKER_METHOD_NOT_ALLOWED:
    return "Method Not Allowed";
  case RESTWORKER_REQUEST_TIMEOUT:
    return "Request Timeout";
  case RESTWORKER_INTERVAL_SERVER_ERROR:
    return "Interval Server Error";
  case RESTWORKER_NOT_IMPLEMENTED:
    return "Not Implemented";
  default:
    return "Status not defined";
  }
}

struct restworker_res {
  struct hlist_node br_hash_node;
  struct json *br_json;
  void *br_data;
  char br_path[];
};

struct restworker_conn {
  struct ev_io bc_io_w;
  struct ev_timer bc_timer_w;
  struct n_buf bc_rd_buf, bc_wr_hdr, bc_wr_buf;
  struct restworker_listen *bc_listen;
  struct list_head bc_listen_link;
};

static void restworker_conn_cb(EV_P_ struct evx_listen *el, int fd,
                         const struct sockaddr *addr, socklen_t addrlen);
static void restworker_conn_rd_cb(EV_P_ struct ev_io *w, int revents);
static void restworker_conn_timer_cb(EV_P_ struct ev_timer *w, int revents);
static void restworker_conn_kill(EV_P_ struct restworker_conn *bc);

int restworker_listen_init(struct restworker_listen *bl, size_t nr_res)
{
  memset(bl, 0, sizeof(*bl));
  evx_listen_init(&bl->bl_listen, &restworker_conn_cb);
  INIT_LIST_HEAD(&bl->bl_conn_list);
  bl->bl_timeout = 15.0; /* XXX */
  bl->bl_rd_buf_size = 4096;
  bl->bl_wr_hdr_size = 4096;
  bl->bl_wr_buf_size = 1048576; /* XXX */

  if (hash_table_init(&bl->bl_res_table, nr_res) < 0)
    return -1;

  return 0;
}

void restworker_listen_destroy(struct restworker_listen *bl)
{
  /* ... */
  memset(bl, 0, sizeof(*bl));
}

int restworker_set_json(struct restworker_listen *bl, const char *path,
                  struct json *j, void *data)
{
  struct hash_table *t = &bl->bl_res_table;
  struct hlist_head *head;
  struct restworker_res *br;

  br = str_table_lookup_entry(t, path, &head, struct restworker_res,
                              br_hash_node, br_path);
  if (br != NULL) {
    errno = EEXIST;
    return -1;
  }

  br = malloc(sizeof(*br) + strlen(path) + 1);
  if (br == NULL)
    return -1;

  memset(br, 0, sizeof(*br));
  strcpy(br->br_path, path);
  br->br_json = j;
  br->br_data = data;
  hlist_add_head(&br->br_hash_node, head);

  return 0;
}

static struct restworker_res *
restworker_lookup_res(struct restworker_listen *bl, const char *path, int *status)
{
  struct hash_table *t = &bl->bl_res_table;
  struct hlist_head *head;
  struct restworker_res *br;

  TRACE("path `%s'\n", path);

  br = str_table_lookup_entry(t, path, &head, struct restworker_res,
                              br_hash_node, br_path);
  if (br == NULL)
    *status = RESTWORKER_NOT_FOUND;

  return br;
}

static void restworker_conn_kill(EV_P_ struct restworker_conn *bc)
{
  TRACE("restworker_conn_kill fd %d\n", bc->bc_io_w.fd);

  ev_io_stop(EV_A_ &bc->bc_io_w);
  ev_timer_stop(EV_A_ &bc->bc_timer_w);

  if (!(bc->bc_io_w.fd < 0))
    close(bc->bc_io_w.fd);
  bc->bc_io_w.fd = -1;

  list_del(&bc->bc_listen_link);
  n_buf_destroy(&bc->bc_rd_buf);
  n_buf_destroy(&bc->bc_wr_hdr);
  n_buf_destroy(&bc->bc_wr_buf);

  free(bc);
}

static void restworker_conn_cb(EV_P_ struct evx_listen *el, int fd,
                         const struct sockaddr *addr, socklen_t addrlen)
{
  struct restworker_listen *bl = container_of(el, struct restworker_listen, bl_listen);
  struct restworker_conn *bc = NULL;

  bc = malloc(sizeof(*bc));
  if (bc == NULL)
    goto err;

  memset(bc, 0, sizeof(*bc));

  ev_io_init(&bc->bc_io_w, &restworker_conn_rd_cb, fd, EV_READ);
  ev_io_start(EV_A_ &bc->bc_io_w);

  ev_timer_init(&bc->bc_timer_w, &restworker_conn_timer_cb, bl->bl_timeout, 0);
  ev_timer_start(EV_A_ &bc->bc_timer_w);

  bc->bc_listen = bl;
  list_add(&bc->bc_listen_link, &bl->bl_conn_list);

  if (n_buf_init(&bc->bc_rd_buf, bl->bl_rd_buf_size) < 0)
    goto err;
  if (n_buf_init(&bc->bc_wr_hdr, bl->bl_wr_hdr_size) < 0)
    goto err;
  if (n_buf_init(&bc->bc_wr_buf, bl->bl_wr_buf_size) < 0)
    goto err;

  return;

 err:
  if (bc != NULL)
    restworker_conn_kill(EV_A_ bc);
}

static void restworker_conn_fmt_hdr(struct restworker_conn *bc, int status)
{
  if (status < 0)
    status = RESTWORKER_INTERVAL_SERVER_ERROR;
  else if (status == 0)
    status = RESTWORKER_OK;

  n_buf_clear(&bc->bc_wr_hdr);
  n_buf_printf(&bc->bc_wr_hdr,
               "HTTP/1.1 %d %s\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: %zu\r\n"
               "Connection: close\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "\r\n", status, restworker_strstatus(status),
               n_buf_length(&bc->bc_wr_buf));
}

static void restworker_conn_error(EV_P_ struct restworker_conn *bc, int status)
{
  int eof = 0, err = 0;

  n_buf_clear(&bc->bc_wr_buf);
  restworker_conn_fmt_hdr(bc, status);
  n_buf_drain(&bc->bc_wr_hdr, bc->bc_io_w.fd, &eof, &err);
  restworker_conn_kill(EV_A_ bc);
}

static void restworker_conn_timer_cb(EV_P_ struct ev_timer *w, int revents)
{
  struct restworker_conn *bc = container_of(w, struct restworker_conn, bc_timer_w);

  restworker_conn_error(EV_A_ bc, RESTWORKER_REQUEST_TIMEOUT);
}

static void restworker_conn_wr_cb(EV_P_ struct ev_io *w, int revents)
{
  struct restworker_conn *bc = container_of(w, struct restworker_conn, bc_io_w);
  int eof = 0, err = 0;

  n_buf_drain(&bc->bc_wr_hdr, w->fd, &eof, &err);
  if (!eof)
    return;
  if (err != 0) {
    restworker_conn_error(EV_A_ bc, -1);
    return;
  }

  eof = 0;
  n_buf_drain(&bc->bc_wr_buf, w->fd, &eof, &err);
  if (!eof)
    return;
  if (err != 0) {
    restworker_conn_error(EV_A_ bc, -1);
    return;
  }

  restworker_conn_kill(EV_A_ bc);
}

static void restworker_conn_rd_cb(EV_P_ struct ev_io *w, int revents)
{
  struct restworker_conn *bc = container_of(w, struct restworker_conn, bc_io_w);
  int eof = 0, err = 0;
  char *req, *meth, *path;
  size_t req_len;
  struct restworker_res *br;

  n_buf_fill(&bc->bc_rd_buf, w->fd, &eof, &err);
  if (err != 0) {
    restworker_conn_error(EV_A_ bc, -1);
    return;
  }

  if (n_buf_get_msg(&bc->bc_rd_buf, &req, &req_len) != 0) {
    if (eof)
      restworker_conn_error(EV_A_ bc, RESTWORKER_BAD_REQUEST);
    return;
  }

  if (split(&req, &meth, &path, (char *) NULL) != 2) {
    restworker_conn_error(EV_A_ bc, RESTWORKER_BAD_REQUEST);
    return;
  }

  if (strcmp(meth, "GET") != 0) {
    restworker_conn_error(EV_A_ bc, RESTWORKER_NOT_IMPLEMENTED);
    return;
  }

  int status = 0;
  br = restworker_lookup_res(bc->bc_listen, path, &status);
  if (br == NULL) {
    restworker_conn_error(EV_A_ bc, status);
    return;
  }

  ssize_t len = 0;
  len += json_format(&bc->bc_wr_buf, br->br_json, br->br_data);
  len += n_buf_printf(&bc->bc_wr_buf, "\r\n");

  if (len > bc->bc_wr_buf.nb_size) {
    restworker_conn_error(EV_A_ bc, -1); 
    return;
  }

  restworker_conn_fmt_hdr(bc, RESTWORKER_OK);

  ev_io_stop(EV_A_ &bc->bc_io_w);
  ev_io_init(&bc->bc_io_w, &restworker_conn_wr_cb, w->fd, EV_WRITE);
  ev_io_start(EV_A_ &bc->bc_io_w);
}
