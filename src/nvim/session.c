// sessions.c
#include "nvim/session.h"
#include "klib/kvec.h"
#include "mpack/mpack_core.h"
#include "nvim/api/private/defs.h"
#include "nvim/api/private/helpers.h"
#include "nvim/context.h"
#include "nvim/ex_docmd.h"
#include "nvim/memory.h"
#include "nvim/memory_defs.h"
#include "nvim/msgpack_rpc/channel_defs.h"
#include "nvim/msgpack_rpc/packer.h"
#include "nvim/msgpack_rpc/unpacker.h"
#include "nvim/option.h"
#include "nvim/os/fileio.h"
#include "nvim/os/fileio_defs.h"
#include "nvim/os/fs.h"
#include "nvim/os/os.h"

#include "nvim/globals.h"
#include <stddef.h>
#include <stdio.h>

// TODO(rk-dev) Adapt logic for right and top splits
static win_T *leftmost_leaf(frame_T *fr) {
  if (fr->fr_layout == FR_LEAF) {
    return fr->fr_win;
  }
  return leftmost_leaf(fr->fr_child);
}

static void collect_split_operations(frame_T *fr, Array *ops) {
  if (fr->fr_layout == FR_LEAF || fr == NULL) {
    return;
  }

  const char *split_type =
      (fr->fr_layout == FR_COL) ? "VerticalSplit" : "HorizontalSplit";

  for (frame_T *child = fr->fr_child; child && child->fr_next;
       child = child->fr_next) {
    frame_T *next = child->fr_next;

    win_T *a = leftmost_leaf(child);
    win_T *b = leftmost_leaf(next);

    if (a && b) {
      Array op = ARRAY_DICT_INIT;
      ADD(op, STRING_OBJ(cstr_to_string(split_type)));
      ADD(op, INTEGER_OBJ(a->handle));
      ADD(op, INTEGER_OBJ(b->handle));
      ADD(*ops, ARRAY_OBJ(op));
    }
  }

  // Recurse into all children
  for (frame_T *child = fr->fr_child; child != NULL; child = child->fr_next) {
    collect_split_operations(child, ops);
  }
}

Dict get_ui_layout(void) {
  Array tabs_array = ARRAY_DICT_INIT;

  FOR_ALL_TABS(tp) {
    Array windows_array = ARRAY_DICT_INIT;

    FOR_ALL_WINDOWS_IN_TAB(win, tp) {
      Dict win_dict = ARRAY_DICT_INIT;
      if (win->w_buffer != NULL && win->w_buffer->b_ffname != NULL &&
          win->w_buffer->b_p_bt[0] == '\0') {

        PUT(win_dict, "winid", INTEGER_OBJ(win->handle));
        PUT(win_dict, "filename",
            STRING_OBJ(cstr_to_string((const char *)win->w_buffer->b_ffname)));
        PUT(win_dict, "lnum", INTEGER_OBJ(win->w_cursor.lnum));
        ADD(windows_array, DICT_OBJ(win_dict));
      }
    }
    Array ops = ARRAY_DICT_INIT;
    collect_split_operations(tp->tp_topframe, &ops);

    Dict tab_dict = ARRAY_DICT_INIT;
    PUT(tab_dict, "windows", ARRAY_OBJ(windows_array));
    PUT(tab_dict, "splits", ARRAY_OBJ(ops));
    ADD(tabs_array, DICT_OBJ(tab_dict));
  }

  Dict ui_dict = ARRAY_DICT_INIT;

  char cwd[MAXPATHL];
  if (os_dirname(cwd, MAXPATHL) == OK) {
    PUT(ui_dict, "cwd", STRING_OBJ(cstr_to_string(cwd)));
  } else {
    PUT(ui_dict, "cwd", STRING_OBJ(cstr_to_string("")));
  }

  PUT(ui_dict, "tabs", ARRAY_OBJ(tabs_array));

  return ui_dict;
}

static void pack_object(Object obj, PackerBuffer *sbuf);

static void pack_dict(Dict *dict, PackerBuffer *sbuf) {
  mpack_map(&sbuf->ptr, dict->size);

  for (size_t i = 0; i < dict->size; i++) {
    String key = dict->items[i].key;
    Object val = dict->items[i].value;

    // Pack key string
    mpack_str(key, sbuf);

    // Pack value
    pack_object(val, sbuf);
  }
}

static void pack_array(Array arr, PackerBuffer *sbuf) {
  mpack_array(&sbuf->ptr, arr.size);
  for (size_t i = 0; i < arr.size; i++) {
    pack_object(arr.items[i], sbuf);
  }
}

static void pack_object(Object obj, PackerBuffer *sbuf) {
  switch (obj.type) {
  case kObjectTypeNil:
    mpack_nil(&sbuf->ptr);
    break;
  case kObjectTypeBoolean:
    mpack_bool(&sbuf->ptr, obj.data.boolean);
    break;
  case kObjectTypeInteger:
    mpack_integer(&sbuf->ptr, obj.data.integer);
    break;
  case kObjectTypeString:
    mpack_str(obj.data.string, sbuf);
    break;
  case kObjectTypeArray:
    pack_array(obj.data.array, sbuf);
    break;
  case kObjectTypeDict:
    pack_dict(&obj.data.dict, sbuf);
    break;
  default:
    // Unsupported type, maybe pack nil or error
    mpack_nil(&sbuf->ptr);
    break;
  }
}

