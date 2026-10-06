// SPDX-License-Identifier: GPL-2.0
/*
 * json2dir: convert a JSON object into a directory tree, in-kernel.
 *
 * Write to /sys/kernel/json2dir/data with the payload
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

#define MAX_DEPTH 64
#define MAX_NODES 4096

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
	unsigned int depth;
	unsigned int nodes;
};

static void json_free(struct json_value *v)
{
	unsigned int i;

	if (!v)
		return;

	switch (v->type) {
	case J_OBJECT:
		for (i = 0; i < v->u.obj.count; i++) {
			kfree(v->u.obj.pairs[i].key);
			json_free(v->u.obj.pairs[i].value);
		}
		kfree(v->u.obj.pairs);
		break;
	case J_ARRAY:
		for (i = 0; i < v->u.arr.count; i++)
			json_free(v->u.arr.items[i]);
		kfree(v->u.arr.items);
		break;
	case J_STRING:
		kfree(v->u.str);
		break;
	default:
		break;
	}

	kfree(v);
}

static struct json_value *json_new(enum json_type t)
{
	struct json_value *v = kzalloc(sizeof(*v), GFP_KERNEL);

	if (v)
		v->type = t;
	return v;
}

static int obj_add(struct json_value *v, char *key, struct json_value *val)
{
	unsigned int n = v->u.obj.count;
	struct json_pair *np;

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
 * kmalloc'd, NUL-terminated string or ERR_PTR.
 */
