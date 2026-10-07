// SPDX-License-Identifier: GPL-2.0
/*
 * json2dir: convert a JSON object into a directory tree, in-kernel.
 *
 * Write one complete request to /dev/json2dir (or, for small requests,
 * /sys/kernel/json2dir/data) with the payload
 *
 *     <base-path>\0<json>
 *
 * where <base-path> is the directory under which the tree is created
 * (absolute, or relative to the writing process's cwd) and <json> is a
 * JSON object following the json2dir conversion scheme:
 *
 *   object               -> directory
 *   string               -> regular file (contents)
 *   ["link", target]     -> symbolic link
 *   ["script", content]  -> executable file
 *
 * Mirrors the behaviour of the original json2dir CLI (github.com/alurm/json2dir).
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/file.h>
#include <linux/mnt_idmapping.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/vmalloc.h>

#define MAX_DEPTH 128
#define MAX_NODES 16384
#define MAX_PAYLOAD (4 * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* JSON DOM                                                            */
/* ------------------------------------------------------------------ */

enum json_type {
	J_OBJECT,
	J_ARRAY,
	J_STRING,
	J_NUMBER,
	J_BOOL,
	J_NULL,
};

struct json_pair {
	char *key;
	struct json_value *value;
};

struct json_value {
	enum json_type type;
	size_t str_len;
	struct json_value *next_alloc;
	union {
		struct {
			struct json_pair *pairs;
			unsigned int count;
		} obj;
		struct {
			struct json_value **items;
			unsigned int count;
		} arr;
		char *str;
	} u;
};

struct jparser {
	const char *p;
	const char *end;
	unsigned int nodes;
	struct json_value *first, *last;
};

/* Allocation order is independent of tree shape: destruction never recurses. */
static void json_free(struct json_value *v)
{
	while (v) {
		struct json_value *next = v->next_alloc;
		unsigned int i;

		if (v->type == J_OBJECT) {
			for (i = 0; i < v->u.obj.count; i++)
				kvfree(v->u.obj.pairs[i].key);
			kfree(v->u.obj.pairs);
		} else if (v->type == J_ARRAY) {
			kfree(v->u.arr.items);
		} else if (v->type == J_STRING) {
			kvfree(v->u.str);
		}
		kfree(v);
		v = next;
	}
}

static struct json_value *json_new(struct jparser *jp, enum json_type t)
{
	struct json_value *v;

	if (++jp->nodes > MAX_NODES)
		return ERR_PTR(-E2BIG);
	v = kzalloc(sizeof(*v), GFP_KERNEL);
	if (!v)
		return ERR_PTR(-ENOMEM);
	v->type = t;
	if (jp->last)
		jp->last->next_alloc = v;
	else
		jp->first = v;
	jp->last = v;
	return v;
}

static int obj_add(struct json_value *v, char *key, struct json_value *val)
{
	unsigned int n = v->u.obj.count;
	struct json_pair *np;
	unsigned int i;

	/* RFC 4.2 permits rejecting duplicates, including escaped equivalents. */
	for (i = 0; i < n; i++)
		if (!strcmp(v->u.obj.pairs[i].key, key))
			return -EINVAL;
	np = krealloc_array(v->u.obj.pairs, n + 1, sizeof(*np), GFP_KERNEL);
	if (!np)
		return -ENOMEM;

	v->u.obj.pairs = np;
	np[n].key = key;
	np[n].value = val;
	v->u.obj.count = n + 1;
	return 0;
}

static int arr_add(struct json_value *v, struct json_value *item)
{
	unsigned int n = v->u.arr.count;
	struct json_value **np;

	np = krealloc_array(v->u.arr.items, n + 1, sizeof(*np), GFP_KERNEL);
	if (!np)
		return -ENOMEM;

	v->u.arr.items = np;
	np[n] = item;
	v->u.arr.count = n + 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* JSON parser                                                         */
/* ------------------------------------------------------------------ */

static void jp_skip_ws(struct jparser *jp)
{
	while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\t' ||
				   *jp->p == '\n' || *jp->p == '\r'))
		jp->p++;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static unsigned int utf8_encode(unsigned int cp, char *out)
{
	if (cp < 0x80) {
		out[0] = cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = 0xC0 | (cp >> 6);
		out[1] = 0x80 | (cp & 0x3F);
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = 0xE0 | (cp >> 12);
		out[1] = 0x80 | ((cp >> 6) & 0x3F);
		out[2] = 0x80 | (cp & 0x3F);
		return 3;
	}
	out[0] = 0xF0 | (cp >> 18);
	out[1] = 0x80 | ((cp >> 12) & 0x3F);
	out[2] = 0x80 | ((cp >> 6) & 0x3F);
	out[3] = 0x80 | (cp & 0x3F);
	return 4;
}

