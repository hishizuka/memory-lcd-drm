#ifndef TESTS_STUBS_LINUX_DEBUGFS_H_
#define TESTS_STUBS_LINUX_DEBUGFS_H_

struct dentry {
	int unused;
};

struct file_operations;

static inline struct dentry *debugfs_create_dir(const char *name,
	struct dentry *parent)
{
	static struct dentry dentry;

	(void)name;
	(void)parent;
	return &dentry;
}

static inline struct dentry *debugfs_create_file(const char *name,
	unsigned int mode, struct dentry *parent, void *data,
	const struct file_operations *operations)
{
	(void)name;
	(void)mode;
	(void)data;
	(void)operations;
	return parent;
}

static inline void debugfs_remove_recursive(struct dentry *dentry)
{
	(void)dentry;
}

#endif