static bool pack_ui_session(Dict *ui, PackerBuffer *packer) {
  pack_dict(ui, packer);
  return true;
}

static void flush_file_buffer(PackerBuffer *buffer) {
  FileDescriptor *fd = buffer->anydata;
  fd->write_pos = buffer->ptr;
  buffer->anyint = file_flush(fd);
  buffer->ptr = fd->write_pos;
}

void session_save_to_file(Dict *p_ui, const char *fname) {
  FileDescriptor sd_writer;

  const char *session_path = stdpaths_user_state_subpath(fname, 0, true);

  int perm = (int)os_getperm(session_path);
  perm = (perm >= 0) ? ((perm & 0777) | 0600) : 0600;

  int error =
      file_open(&sd_writer, session_path, kFileCreate | kFileNoSymlink, perm);

  if (error < 0) {
    return;
  }

  sd_writer.write_pos = sd_writer.buffer;

  PackerBuffer packer = (PackerBuffer){
      .startptr = sd_writer.buffer,
      .ptr = sd_writer.write_pos,
      .endptr = sd_writer.buffer + ARENA_BLOCK_SIZE,
      .anydata = &sd_writer,
      .anyint = 0,
      .packer_flush = flush_file_buffer,
  };

  if (pack_ui_session(p_ui, &packer)) {
    flush_file_buffer(&packer);
  }
  os_close(sd_writer.fd);
}

static Object unpack_object(const char **data, size_t *size) {
  Object obj = OBJECT_INIT;
  mpack_token_t tok;

  // First read one MsgPack token header:
  if (mpack_rtoken(data, size, &tok) != 0) {
    return obj;
  }

  switch (tok.type) {
  case MPACK_TOKEN_MAP: {
    Dict temp_d = (Dict)ARRAY_DICT_INIT;
    for (size_t i = 0; i < tok.length; i++) {
      String key = unpack_string(data, size);
      if (!key.data) {
        api_free_dict(temp_d);
        return obj;
      }

      char *nul_key = key.data;

      Object val = unpack_object(data, size);

      PUT(temp_d, nul_key, val);
    }
    obj.type = kObjectTypeDict;
    obj.data.dict = temp_d;
    break;
  }

  case MPACK_TOKEN_ARRAY: {
    Array a = (Array)ARRAY_DICT_INIT;
    for (size_t i = 0; i < tok.length; i++) {
      Object val = unpack_object(data, size);
      if (val.type == kObjectTypeNil) {
        api_free_array(a);
        return obj;
      }
      ADD(a, val);
    }
    obj.type = kObjectTypeArray;
    obj.data.array = a;
    break;
  }

  case MPACK_TOKEN_STR:
  case MPACK_TOKEN_BIN: {
    size_t len = tok.length;
    if (*size < len) {
      return obj;
    }

    // Allocate (len + 1) so we can NUL-terminate.
    char *buf = xmalloc(len + 1);
    memcpy(buf, *data, len);
    buf[len] = '\0';

    // Advance past those len payload bytes:
    *data += len;
    *size -= len;

    obj.type = kObjectTypeString;
    obj.data.string.data = buf;
    obj.data.string.size = len;
    break;
  }

  case MPACK_TOKEN_UINT:
  case MPACK_TOKEN_SINT: {
    Integer i;
    if (unpack_uint_or_sint(tok, &i)) {
      obj.type = kObjectTypeInteger;
      obj.data.integer = i;
    }
    break;
  }

  case MPACK_TOKEN_NIL:
    obj.type = kObjectTypeNil;
    break;

  default:
    obj.type = kObjectTypeNil;
    break;
  }

  return obj;
}

static int msgpack_read_dict(FileDescriptor *const sd_reader, Dict *dict,
                             const size_t max_kbyte) {
  const size_t max_bytes = (max_kbyte ? max_kbyte : 4) * 1024;
  char *buf = xmalloc(max_bytes);

  ssize_t nread = read(sd_reader->fd, buf, max_bytes);
  if (nread <= 0) {
    xfree(buf);
    return 1;
  }

  const char *data = buf;
  size_t size = (size_t)nread;

  Object root = unpack_object(&data, &size);
  if (root.type != kObjectTypeDict) {
    xfree(buf);
    return 1;
  }

  *dict = root.data.dict;
  xfree(buf);
  return 0;
}

Dict session_restore(void) {
  FileDescriptor sd_reader;

  // TODO(rk-dev): take fname from caller
  const char *session_path =
      stdpaths_user_state_subpath("ui-session.v1.mpack", 0, true);

  int perm = 0;
  perm = (perm >= 0) ? ((perm & 0777) | 0600) : 0600;

  int open_result = file_open(&sd_reader, session_path, kFileReadOnly, 0);
  if (open_result != 0) {
    return (Dict)ARRAY_DICT_INIT;
  }

  Dict restored_dict = ARRAY_DICT_INIT;
  if (msgpack_read_dict(&sd_reader, &restored_dict, 0) != 0) {
    file_close(&sd_reader, NULL);

    return (Dict)ARRAY_DICT_INIT;
  }

  file_close(&sd_reader, NULL);
  return restored_dict;
}