/*
 * Parse a JSON string (the leading '"' must be at jp->p).  Returns a
 * kvmalloc'd string with an explicit byte length and a trailing NUL, or ERR_PTR.
 */
static char *jp_string(struct jparser *jp, size_t *length)
{
	const char *p = jp->p;
	const char *end = jp->end;
	const char *limit;
	char *out;
	size_t n = 0;

	if (p >= end || *p != '"')
		return ERR_PTR(-EINVAL);
	p++;

	/* Decoding cannot exceed the raw token length. Avoid allocating the
	 * remainder of a large document separately for every short string. */
	for (limit = p; limit < end && *limit != '"'; limit++) {
		if (*limit == '\\') {
			if (++limit == end)
				return ERR_PTR(-EINVAL);
		}
	}
	if (limit == end)
		return ERR_PTR(-EINVAL);
	out = kvmalloc(limit - p + 1, GFP_KERNEL);
	if (!out)
		return ERR_PTR(-ENOMEM);

	while (p < end) {
		unsigned char c = *p;

		if (c == '"') {
			p++;
			jp->p = p;
			out[n] = '\0';
			*length = n;
			return out;
		}

		if (c == '\\') {
			p++;
			if (p >= end)
				goto bad;

			switch (*p) {
			case '"': out[n++] = '"'; p++; break;
			case '\\': out[n++] = '\\'; p++; break;
			case '/': out[n++] = '/'; p++; break;
			case 'b': out[n++] = '\b'; p++; break;
			case 'f': out[n++] = '\f'; p++; break;
			case 'n': out[n++] = '\n'; p++; break;
			case 'r': out[n++] = '\r'; p++; break;
			case 't': out[n++] = '\t'; p++; break;
			case 'u': {
				unsigned int cp, lo;
				int a, b, c2, d;

				p++;
				if (p + 4 > end)
					goto bad;
				a = hexval(p[0]);
				b = hexval(p[1]);
				c2 = hexval(p[2]);
				d = hexval(p[3]);
				if (a < 0 || b < 0 || c2 < 0 || d < 0)
					goto bad;
				cp = (a << 12) | (b << 8) | (c2 << 4) | d;
				p += 4;

				if (cp >= 0xD800 && cp <= 0xDBFF) {
					/* high surrogate: expect \uXXXX low */
					if (p + 2 > end || p[0] != '\\' || p[1] != 'u')
						goto bad;
					p += 2;
					if (p + 4 > end)
						goto bad;
					a = hexval(p[0]);
					b = hexval(p[1]);
					c2 = hexval(p[2]);
					d = hexval(p[3]);
					if (a < 0 || b < 0 || c2 < 0 || d < 0)
						goto bad;
					lo = (a << 12) | (b << 8) | (c2 << 4) | d;
					p += 4;
					if (lo < 0xDC00 || lo > 0xDFFF)
						goto bad;
					cp = 0x10000 + ((cp - 0xD800) << 10) +
					     (lo - 0xDC00);
				} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
					goto bad; /* lone low surrogate */
				}
				n += utf8_encode(cp, out + n);
				break;
			}
			default:
				goto bad;
			}
		} else if (c < 0x20) {
			goto bad;
		} else {
			out[n++] = c;
			p++;
		}
	}

bad:
	kvfree(out);
	return ERR_PTR(-EINVAL);
}

static struct json_value *jp_literal(struct jparser *jp, enum json_type t,
				     const char *word)
{
	size_t len = strlen(word);
	struct json_value *v;

	if (jp->end - jp->p < (long)len || strncmp(jp->p, word, len))
		return ERR_PTR(-EINVAL);

	v = json_new(jp, t);
	if (IS_ERR(v))
		return v;

	jp->p += len;
	return v;
}

static struct json_value *jp_number(struct jparser *jp)
{
	const char *p = jp->p;
	struct json_value *v;

