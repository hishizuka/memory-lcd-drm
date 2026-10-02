#ifndef TESTS_STUBS_LINUX_SEQ_FILE_H_
#define TESTS_STUBS_LINUX_SEQ_FILE_H_

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

typedef long long loff_t;

struct inode {
	void *i_private;
};

struct file {
	void *private_data;
};

struct seq_file {
	void *private;
};

struct file_operations {
	void *owner;
	int (*open)(struct inode *inode, struct file *file);
	ssize_t (*read)(struct file *file, char *buf, size_t count,
		loff_t *position);
	ssize_t (*write)(struct file *file, const char *buf, size_t count,
		loff_t *position);
	loff_t (*llseek)(struct file *file, loff_t offset, int whence);
	int (*release)(struct inode *inode, struct file *file);
};

static inline int seq_printf(struct seq_file *seq, const char *format, ...)
{
	(void)seq;
	(void)format;
	return 0;
}

static inline int single_open(struct file *file,
	int (*show)(struct seq_file *, void *), void *data)
{
	static struct seq_file seq;

	(void)show;
	seq.private = data;
	file->private_data = &seq;
	return 0;
}

static inline int single_release(struct inode *inode, struct file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

static inline ssize_t seq_read(struct file *file, char *buf, size_t count,
	loff_t *position)
{
	(void)file;
	(void)buf;
	(void)count;
	(void)position;
	return 0;
}

static inline loff_t seq_lseek(struct file *file, loff_t offset, int whence)
{
	(void)file;
	(void)whence;
	return offset;
}

#endif
