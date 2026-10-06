#include "lanicom/control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WT_VARINT = 0, WT_I64 = 1, WT_LEN = 2, WT_I32 = 5 };

/* --- writer -------------------------------------------------------------- */

typedef struct {
  uint8_t *p;
  size_t len, cap;
  bool overflow;
} wbuf_t;

static void put(wbuf_t *w, const void *data, size_t n) {
  if (w->len + n > w->cap) {
    w->overflow = true;
    return;
  }
  memcpy(w->p + w->len, data, n);
  w->len += n;
}

static void put_varint(wbuf_t *w, uint64_t v) {
  uint8_t b[10];
  size_t n = 0;
  do {
    b[n] = (uint8_t)(v & 0x7F);
    v >>= 7;
    if (v)
      b[n] |= 0x80;
    n++;
  } while (v);
  put(w, b, n);
}

static void put_key(wbuf_t *w, uint32_t num, uint32_t wire) { put_varint(w, (uint64_t)num << 3 | wire); }

static void put_fixed(wbuf_t *w, uint64_t v, size_t n) {
  uint8_t b[8];
  for (size_t i = 0; i < n; i++)
    b[i] = (uint8_t)(v >> (8 * i));
  put(w, b, n);
}

static void put_string(wbuf_t *w, uint32_t num, const char *s) {
  size_t n = strlen(s);
  put_key(w, num, WT_LEN);
  put_varint(w, n);
  put(w, s, n);
}

/* Length-delimited submessage: reserve one length byte (all our submessages are < 128
 * bytes in practice), and shift if it turned out longer. */
static size_t begin_sub(wbuf_t *w, uint32_t num) {
  put_key(w, num, WT_LEN);
  put(w, "\0", 1);
  return w->len;
}

static void end_sub(wbuf_t *w, size_t start) {
  if (w->overflow)
    return;
  size_t n = w->len - start;
  uint8_t b[10];
  size_t vlen = 0;
  uint64_t v = n;
  do {
    b[vlen] = (uint8_t)(v & 0x7F);
    v >>= 7;
    if (v)
      b[vlen] |= 0x80;
    vlen++;
  } while (v);
  if (vlen > 1) {
    if (w->len + vlen - 1 > w->cap) {
      w->overflow = true;
      return;
    }
    memmove(w->p + start + vlen - 1, w->p + start, n);
    w->len += vlen - 1;
  }
  memcpy(w->p + start - 1, b, vlen);
}

static void encode_hello(wbuf_t *w, const lc_hello_t *h) {
  if (h->name[0])
    put_string(w, 1, h->name);
  for (uint8_t i = 0; i < h->n_zones && i < LC_ZONES_MAX; i++)
    put_string(w, 2, h->zones[i]);
  if (h->caps) {
    put_key(w, 3, WT_VARINT);
    put_varint(w, h->caps);
  }
  if (h->challenge) {
    put_key(w, 4, WT_I64);
    put_fixed(w, h->challenge, 8);
  }
  if (h->echo) {
    put_key(w, 5, WT_I64);
    put_fixed(w, h->echo, 8);
  }
  if (h->bye) {
    put_key(w, 6, WT_VARINT);
    put_varint(w, 1);
  }
}

int lc_control_encode(const lc_control_t *msg, uint8_t *out, size_t cap) {
  wbuf_t w = {out, 0, cap, false};
  size_t outer, inner;
  switch (msg->type) {
    case LC_MSG_HELLO:
      outer = begin_sub(&w, 1);
      encode_hello(&w, &msg->u.hello);
      end_sub(&w, outer);
      break;
    case LC_MSG_TALK_START: {
      const lc_target_t *t = &msg->u.talk_start.target;
      outer = begin_sub(&w, 2);
      inner = begin_sub(&w, 1);
      if (t->kind == LC_TARGET_DEVICE) {
        put_key(&w, 1, WT_I32);
        put_fixed(&w, t->device, 4);
      } else if (t->kind == LC_TARGET_ZONE) {
        put_string(&w, 2, t->zone);
      } else {
        put_key(&w, 3, WT_VARINT);
        put_varint(&w, 1);
      }
      end_sub(&w, inner);
      if (msg->u.talk_start.stream_id) {
        put_key(&w, 2, WT_VARINT);
        put_varint(&w, msg->u.talk_start.stream_id);
      }
      end_sub(&w, outer);
      break;
    }
    case LC_MSG_TALK_STOP:
      outer = begin_sub(&w, 3);
      if (msg->u.talk_stop.stream_id) {
        put_key(&w, 1, WT_VARINT);
        put_varint(&w, msg->u.talk_stop.stream_id);
      }
      end_sub(&w, outer);
      break;
    default:
      return -1;
  }
  return w.overflow ? -1 : (int)w.len;
}