	if (p < jp->end && *p == '-')
		p++;
	if (p >= jp->end || *p < '0' || *p > '9')
		return ERR_PTR(-EINVAL);
	if (*p == '0') {
		p++;
		if (p < jp->end && *p >= '0' && *p <= '9')
			return ERR_PTR(-EINVAL); /* leading zero */
	} else {
		while (p < jp->end && *p >= '0' && *p <= '9')
			p++;
	}
	if (p < jp->end && *p == '.') {
		p++;
		if (p >= jp->end || *p < '0' || *p > '9')
			return ERR_PTR(-EINVAL);
		while (p < jp->end && *p >= '0' && *p <= '9')
			p++;
	}
	if (p < jp->end && (*p == 'e' || *p == 'E')) {
		p++;
		if (p < jp->end && (*p == '+' || *p == '-'))
			p++;
		if (p >= jp->end || *p < '0' || *p > '9')
			return ERR_PTR(-EINVAL);
		while (p < jp->end && *p >= '0' && *p <= '9')
			p++;
	}

	v = json_new(jp, J_NUMBER);
	if (IS_ERR(v))
		return v;

	jp->p = p;
	return v;
}

/* Read one token; container children are handled by json_parse's heap stack. */
static struct json_value *jp_value(struct jparser *jp)
{
	struct json_value *v;
	char *str;
	size_t len;

	jp_skip_ws(jp);
	if (jp->p == jp->end)
		return ERR_PTR(-EINVAL);
	switch (*jp->p) {
	case '{':
		jp->p++;
		return json_new(jp, J_OBJECT);
	case '[':
		jp->p++;
		return json_new(jp, J_ARRAY);
	case '"':
		str = jp_string(jp, &len);
		if (IS_ERR(str))
			return ERR_CAST(str);
		v = json_new(jp, J_STRING);
		if (IS_ERR(v)) {
			kvfree(str);
			return v;
		}
		v->u.str = str;
		v->str_len = len;
		return v;
	case 't': return jp_literal(jp, J_BOOL, "true");
	case 'f': return jp_literal(jp, J_BOOL, "false");
	case 'n': return jp_literal(jp, J_NULL, "null");
	default: return jp_number(jp);
	}
}

static bool valid_utf8(const unsigned char *s, size_t len)
{
	size_t i = 0;

	while (i < len) {
		unsigned int c = s[i++], cp, min, n;

		if (c < 0x80)
			continue;
		if (c >= 0xc2 && c <= 0xdf) {
			cp = c & 0x1f; min = 0x80; n = 1;
		} else if (c >= 0xe0 && c <= 0xef) {
			cp = c & 0xf; min = 0x800; n = 2;
		} else if (c >= 0xf0 && c <= 0xf4) {
			cp = c & 7; min = 0x10000; n = 3;
		} else {
			return false;
		}
		if (len - i < n)
			return false;
		while (n--) {
			c = s[i++];
			if ((c & 0xc0) != 0x80)
				return false;
			cp = (cp << 6) | (c & 0x3f);
		}
		if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
			return false;
	}
	return true;
}

struct parse_frame {
	struct json_value *value;
	/* 0: first member or end; 1: member required; 2: separator or end. */
	unsigned int state;
};

static struct json_value *json_parse(const char *text, size_t len)
{
	struct jparser jp = { .p = text, .end = text + len };
	struct parse_frame *stack;
	struct json_value *root, *child;
	unsigned int depth = 0;
	int err = -EINVAL;

	if (!valid_utf8((const unsigned char *)text, len))
		return ERR_PTR(-EINVAL);
	stack = kcalloc(MAX_DEPTH, sizeof(*stack), GFP_KERNEL);
	if (!stack)
		return ERR_PTR(-ENOMEM);
	root = jp_value(&jp);
	if (IS_ERR(root)) {
		err = PTR_ERR(root);
		goto fail;
	}
	if (root->type != J_OBJECT)
		goto fail;
	stack[depth++].value = root;
	while (depth) {
		struct parse_frame *f = &stack[depth - 1];
		struct json_value *parent = f->value;
		char close = parent->type == J_OBJECT ? '}' : ']';
		char *key = NULL;
		size_t key_len;

		jp_skip_ws(&jp);
		if (jp.p == jp.end)
			goto invalid;
		if (f->state != 1 && *jp.p == close) {
			jp.p++;
			depth--;
			continue;
		}
		if (f->state == 2) {
			if (*jp.p++ != ',')
				goto invalid;
			f->state = 1;
			continue;
		}
		if (parent->type == J_OBJECT) {
			key = jp_string(&jp, &key_len);
			if (IS_ERR(key)) {
				err = PTR_ERR(key);
				goto fail;
			}
			jp_skip_ws(&jp);
			if (memchr(key, 0, key_len) || jp.p == jp.end || *jp.p++ != ':') {
				kvfree(key);
				goto invalid;
			}
		}
		child = jp_value(&jp);
		if (IS_ERR(child)) {
			kvfree(key);
			err = PTR_ERR(child);
			goto fail;
		}
		err = parent->type == J_OBJECT ? obj_add(parent, key, child) : arr_add(parent, child);
		if (err) {
			kvfree(key);
			goto fail;
		}
		f->state = 2;
		if (child->type == J_OBJECT || child->type == J_ARRAY) {
			if (depth == MAX_DEPTH)
				goto invalid;
			stack[depth++] = (struct parse_frame) { .value = child };
		}
	}
	jp_skip_ws(&jp);
	if (jp.p != jp.end)
		goto invalid;
	kfree(stack);
	return root;
invalid:
	err = -EINVAL;
fail:
	json_free(jp.first);
	kfree(stack);
	return ERR_PTR(err);
}