static char *jp_string(struct jparser *jp)
{
	const char *p = jp->p;
	const char *end = jp->end;
	char *out;
	size_t n = 0;

	if (p >= end || *p != '"')
		return ERR_PTR(-EINVAL);
	p++;

	out = kmalloc(end - p + 1, GFP_KERNEL);
	if (!out)
		return ERR_PTR(-ENOMEM);

	while (p < end) {
		unsigned char c = *p;

		if (c == '"') {
			p++;
			jp->p = p;
			out[n] = '\0';
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
	kfree(out);
	return ERR_PTR(-EINVAL);
}

static struct json_value *jp_value(struct jparser *jp);

static struct json_value *jp_object(struct jparser *jp)
{
	struct json_value *v = json_new(J_OBJECT);
	char *key;
	struct json_value *val;
	int err;

	if (!v)
		return ERR_PTR(-ENOMEM);

	jp->p++; /* consume '{' */
	jp_skip_ws(jp);

	if (jp->p < jp->end && *jp->p == '}') {
		jp->p++;
		return v;
	}

	for (;;) {
		jp_skip_ws(jp);
		if (jp->p >= jp->end || *jp->p != '"') {
			err = -EINVAL;
			goto fail;
		}

		key = jp_string(jp);
		if (IS_ERR(key)) {
			err = PTR_ERR(key);
			goto fail;
		}

		jp_skip_ws(jp);
		if (jp->p >= jp->end || *jp->p != ':') {
			kfree(key);
			err = -EINVAL;
			goto fail;
		}
		jp->p++;

		val = jp_value(jp);
		if (IS_ERR(val)) {
			kfree(key);
			err = PTR_ERR(val);
			goto fail;
		}

		err = obj_add(v, key, val);
		if (err) {
			kfree(key);
			json_free(val);
			goto fail;
		}

		jp_skip_ws(jp);
		if (jp->p < jp->end && *jp->p == ',') {
			jp->p++;
			continue;
		}
		if (jp->p < jp->end && *jp->p == '}') {
			jp->p++;
			return v;
		}
		err = -EINVAL;
		goto fail;
	}

fail:
	json_free(v);
	return ERR_PTR(err);
}

static struct json_value *jp_array(struct jparser *jp)
{
	struct json_value *v = json_new(J_ARRAY);
	struct json_value *item;
	int err;

	if (!v)
		return ERR_PTR(-ENOMEM);

	jp->p++; /* consume '[' */
	jp_skip_ws(jp);

	if (jp->p < jp->end && *jp->p == ']') {
		jp->p++;
		return v;
	}

	for (;;) {
		item = jp_value(jp);
		if (IS_ERR(item)) {
			err = PTR_ERR(item);
			goto fail;
		}

		err = arr_add(v, item);
		if (err) {
			json_free(item);
			goto fail;
		}

		jp_skip_ws(jp);
		if (jp->p < jp->end && *jp->p == ',') {
			jp->p++;
			continue;
		}
		if (jp->p < jp->end && *jp->p == ']') {
			jp->p++;
			return v;
		}
		err = -EINVAL;
		goto fail;
	}

fail:
	json_free(v);
	return ERR_PTR(err);
}

static struct json_value *jp_literal(struct jparser *jp, enum json_type t,
				     const char *word)
{
	size_t len = strlen(word);
	struct json_value *v;

	if (jp->end - jp->p < (long)len || strncmp(jp->p, word, len))
		return ERR_PTR(-EINVAL);

	v = json_new(t);
	if (!v)
		return ERR_PTR(-ENOMEM);

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

	v = json_new(J_NUMBER);
	if (!v)
		return ERR_PTR(-ENOMEM);

	jp->p = p;
	return v;
}

static struct json_value *jp_value(struct jparser *jp)
{
	struct json_value *v;

	if (jp->depth >= MAX_DEPTH)
		return ERR_PTR(-EINVAL);
	if (++jp->nodes > MAX_NODES)
		return ERR_PTR(-EINVAL);

	jp_skip_ws(jp);
	if (jp->p >= jp->end)
		return ERR_PTR(-EINVAL);

	jp->depth++;

	switch (*jp->p) {
	case '{':
		v = jp_object(jp);
		break;
	case '[':
		v = jp_array(jp);
		break;
	case '"': {
		char *s = jp_string(jp);

		if (IS_ERR(s)) {
			jp->depth--;
			return ERR_CAST(s);
		}
		v = json_new(J_STRING);
		if (!v) {
			kfree(s);
			jp->depth--;
			return ERR_PTR(-ENOMEM);
		}
		v->u.str = s;
		break;
	}
	case 't':
		v = jp_literal(jp, J_BOOL, "true");
		break;
	case 'f':
		v = jp_literal(jp, J_BOOL, "false");
		break;
	case 'n':
		v = jp_literal(jp, J_NULL, "null");
		break;
	default:
		v = jp_number(jp);
		break;
	}

	jp->depth--;
	return v;
}

/*
 * Parse a NUL-terminated JSON text.  Returns the top-level value, which
 * must be an object, or ERR_PTR.
 */
static struct json_value *json_parse(const char *text, size_t len)
{
	struct jparser jp = {
		.p = text,
		.end = text + len,
	};
	struct json_value *v;

	v = jp_value(&jp);
	if (IS_ERR(v))
		return v;

	jp_skip_ws(&jp);
	if (jp.p != jp.end) {
		json_free(v);
		return ERR_PTR(-EINVAL); /* trailing garbage */
	}

	return v;
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
	dentry = vfs_mkdir(idmap, d_inode(parent.dentry), dentry, 0755, NULL);
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

/* Try to remove whatever (file or symlink) is at path; errors ignored. */
static void try_unlink(const char *path)
{
	const char *slash;
	const char *parent_str;
	const char *name_str;
	char *parent_buf = NULL;
	struct path parent;
	struct qstr name;
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
			return;
		memcpy(parent_buf, path, plen);
		parent_buf[plen] = '\0';
		parent_str = parent_buf;
		name_str = slash + 1;
	}

	if (name_str[0] == '\0')
		goto out;

	err = kern_path(parent_str, LOOKUP_DIRECTORY | LOOKUP_FOLLOW, &parent);
	if (err)
		goto out;

	name.name = name_str;
	name.len = strlen(name_str);

	idmap = mnt_idmap(parent.mnt);
	dentry = start_removing(idmap, parent.dentry, &name);
	if (IS_ERR(dentry)) {
		path_put(&parent);
		goto out;
	}

	vfs_unlink(idmap, d_inode(parent.dentry), dentry, NULL);
	end_dirop(dentry);
	path_put(&parent);

out:
	kfree(parent_buf);
}

static int file_add_exec(struct file *f)
{
	struct inode *inode = file_inode(f);
	struct mnt_idmap *idmap = mnt_idmap(f->f_path.mnt);
	struct iattr attr;
	int err;

	attr.ia_valid = ATTR_MODE | ATTR_CTIME;
	attr.ia_mode = inode->i_mode | 0111;

	inode_lock(inode);
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

	f = filp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (IS_ERR(f))
		return PTR_ERR(f);

	written = kernel_write(f, data, len, &pos);
	if (written < 0)
		err = written;
	else if ((size_t)written != len)
		err = -EIO;

	if (!err && executable)
		err = file_add_exec(f);

	filp_close(f, NULL);
	return err;
}

/* ------------------------------------------------------------------ */
/* Tree walker                                                         */
/* ------------------------------------------------------------------ */

static int json_object_to_dir(const char *base, struct json_value *object,
			      unsigned int depth);

static int handle_value(const char *base, const char *name,
			struct json_value *value, unsigned int depth)
{
	char *path;
	int err;

	if (!valid_component(name))
		return -EINVAL;

	path = path_join(base, name);
	if (!path)
		return -ENOMEM;

	/* Ignore errors: delete an existing file/symlink first, mirroring
	 * the original's fs::remove_file(). */
	try_unlink(path);

	switch (value->type) {
	case J_OBJECT:
		err = make_dir(path);
		if (err == -EEXIST)
			err = 0;
		if (!err)
			err = json_object_to_dir(path, value, depth + 1);
		break;

	case J_STRING:
		err = write_file(path, value->u.str, strlen(value->u.str), false);
		break;

	case J_ARRAY: {
		struct json_value **items = value->u.arr.items;
		const char *kind, *payload;

		if (value->u.arr.count != 2 ||
		    items[0]->type != J_STRING ||
		    items[1]->type != J_STRING) {
			err = -EINVAL;
			break;
		}

		kind = items[0]->u.str;
		payload = items[1]->u.str;

		if (strcmp(kind, "link") == 0) {
			err = make_symlink(path, payload);
		} else if (strcmp(kind, "script") == 0) {
			err = write_file(path, payload, strlen(payload), true);
		} else {
			err = -EINVAL;
		}
		break;
	}

	default:
		err = -EINVAL; /* number/bool/null not allowed */
		break;
	}

	kfree(path);
	return err;
}

static int json_object_to_dir(const char *base, struct json_value *object,
			      unsigned int depth)
{
	unsigned int i;
	int err;

	if (depth >= MAX_DEPTH)
		return -EINVAL;

	for (i = 0; i < object->u.obj.count; i++) {
		err = handle_value(base, object->u.obj.pairs[i].key,
				   object->u.obj.pairs[i].value, depth);
		if (err)
			return err;
	}

	return 0;
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

	copy = kmalloc(count + 1, GFP_KERNEL);
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

	err = json_object_to_dir(base, root, 0);

	json_free(root);

out_copy:
	kfree(copy);
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

	pr_info("loaded; write '<base>\\0<json>' to /sys/kernel/json2dir/data\n");
	return 0;
}

static void __exit json2dir_exit(void)
{
	sysfs_remove_file(json2dir_kobj, &data_attr.attr);
	kobject_put(json2dir_kobj);
	pr_info("unloaded\n");
}

module_init(json2dir_init);
module_exit(json2dir_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("json2dir-syscall");
MODULE_DESCRIPTION("Convert a JSON object into a directory tree");
