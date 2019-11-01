#ifndef _RESTWORKER_H_
#define _RESTWORKER_H_
#include <stddef.h>
#include "evx.h"
#include "hash.h"

struct restworker_listen {
  struct evx_listen bl_listen;
  struct list_head bl_conn_list;
  /* TODO set max connecitons via bl_max_conn */
  double bl_timeout;
  size_t bl_rd_buf_size, bl_wr_hdr_size, bl_wr_buf_size;
  struct hash_table bl_res_table;
};

int restworker_listen_init(struct restworker_listen *bl, size_t nr_res);

void restworker_listen_destroy(struct restworker_listen *bl);

struct json;

int restworker_set_json(struct restworker_listen *bl, const char *path,
                  struct json *j, void *data);

int restworker_del_res(struct restworker_listen *bl, const char *path);

#endif