/* ------------------------------------------------------------------ */
/* Filesystem helpers                                                  */
/* ------------------------------------------------------------------ */

static char *path_join(const char *parent, const char *name)
{
	size_t plen = strlen(parent);
	size_t nlen = strlen(name);
	bool slash = plen != 0 && parent[plen - 1] != '/';
	char *out;

	out = kmalloc(plen + (slash ? 1 : 0) + nlen + 1, GFP_KERNEL);
	if (!out)
		return NULL;

	memcpy(out, parent, plen);
	if (slash)
		out[plen++] = '/';
	memcpy(out + plen, name, nlen + 1);
	return out;
}

/* True iff name is exactly one normal path component. */
static bool valid_component(const char *name)
{
	size_t len = strlen(name);

	if (len == 0)
		return false;
	if (name[0] == '.' && (len == 1 || (len == 2 && name[1] == '.')))
		return false;
	while (*name) {
		if (*name == '/')
			return false;
		name++;
	}
	return true;
}

static int make_dir(const char *path)
{
	struct path parent;
	struct dentry *dentry;
	struct mnt_idmap *idmap;
	int err;

	dentry = start_creating_path(AT_FDCWD, path, &parent, LOOKUP_DIRECTORY);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	idmap = mnt_idmap(parent.mnt);
	dentry = vfs_mkdir(idmap, d_inode(parent.dentry), dentry, 0777, NULL);
	if (IS_ERR(dentry))
		err = PTR_ERR(dentry);
	else
		err = 0;

	end_creating_path(&parent, dentry);
	return err;
}

static int make_symlink(const char *path, const char *target)
{
	struct path parent;
	struct dentry *dentry;
	struct mnt_idmap *idmap;
	int err;

	dentry = start_creating_path(AT_FDCWD, path, &parent, LOOKUP_DIRECTORY);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	idmap = mnt_idmap(parent.mnt);
	err = vfs_symlink(idmap, d_inode(parent.dentry), dentry, target, NULL);

	end_creating_path(&parent, dentry);
	return err;
}

/* Remove the entry itself. Directories remain for merge/error handling. */
static int try_unlink(const char *path)
{
	const char *slash;
	const char *parent_str;
	const char *name_str;
	char *parent_buf = NULL;
	struct path parent;
	struct qstr name = { };
	struct dentry *dentry;
	struct mnt_idmap *idmap;
	int err;

	slash = strrchr(path, '/');
	if (!slash) {
		parent_str = ".";
		name_str = path;
	} else if (slash == path) {
		parent_str = "/";
		name_str = slash + 1;
	} else {
		size_t plen = slash - path;

		parent_buf = kmalloc(plen + 1, GFP_KERNEL);
		if (!parent_buf)
			return -ENOMEM;
		memcpy(parent_buf, path, plen);
		parent_buf[plen] = '\0';
		parent_str = parent_buf;
		name_str = slash + 1;
	}

	if (name_str[0] == '\0') {
		err = -EINVAL;
		goto out;
	}

	err = kern_path(parent_str, LOOKUP_DIRECTORY | LOOKUP_FOLLOW, &parent);
	if (err)
		goto out;

	name.name = name_str;
	name.len = strlen(name_str);

	err = mnt_want_write(parent.mnt);
	if (err) {
		path_put(&parent);
		goto out;
	}
	idmap = mnt_idmap(parent.mnt);
	dentry = start_removing(idmap, parent.dentry, &name);
	if (IS_ERR(dentry)) {
		err = PTR_ERR(dentry);
		mnt_drop_write(parent.mnt);
		path_put(&parent);
		goto out;
	}

	err = vfs_unlink(idmap, d_inode(parent.dentry), dentry, NULL);
	end_dirop(dentry);
	mnt_drop_write(parent.mnt);
	path_put(&parent);

out:
	kfree(parent_buf);
	return err == -ENOENT || err == -EISDIR ? 0 : err;
}