/* --- reader -------------------------------------------------------------- */

typedef struct {
  uint32_t num, wire;
  uint64_t value;      /* varint / fixed */
  const uint8_t *data; /* LEN */
  size_t len;
} field_t;

typedef struct {
  const uint8_t *p, *end;
  bool error;
} rbuf_t;

static bool get_varint(rbuf_t *r, uint64_t *v) {
  uint64_t result = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (r->p >= r->end)
      return false;
    uint8_t b = *r->p++;
    result |= (uint64_t)(b & 0x7F) << shift;
    if (!(b & 0x80)) {
      *v = result;
      return true;
    }
  }
  return false;
}

/* Returns true and fills `f` for the next field; false at the end or on error (r->error). */
static bool next_field(rbuf_t *r, field_t *f) {
  if (r->error || r->p >= r->end)
    return false;
  uint64_t key;
  if (!get_varint(r, &key) || (key >> 3) == 0 || (key >> 3) > 0x1FFFFFFF) {
    r->error = true;
    return false;
  }
  f->num = (uint32_t)(key >> 3);
  f->wire = (uint32_t)(key & 7);
  size_t avail = (size_t)(r->end - r->p);
  switch (f->wire) {
    case WT_VARINT:
      if (!get_varint(r, &f->value))
        r->error = true;
      break;
    case WT_I64:
    case WT_I32: {
      size_t n = f->wire == WT_I64 ? 8 : 4;
      if (avail < n) {
        r->error = true;
        break;
      }
      f->value = 0;
      for (size_t i = 0; i < n; i++)
        f->value |= (uint64_t)r->p[i] << (8 * i);
      r->p += n;
      break;
    }
    case WT_LEN: {
      uint64_t n;
      if (!get_varint(r, &n) || n > (uint64_t)(r->end - r->p)) {
        r->error = true;
        break;
      }
      f->data = r->p;
      f->len = (size_t)n;
      r->p += n;
      break;
    }
    default:
      r->error = true;
  }
  return !r->error;
}

static bool copy_string(const field_t *f, char *dst, size_t cap) {
  if (f->wire != WT_LEN)
    return false;
  size_t n = f->len < cap - 1 ? f->len : cap - 1;
  /* Don't cut a UTF-8 sequence in half. */
  while (n < f->len && n > 0 && (f->data[n] & 0xC0) == 0x80)
    n--;
  memcpy(dst, f->data, n);
  dst[n] = 0;
  return memchr(dst, 0, n) == NULL;
}

#define EXPECT(f, w) \
  do { \
    if ((f).wire != (w)) \
      return -1; \
  } while (0)

static int decode_hello(const uint8_t *data, size_t len, lc_hello_t *h) {
  memset(h, 0, sizeof(*h));
  rbuf_t r = {data, data + len, false};
  field_t f;
  while (next_field(&r, &f)) {
    switch (f.num) {
      case 1:
        if (!copy_string(&f, h->name, sizeof(h->name)))
          return -1;
        break;
      case 2:
        EXPECT(f, WT_LEN);
        if (h->n_zones < LC_ZONES_MAX) {
          if (!copy_string(&f, h->zones[h->n_zones], sizeof(h->zones[0])))
            return -1;
          h->n_zones++;
        }
        break;
      case 3:
        EXPECT(f, WT_VARINT);
        h->caps = (uint32_t)f.value;
        break;
      case 4:
        EXPECT(f, WT_I64);
        h->challenge = f.value;
        break;
      case 5:
        EXPECT(f, WT_I64);
        h->echo = f.value;
        break;
      case 6:
        EXPECT(f, WT_VARINT);
        h->bye = f.value != 0;
        break;
      default:
        break;
    }
  }
  return r.error ? -1 : 0;
}