static int file_add_exec(struct file *f)
{
	struct inode *inode = file_inode(f);
	struct mnt_idmap *idmap = mnt_idmap(f->f_path.mnt);
	struct iattr attr = { };
	int err;

	attr.ia_valid = ATTR_MODE | ATTR_CTIME;
	inode_lock(inode);
	attr.ia_mode = inode->i_mode | 0111;
	err = setattr_prepare(idmap, f->f_path.dentry, &attr);
	if (!err)
		err = notify_change(idmap, f->f_path.dentry, &attr, NULL);
	inode_unlock(inode);

	return err;
}

static int write_file(const char *path, const char *data, size_t len,
		      bool executable)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t written;
	int err = 0;

	f = filp_open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
	if (IS_ERR(f))
		return PTR_ERR(f);

	while (len) {
		written = kernel_write(f, data, len, &pos);
		if (written <= 0) {
			err = written < 0 ? written : -EIO;
			break;
		}
		data += written;
		len -= written;
	}

	if (!err && executable)
		err = file_add_exec(f);

	filp_close(f, NULL);
	return err;
}

/* ------------------------------------------------------------------ */
/* Tree walker                                                         */
/* ------------------------------------------------------------------ */

static bool string_is(const struct json_value *v, const char *s)
{
	return v->type == J_STRING && v->str_len == strlen(s) &&
	       !memcmp(v->u.str, s, v->str_len);
}

/* Validate the whole scheme before any filesystem operation. */
static int validate_document(struct json_value *root)
{
	struct json_value *node;
	unsigned int i;

	for (node = root; node; node = node->next_alloc) {
		if (node->type != J_OBJECT)
			continue;
		for (i = 0; i < node->u.obj.count; i++) {
			struct json_pair *pair = &node->u.obj.pairs[i];
			struct json_value *v = pair->value;
			struct json_value **items;

			if (!valid_component(pair->key))
				return -EINVAL;
			if (v->type == J_OBJECT || v->type == J_STRING)
				continue;
			if (v->type != J_ARRAY || v->u.arr.count != 2)
				return -EINVAL;
			items = v->u.arr.items;
			if (items[1]->type != J_STRING)
				return -EINVAL;
			if (string_is(items[0], "link")) {
				if (!items[1]->str_len ||
				    memchr(items[1]->u.str, 0, items[1]->str_len))
					return -EINVAL;
			} else if (!string_is(items[0], "script")) {
				return -EINVAL;
			}
		}
	}
	return 0;
}

static int ensure_dir(const char *path)
{
	struct path existing;
	int err = make_dir(path);

	if (err != -EEXIST)
		return err;
	/* EEXIST alone does not establish that the entry is a directory. */
	err = kern_path(path, 0, &existing);
	if (err)
		return err;
	err = d_is_dir(existing.dentry) ? 0 : -ENOTDIR;
	path_put(&existing);
	return err;
}

struct walk_frame {
	const struct json_value *object;
	char *path;
	unsigned int index;
};