static int decode_target(const uint8_t *data, size_t len, lc_target_t *t) {
  memset(t, 0, sizeof(*t));
  t->kind = LC_TARGET_ALL;
  rbuf_t r = {data, data + len, false};
  field_t f;
  while (next_field(&r, &f)) {
    switch (f.num) {
      case 1:
        EXPECT(f, WT_I32);
        t->kind = LC_TARGET_DEVICE;
        t->device = (uint32_t)f.value;
        break;
      case 2:
        t->kind = LC_TARGET_ZONE;
        if (!copy_string(&f, t->zone, sizeof(t->zone)))
          return -1;
        break;
      case 3:
        EXPECT(f, WT_VARINT);
        t->kind = LC_TARGET_ALL;
        break;
      default:
        break;
    }
  }
  return r.error ? -1 : 0;
}

int lc_control_decode(const uint8_t *data, size_t len, lc_control_t *msg) {
  memset(msg, 0, sizeof(*msg));
  rbuf_t r = {data, data + len, false};
  field_t f, g;
  while (next_field(&r, &f)) {
    if (f.num < 1 || f.num > 3)
      continue;
    EXPECT(f, WT_LEN);
    if (f.num == 1) {
      msg->type = LC_MSG_HELLO;
      if (decode_hello(f.data, f.len, &msg->u.hello))
        return -1;
    } else if (f.num == 2) {
      msg->type = LC_MSG_TALK_START;
      memset(&msg->u.talk_start, 0, sizeof(msg->u.talk_start));
      rbuf_t s = {f.data, f.data + f.len, false};
      while (next_field(&s, &g)) {
        if (g.num == 1) {
          EXPECT(g, WT_LEN);
          if (decode_target(g.data, g.len, &msg->u.talk_start.target))
            return -1;
        } else if (g.num == 2) {
          EXPECT(g, WT_VARINT);
          msg->u.talk_start.stream_id = (uint32_t)g.value;
        }
      }
      if (s.error)
        return -1;
    } else {
      msg->type = LC_MSG_TALK_STOP;
      msg->u.talk_stop.stream_id = 0;
      rbuf_t s = {f.data, f.data + f.len, false};
      while (next_field(&s, &g)) {
        if (g.num == 1) {
          EXPECT(g, WT_VARINT);
          msg->u.talk_stop.stream_id = (uint32_t)g.value;
        }
      }
      if (s.error)
        return -1;
    }
  }
  return r.error ? -1 : 0;
}

bool lc_target_matches(const lc_target_t *t, uint32_t sender_id, const lc_hello_t *hello) {
  switch (t->kind) {
    case LC_TARGET_DEVICE:
      return t->device == sender_id;
    case LC_TARGET_ZONE:
      for (uint8_t i = 0; i < hello->n_zones; i++)
        if (strcmp(hello->zones[i], t->zone) == 0)
          return true;
      return false;
    default:
      return true;
  }
}

int lc_target_parse(const char *text, lc_target_t *t) {
  memset(t, 0, sizeof(*t));
  if (text == NULL || strcmp(text, "all") == 0 || text[0] == 0) {
    t->kind = LC_TARGET_ALL;
    return 0;
  }
  if (strncmp(text, "zone:", 5) == 0 && text[5] && strlen(text + 5) <= LC_ZONE_MAX) {
    t->kind = LC_TARGET_ZONE;
    strcpy(t->zone, text + 5);
    return 0;
  }
  if (strncmp(text, "device:", 7) == 0 && text[7]) {
    char *end;
    unsigned long v = strtoul(text + 7, &end, 16);
    if (*end || v == 0 || v > 0xFFFFFFFFul)
      return -1;
    t->kind = LC_TARGET_DEVICE;
    t->device = (uint32_t)v;
    return 0;
  }
  return -1;
}