static int json_object_to_dir(const char *base, struct json_value *object)
{
	struct walk_frame *stack;
	unsigned int depth = 1;
	int err = validate_document(object);

	if (err)
		return err;
	stack = kcalloc(MAX_DEPTH, sizeof(*stack), GFP_KERNEL);
	if (!stack)
		return -ENOMEM;
	stack[0].object = object;
	stack[0].path = kstrdup(base, GFP_KERNEL);
	if (!stack[0].path) {
		err = -ENOMEM;
		goto out;
	}
	while (depth) {
		struct walk_frame *f = &stack[depth - 1];
		const struct json_pair *pair;
		const struct json_value *v;
		char *path;

		if (f->index == f->object->u.obj.count) {
			kfree(f->path);
			depth--;
			continue;
		}
		pair = &f->object->u.obj.pairs[f->index++];
		v = pair->value;
		path = path_join(f->path, pair->key);
		if (!path) {
			err = -ENOMEM;
			goto out;
		}
		err = try_unlink(path);
		if (!err) {
			if (v->type == J_OBJECT) {
				err = ensure_dir(path);
			} else if (v->type == J_STRING) {
				err = write_file(path, v->u.str, v->str_len, false);
			} else {
				const struct json_value *payload = v->u.arr.items[1];

				if (string_is(v->u.arr.items[0], "link"))
					err = make_symlink(path, payload->u.str);
				else
					err = write_file(path, payload->u.str, payload->str_len, true);
			}
		}
		if (err) {
			kfree(path);
			goto out;
		}
		if (v->type == J_OBJECT) {
			if (depth == MAX_DEPTH) {
				kfree(path);
				err = -E2BIG;
				goto out;
			}
			stack[depth++] = (struct walk_frame) { .object = v, .path = path };
		} else {
			kfree(path);
		}
		cond_resched();
	}
out:
	while (depth)
		kfree(stack[--depth].path);
	kfree(stack);
	return err;
}

/* ------------------------------------------------------------------ */
/* sysfs interface                                                     */
/* ------------------------------------------------------------------ */

static int process_payload(const char *buf, size_t count)
{
	char *copy;
	char *sep;
	char *base;
	struct json_value *root;
	size_t json_len;
	int err;

	if (count > MAX_PAYLOAD)
		return -E2BIG;
	copy = kvmalloc(count + 1, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	memcpy(copy, buf, count);
	copy[count] = '\0';

	sep = memchr(copy, '\0', count);
	if (!sep) {
		err = -EINVAL;
		goto out_copy;
	}

	base = copy;
	if (base == sep)
		base = "."; /* empty base path -> cwd */

	*sep = '\0';
	json_len = copy + count - sep - 1;
	if (json_len == 0) {
		err = -EINVAL;
		goto out_copy;
	}

	root = json_parse(sep + 1, json_len);
	if (IS_ERR(root)) {
		err = PTR_ERR(root);
		goto out_copy;
	}

	if (root->type != J_OBJECT) {
		json_free(root);
		err = -EINVAL;
		goto out_copy;
	}

	err = json_object_to_dir(base, root);

	json_free(root);

out_copy:
	kvfree(copy);
	return err;
}

static ssize_t data_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	int err = process_payload(buf, count);

	if (err)
		return err;
	return count;
}

static struct kobj_attribute data_attr = __ATTR(data, 0200, NULL, data_store);

/* One write is one complete request; no shared staging buffer or commit state.
 * Unlike a text sysfs attribute, the misc device accepts multi-page documents. */
static ssize_t device_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *pos)
{
	char *payload;
	int err;

	if (count > MAX_PAYLOAD)
		return -E2BIG;
	payload = kvmalloc(count ? count : 1, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;
	if (copy_from_user(payload, buf, count)) {
		kvfree(payload);
		return -EFAULT;
	}
	err = process_payload(payload, count);
	kvfree(payload);
	return err ? err : count;
}

static const struct file_operations device_ops = {
	.owner = THIS_MODULE,
	.write = device_write,
	.open = nonseekable_open,
};

static struct miscdevice json2dir_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "json2dir",
	.fops = &device_ops,
	.mode = 0600,
};

static struct kobject *json2dir_kobj;

static int __init json2dir_init(void)
{
	int err;

	json2dir_kobj = kobject_create_and_add("json2dir", kernel_kobj);
	if (!json2dir_kobj)
		return -ENOMEM;

	err = sysfs_create_file(json2dir_kobj, &data_attr.attr);
	if (err) {
		kobject_put(json2dir_kobj);
		return err;
	}

	err = misc_register(&json2dir_device);
	if (err) {
		sysfs_remove_file(json2dir_kobj, &data_attr.attr);
		kobject_put(json2dir_kobj);
		return err;
	}

	pr_info("loaded; write '<base>\\0<json>' to /dev/json2dir or /sys/kernel/json2dir/data\n");
	return 0;
}

static void __exit json2dir_exit(void)
{
	misc_deregister(&json2dir_device);
	sysfs_remove_file(json2dir_kobj, &data_attr.attr);
	kobject_put(json2dir_kobj);
	pr_info("unloaded\n");
}

module_init(json2dir_init);
module_exit(json2dir_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("json2dir-syscall");
MODULE_DESCRIPTION("Convert a JSON object into a directory tree");
