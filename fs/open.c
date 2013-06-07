/*
 *  linux/fs/open.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/string.h>
#include <linux/mm.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/fsnotify.h>
#include <linux/module.h>
#include <linux/tty.h>
#include <linux/namei.h>
#include <linux/backing-dev.h>
#include <linux/capability.h>
#include <linux/securebits.h>
#include <linux/security.h>
#include <linux/mount.h>
#include <linux/fcntl.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/fs.h>
#include <linux/personality.h>
#include <linux/pagemap.h>
#include <linux/syscalls.h>
#include <linux/rcupdate.h>
#include <linux/audit.h>
#include <linux/falloc.h>
#include <linux/fs_struct.h>
#include <linux/ima.h>
#include <linux/dnotify.h>

#include "internal.h"

int do_truncate2(struct vfsmount *mnt, struct dentry *dentry, loff_t length,
		unsigned int time_attrs, struct file *filp)
{
	int ret;
	struct iattr newattrs;

	/* Not pretty: "inode->i_size" shouldn't really be signed. But it is. */
	if (length < 0)
		return -EINVAL;

	newattrs.ia_size = length;
	newattrs.ia_valid = ATTR_SIZE | time_attrs;
	if (filp) {
		newattrs.ia_file = filp;
		newattrs.ia_valid |= ATTR_FILE;
	}

	/* Remove suid/sgid on truncate too */
	ret = should_remove_suid(dentry);
	if (ret)
		newattrs.ia_valid |= ret | ATTR_FORCE;

	mutex_lock(&dentry->d_inode->i_mutex);
	ret = notify_change2(mnt, dentry, &newattrs);
	mutex_unlock(&dentry->d_inode->i_mutex);
	return ret;
}
int do_truncate(struct dentry *dentry, loff_t length, unsigned int time_attrs,
	struct file *filp)
{
	return do_truncate2(NULL, dentry, length, time_attrs, filp);
}

static long do_sys_truncate(const char __user *pathname, loff_t length)
{
	struct path path;
	struct inode *inode;
	struct vfsmount *mnt;
	int error;

	error = -EINVAL;
	if (length < 0)	/* sorry, but loff_t says... */
		goto out;

	error = user_path(pathname, &path);
	if (error)
		goto out;
	inode = path.dentry->d_inode;
	mnt = path.mnt;

	/* For directories it's -EISDIR, for other non-regulars - -EINVAL */
	error = -EISDIR;
	if (S_ISDIR(inode->i_mode))
		goto dput_and_out;

	error = -EINVAL;
	if (!S_ISREG(inode->i_mode))
		goto dput_and_out;

	error = mnt_want_write(path.mnt);
	if (error)
		goto dput_and_out;

	error = inode_permission2(mnt, inode, MAY_WRITE);
	if (error)
		goto mnt_drop_write_and_out;

	error = -EPERM;
	if (IS_APPEND(inode))
		goto mnt_drop_write_and_out;

	error = get_write_access(inode);
	if (error)
		goto mnt_drop_write_and_out;

	/*
	 * Make sure that there are no leases.  get_write_access() protects
	 * against the truncate racing with a lease-granting setlease().
	 */
	error = break_lease(inode, O_WRONLY);
	if (error)
		goto put_write_and_out;

	error = locks_verify_truncate(inode, NULL, length);
	if (!error)
		error = security_path_truncate(&path);
	if (!error)
		error = do_truncate2(mnt, path.dentry, length, 0, NULL);

put_write_and_out:
	put_write_access(inode);
mnt_drop_write_and_out:
	mnt_drop_write(path.mnt);
dput_and_out:
	path_put(&path);
out:
	return error;
}

SYSCALL_DEFINE2(truncate, const char __user *, path, long, length)
{
	return do_sys_truncate(path, length);
}

static long do_sys_ftruncate(unsigned int fd, loff_t length, int small)
{
	struct inode * inode;
	struct dentry *dentry;
	struct vfsmount *mnt;
	struct file * file;
	int error;

	error = -EINVAL;
	if (length < 0)
		goto out;
	error = -EBADF;
	file = fget(fd);
	if (!file)
		goto out;

	/* explicitly opened as large or we are on 64-bit box */
	if (file->f_flags & O_LARGEFILE)
		small = 0;

	dentry = file->f_path.dentry;
	mnt = file->f_path.mnt;
	inode = dentry->d_inode;
	error = -EINVAL;
	if (!S_ISREG(inode->i_mode) || !(file->f_mode & FMODE_WRITE))
		goto out_putf;

	error = -EINVAL;
	/* Cannot ftruncate over 2^31 bytes without large file support */
	if (small && length > MAX_NON_LFS)
		goto out_putf;

	error = -EPERM;
	if (IS_APPEND(inode))
		goto out_putf;

	error = locks_verify_truncate(inode, file, length);
	if (!error)
		error = security_path_truncate(&file->f_path);
	if (!error)
		error = do_truncate2(mnt, dentry, length, ATTR_MTIME|ATTR_CTIME, file);
out_putf:
	fput(file);
out:
	return error;
}

SYSCALL_DEFINE2(ftruncate, unsigned int, fd, unsigned long, length)
{
	long ret = do_sys_ftruncate(fd, length, 1);
	/* avoid REGPARM breakage on x86: */
	asmlinkage_protect(2, ret, fd, length);
	return ret;
}

/* LFS versions of truncate are only needed on 32 bit machines */
#if BITS_PER_LONG == 32
SYSCALL_DEFINE(truncate64)(const char __user * path, loff_t length)
{
	return do_sys_truncate(path, length);
}
#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
asmlinkage long SyS_truncate64(long path, loff_t length)
{
	return SYSC_truncate64((const char __user *) path, length);
}
SYSCALL_ALIAS(sys_truncate64, SyS_truncate64);
#endif

SYSCALL_DEFINE(ftruncate64)(unsigned int fd, loff_t length)
{
	long ret = do_sys_ftruncate(fd, length, 0);
	/* avoid REGPARM breakage on x86: */
	asmlinkage_protect(2, ret, fd, length);
	return ret;
}
#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
asmlinkage long SyS_ftruncate64(long fd, loff_t length)
{
	return SYSC_ftruncate64((unsigned int) fd, length);
}
SYSCALL_ALIAS(sys_ftruncate64, SyS_ftruncate64);
#endif
#endif /* BITS_PER_LONG == 32 */


int do_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	struct inode *inode = file->f_path.dentry->d_inode;
	long ret;

	if (offset < 0 || len <= 0)
		return -EINVAL;

	/* Return error if mode is not supported */
	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE))
		return -EOPNOTSUPP;

	/* Punch hole must have keep size set */
	if ((mode & FALLOC_FL_PUNCH_HOLE) &&
	    !(mode & FALLOC_FL_KEEP_SIZE))
		return -EOPNOTSUPP;

	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;

	/* It's not possible punch hole on append only file */
	if (mode & FALLOC_FL_PUNCH_HOLE && IS_APPEND(inode))
		return -EPERM;

	if (IS_IMMUTABLE(inode))
		return -EPERM;

	/*
	 * Revalidate the write permissions, in case security policy has
	 * changed since the files were opened.
	 */
	ret = security_file_permission(file, MAY_WRITE);
	if (ret)
		return ret;

	if (S_ISFIFO(inode->i_mode))
		return -ESPIPE;

	/*
	 * Let individual file system decide if it supports preallocation
	 * for directories or not.
	 */
	if (!S_ISREG(inode->i_mode) && !S_ISDIR(inode->i_mode))
		return -ENODEV;

	/* Check for wrap through zero too */
	if (((offset + len) > inode->i_sb->s_maxbytes) || ((offset + len) < 0))
		return -EFBIG;

	if (!file->f_op->fallocate)
		return -EOPNOTSUPP;

	return file->f_op->fallocate(file, mode, offset, len);
}

SYSCALL_DEFINE(fallocate)(int fd, int mode, loff_t offset, loff_t len)
{
	struct file *file;
	int error = -EBADF;

	file = fget(fd);
	if (file) {
		error = do_fallocate(file, mode, offset, len);
		fput(file);
	}

	return error;
}

#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
asmlinkage long SyS_fallocate(long fd, long mode, loff_t offset, loff_t len)
{
	return SYSC_fallocate((int)fd, (int)mode, offset, len);
}
SYSCALL_ALIAS(sys_fallocate, SyS_fallocate);
#endif

/*
 * access() needs to use the real uid/gid, not the effective uid/gid.
 * We do this by temporarily clearing all FS-related capabilities and
 * switching the fsuid/fsgid around to the real ones.
 */
SYSCALL_DEFINE3(faccessat, int, dfd, const char __user *, filename, int, mode)
{
	const struct cred *old_cred;
	struct cred *override_cred;
	struct path path;
	struct inode *inode;
	struct vfsmount *mnt;
	int res;

	if (mode & ~S_IRWXO)	/* where's F_OK, X_OK, W_OK, R_OK? */
		return -EINVAL;

	override_cred = prepare_creds();
	if (!override_cred)
		return -ENOMEM;

	override_cred->fsuid = override_cred->uid;
	override_cred->fsgid = override_cred->gid;

	if (!issecure(SECURE_NO_SETUID_FIXUP)) {
		/* Clear the capabilities if we switch to a non-root user */
		if (override_cred->uid)
			cap_clear(override_cred->cap_effective);
		else
			override_cred->cap_effective =
				override_cred->cap_permitted;
	}

	old_cred = override_creds(override_cred);

	res = user_path_at(dfd, filename, LOOKUP_FOLLOW, &path);
	if (res)
		goto out;

	inode = path.dentry->d_inode;
	mnt = path.mnt;

	if ((mode & MAY_EXEC) && S_ISREG(inode->i_mode)) {
		/*
		 * MAY_EXEC on regular files is denied if the fs is mounted
		 * with the "noexec" flag.
		 */
		res = -EACCES;
		if (path.mnt->mnt_flags & MNT_NOEXEC)
			goto out_path_release;
	}

	res = inode_permission2(mnt, inode, mode | MAY_ACCESS);
	/* SuS v2 requires we report a read only fs too */
	if (res || !(mode & S_IWOTH) || special_file(inode->i_mode))
		goto out_path_release;
	/*
	 * This is a rare case where using __mnt_is_readonly()
	 * is OK without a mnt_want/drop_write() pair.  Since
	 * no actual write to the fs is performed here, we do
	 * not need to telegraph to that to anyone.
	 *
	 * By doing this, we accept that this access is
	 * inherently racy and know that the fs may change
	 * state before we even see this result.
	 */
	if (__mnt_is_readonly(path.mnt))
		res = -EROFS;

out_path_release:
	path_put(&path);
out:
	revert_creds(old_cred);
	put_cred(override_cred);
	return res;
}

SYSCALL_DEFINE2(access, const char __user *, filename, int, mode)
{
	return sys_faccessat(AT_FDCWD, filename, mode);
}

SYSCALL_DEFINE1(chdir, const char __user *, filename)
{
	struct path path;
	int error;

	error = user_path_dir(filename, &path);
	if (error)
		goto out;

	error = inode_permission2(path.mnt, path.dentry->d_inode, MAY_EXEC | MAY_CHDIR);
	if (error)
		goto dput_and_out;

	set_fs_pwd(current->fs, &path);

dput_and_out:
	path_put(&path);
out:
	return error;
}

SYSCALL_DEFINE1(fchdir, unsigned int, fd)
{
	struct file *file;
	struct inode *inode;
	struct vfsmount *mnt;
	int error, fput_needed;

	error = -EBADF;
	file = fget_raw_light(fd, &fput_needed);
	if (!file)
		goto out;

	inode = file->f_path.dentry->d_inode;
	mnt = file->f_path.mnt;

	error = -ENOTDIR;
	if (!S_ISDIR(inode->i_mode))
		goto out_putf;

	error = inode_permission2(mnt, inode, MAY_EXEC | MAY_CHDIR);
	if (!error)
		set_fs_pwd(current->fs, &file->f_path);
out_putf:
	fput_light(file, fput_needed);
out:
	return error;
}

SYSCALL_DEFINE1(chroot, const char __user *, filename)
{
	struct path path;
	int error;

	error = user_path_dir(filename, &path);
	if (error)
		goto out;

	error = inode_permission2(path.mnt, path.dentry->d_inode, MAY_EXEC | MAY_CHDIR);
	if (error)
		goto dput_and_out;

	error = -EPERM;
	if (!capable(CAP_SYS_CHROOT))
		goto dput_and_out;
	error = security_path_chroot(&path);
	if (error)
		goto dput_and_out;

	set_fs_root(current->fs, &path);
	error = 0;
dput_and_out:
	path_put(&path);
out:
	return error;
}

static int chmod_common(struct path *path, umode_t mode)
{
	struct inode *inode = path->dentry->d_inode;
	struct iattr newattrs;
	int error;

	error = mnt_want_write(path->mnt);
	if (error)
		return error;
	mutex_lock(&inode->i_mutex);
	error = security_path_chmod(path, mode);
	if (error)
		goto out_unlock;
	newattrs.ia_mode = (mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO);
	newattrs.ia_valid = ATTR_MODE | ATTR_CTIME;
	error = notify_change2(path->mnt, path->dentry, &newattrs);
out_unlock:
	mutex_unlock(&inode->i_mutex);
	mnt_drop_write(path->mnt);
	return error;
}

SYSCALL_DEFINE2(fchmod, unsigned int, fd, umode_t, mode)
{
	struct file * file;
	int err = -EBADF;

	file = fget(fd);
	if (file) {
		audit_inode(NULL, file->f_path.dentry);
		err = chmod_common(&file->f_path, mode);
		fput(file);
	}
	return err;
}

SYSCALL_DEFINE3(fchmodat, int, dfd, const char __user *, filename, umode_t, mode)
{
	struct path path;
	int error;

	error = user_path_at(dfd, filename, LOOKUP_FOLLOW, &path);
	if (!error) {
		error = chmod_common(&path, mode);
		path_put(&path);
	}
	return error;
}

SYSCALL_DEFINE2(chmod, const char __user *, filename, umode_t, mode)
{
	return sys_fchmodat(AT_FDCWD, filename, mode);
}

static int chown_common(struct path *path, uid_t user, gid_t group)
{
	struct inode *inode = path->dentry->d_inode;
	int error;
	struct iattr newattrs;
	kuid_t uid;
	kgid_t gid;

	uid = make_kuid(current_user_ns(), user);
	gid = make_kgid(current_user_ns(), group);

	newattrs.ia_valid =  ATTR_CTIME;
	if (user != (uid_t) -1) {
		if (!uid_valid(uid))
			return -EINVAL;
		newattrs.ia_valid |= ATTR_UID;
		newattrs.ia_uid = uid;
	}
	if (group != (gid_t) -1) {
		if (!gid_valid(gid))
			return -EINVAL;
		newattrs.ia_valid |= ATTR_GID;
		newattrs.ia_gid = gid;
	}
	if (!S_ISDIR(inode->i_mode))
		newattrs.ia_valid |=
			ATTR_KILL_SUID | ATTR_KILL_SGID | ATTR_KILL_PRIV;
	mutex_lock(&inode->i_mutex);
	error = security_path_chown(path, user, group);
	if (!error)
		error = notify_change2(path->mnt, path->dentry, &newattrs);
	mutex_unlock(&inode->i_mutex);

	return error;
}

SYSCALL_DEFINE5(fchownat, int, dfd, const char __user *, filename, uid_t, user,
		gid_t, group, int, flag)
{
	struct path path;
	int error = -EINVAL;
	int lookup_flags;

	if ((flag & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) != 0)
		goto out;

	lookup_flags = (flag & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW;
	if (flag & AT_EMPTY_PATH)
		lookup_flags |= LOOKUP_EMPTY;
	error = user_path_at(dfd, filename, lookup_flags, &path);
	if (error)
		goto out;
	error = mnt_want_write(path.mnt);
	if (error)
		goto out_release;
	error = chown_common(&path, user, group);
	mnt_drop_write(path.mnt);
out_release:
	path_put(&path);
out:
	return error;
}

SYSCALL_DEFINE3(chown, const char __user *, filename, uid_t, user, gid_t, group)
{
	return sys_fchownat(AT_FDCWD, filename, user, group, 0);
}

SYSCALL_DEFINE3(lchown, const char __user *, filename, uid_t, user, gid_t, group)
{
	return sys_fchownat(AT_FDCWD, filename, user, group,
			    AT_SYMLINK_NOFOLLOW);
}

SYSCALL_DEFINE3(fchown, unsigned int, fd, uid_t, user, gid_t, group)
{
	struct file * file;
	int error = -EBADF;
	struct dentry * dentry;

	file = fget(fd);
	if (!file)
		goto out;

	error = mnt_want_write_file(file);
	if (error)
		goto out_fput;
	dentry = file->f_path.dentry;
	audit_inode(NULL, dentry);
	error = chown_common(&file->f_path, user, group);
	mnt_drop_write_file(file);
out_fput:
	fput(file);
out:
	return error;
}

/*
 * You have to be very careful that these write
 * counts get cleaned up in error cases and
 * upon __fput().  This should probably never
 * be called outside of __dentry_open().
 */
static inline int __get_file_write_access(struct inode *inode,
					  struct vfsmount *mnt)
{
	int error;
	error = get_write_access(inode);
	if (error)
		return error;
	/*
	 * Do not take mount writer counts on
	 * special files since no writes to
	 * the mount itself will occur.
	 */
	if (!special_file(inode->i_mode)) {
		/*
		 * Balanced in __fput()
		 */
		error = mnt_want_write(mnt);
		if (error)
			put_write_access(inode);
	}
	return error;
}

int open_check_o_direct(struct file *f)
{
	/* NB: we're sure to have correct a_ops only after f_op->open */
	if (f->f_flags & O_DIRECT) {
		if (!f->f_mapping->a_ops ||
		    ((!f->f_mapping->a_ops->direct_IO) &&
		    (!f->f_mapping->a_ops->get_xip_mem))) {
			return -EINVAL;
		}
	}
	return 0;
}

static struct file *do_dentry_open(struct dentry *dentry, struct vfsmount *mnt,
				   struct file *f,
				   int (*open)(struct inode *, struct file *),
				   const struct cred *cred)
{
	static const struct file_operations empty_fops = {};
	struct inode *inode;
	int error;

	f->f_mode = OPEN_FMODE(f->f_flags) | FMODE_LSEEK |
				FMODE_PREAD | FMODE_PWRITE;

	if (unlikely(f->f_flags & O_PATH))
		f->f_mode = FMODE_PATH;

	inode = dentry->d_inode;
	if (f->f_mode & FMODE_WRITE) {
		error = __get_file_write_access(inode, mnt);
		if (error)
			goto cleanup_file;
		if (!special_file(inode->i_mode))
			file_take_write(f);
	}

	f->f_mapping = inode->i_mapping;
	f->f_path.dentry = dentry;
	f->f_path.mnt = mnt;
	f->f_pos = 0;

	if (unlikely(f->f_mode & FMODE_PATH)) {
		f->f_op = &empty_fops;
		return f;
	}

	f->f_op = fops_get(inode->i_fop);

	error = security_file_open(f, cred);
	if (error)
		goto cleanup_all;

	error = break_lease(inode, f->f_flags);
	if (error)
		goto cleanup_all;

	if (!open && f->f_op)
		open = f->f_op->open;
	if (open) {
		error = open(inode, f);
		if (error)
			goto cleanup_all;
	}
	if ((f->f_mode & (FMODE_READ | FMODE_WRITE)) == FMODE_READ)
		i_readcount_inc(inode);

	f->f_flags &= ~(O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC);

	file_ra_state_init(&f->f_ra, f->f_mapping->host->i_mapping);

	return f;

cleanup_all:
	fops_put(f->f_op);
	if (f->f_mode & FMODE_WRITE) {
		put_write_access(inode);
		if (!special_file(inode->i_mode)) {
			/*
			 * We don't consider this a real
			 * mnt_want/drop_write() pair
			 * because it all happenend right
			 * here, so just reset the state.
			 */
			file_reset_write(f);
			mnt_drop_write(mnt);
		}
	}
	f->f_path.dentry = NULL;
	f->f_path.mnt = NULL;
cleanup_file:
	dput(dentry);
	mntput(mnt);
	return ERR_PTR(error);
}

static struct file *__dentry_open(struct dentry *dentry, struct vfsmount *mnt,
				struct file *f,
				int (*open)(struct inode *, struct file *),
				const struct cred *cred)
{
	struct file *res = do_dentry_open(dentry, mnt, f, open, cred);
	if (!IS_ERR(res)) {
		int error = open_check_o_direct(f);
		if (error) {
			fput(res);
			res = ERR_PTR(error);
		}
	} else {
		put_filp(f);
	}
	return res;
}

/**
 * finish_open - finish opening a file
 * @od: opaque open data
 * @dentry: pointer to dentry
 * @open: open callback
 *
 * This can be used to finish opening a file passed to i_op->atomic_open().
 *
 * If the open callback is set to NULL, then the standard f_op->open()
 * filesystem callback is substituted.
 */
int finish_open(struct file *file, struct dentry *dentry,
		int (*open)(struct inode *, struct file *),
		int *opened)
{
	struct file *res;
	BUG_ON(*opened & FILE_OPENED); /* once it's opened, it's opened */

	mntget(file->f_path.mnt);
	dget(dentry);

	res = do_dentry_open(dentry, file->f_path.mnt, file, open, current_cred());
	if (!IS_ERR(res)) {
		*opened |= FILE_OPENED;
		return 0;
	}

	return PTR_ERR(res);
}
EXPORT_SYMBOL(finish_open);

/**
 * finish_no_open - finish ->atomic_open() without opening the file
 *
 * @od: opaque open data
 * @dentry: dentry or NULL (as returned from ->lookup())
 *
 * This can be used to set the result of a successful lookup in ->atomic_open().
 * The filesystem's atomic_open() method shall return NULL after calling this.
 */
int finish_no_open(struct file *file, struct dentry *dentry)
{
	file->f_path.dentry = dentry;
	return 1;
}
EXPORT_SYMBOL(finish_no_open);

/*
 * dentry_open() will have done dput(dentry) and mntput(mnt) if it returns an
 * error.
 */
struct file *dentry_open(struct dentry *dentry, struct vfsmount *mnt, int flags,
			 const struct cred *cred)
{
	int error;
	struct file *f;

	validate_creds(cred);

	/* We must always pass in a valid mount pointer. */
	BUG_ON(!mnt);

	error = -ENFILE;
	f = get_empty_filp();
	if (f == NULL) {
		dput(dentry);
		mntput(mnt);
		return ERR_PTR(error);
	}

	f->f_flags = flags;
	return __dentry_open(dentry, mnt, f, NULL, cred);
}
EXPORT_SYMBOL(dentry_open);

static inline int build_open_flags(int flags, umode_t mode, struct open_flags *op)
{
	int lookup_flags = 0;
	int acc_mode;

	if (flags & O_CREAT)
		op->mode = (mode & S_IALLUGO) | S_IFREG;
	else
		op->mode = 0;

	/* Must never be set by userspace */
	flags &= ~FMODE_NONOTIFY;

	/*
	 * O_SYNC is implemented as __O_SYNC|O_DSYNC.  As many places only
	 * check for O_DSYNC if the need any syncing at all we enforce it's
	 * always set instead of having to deal with possibly weird behaviour
	 * for malicious applications setting only __O_SYNC.
	 */
	if (flags & __O_SYNC)
		flags |= O_DSYNC;

	if (flags & O_TMPFILE) {
		if (!(flags & O_CREAT))
			return -EINVAL;
		acc_mode = MAY_OPEN | ACC_MODE(flags);
	} else if (flags & O_PATH) {
		/*
		 * If we have O_PATH in the open flag. Then we
		 * cannot have anything other than the below set of flags
		 */
		flags &= O_DIRECTORY | O_NOFOLLOW | O_PATH;
		acc_mode = 0;
	} else {
		acc_mode = MAY_OPEN | ACC_MODE(flags);
	}

	op->open_flag = flags;

	/* O_TRUNC implies we need access checks for write permissions */
	if (flags & O_TRUNC)
		acc_mode |= MAY_WRITE;

	/* Allow the LSM permission hook to distinguish append
	   access from general write access. */
	if (flags & O_APPEND)
		acc_mode |= MAY_APPEND;

	op->acc_mode = acc_mode;

	op->intent = flags & O_PATH ? 0 : LOOKUP_OPEN;

	if (flags & O_CREAT) {
		op->intent |= LOOKUP_CREATE;
		if (flags & O_EXCL)
			op->intent |= LOOKUP_EXCL;
	}

	if (flags & O_DIRECTORY)
		lookup_flags |= LOOKUP_DIRECTORY;
	if (!(flags & O_NOFOLLOW))
		lookup_flags |= LOOKUP_FOLLOW;
	return lookup_flags;
}

/**
 * file_open_name - open file and return file pointer
 *
 * @name:	struct filename containing path to open
 * @flags:	open flags as per the open(2) second argument
 * @mode:	mode for the new file if O_CREAT is set, else ignored
 *
 * This is the helper to open a file from kernelspace if you really
 * have to.  But in generally you should not do this, so please move
 * along, nothing to see here..
 */
struct file *file_open_name(struct filename *name, int flags, umode_t mode)
{
	struct open_flags op;
	int lookup = build_open_flags(flags, mode, &op);
	return do_filp_open(AT_FDCWD, name, &op, lookup);
}

/**
 * filp_open - open file and return file pointer
 *
 * @filename:	path to open
 * @flags:	open flags as per the open(2) second argument
 * @mode:	mode for the new file if O_CREAT is set, else ignored
 *
 * This is the helper to open a file from kernelspace if you really
 * have to.  But in generally you should not do this, so please move
 * along, nothing to see here..
 */
struct file *filp_open(const char *filename, int flags, umode_t mode)
{
	struct filename name = {.name = filename};
	return file_open_name(&name, flags, mode);
}
EXPORT_SYMBOL(filp_open);

struct file *file_open_root(struct dentry *dentry, struct vfsmount *mnt,
			    const char *filename, int flags)
{
	struct open_flags op;
	int lookup = build_open_flags(flags, 0, &op);
	if (flags & O_CREAT)
		return ERR_PTR(-EINVAL);
	if (!filename && (flags & O_DIRECTORY))
		if (!dentry->d_inode->i_op->lookup)
			return ERR_PTR(-ENOTDIR);
	return do_file_open_root(dentry, mnt, filename, &op, lookup);
}
EXPORT_SYMBOL(file_open_root);

long do_sys_open(int dfd, const char __user *filename, int flags, umode_t mode)
{
	struct open_flags op;
	int lookup = build_open_flags(flags, mode, &op);
	struct filename *tmp = getname(filename);
	int fd = PTR_ERR(tmp);

	if (!IS_ERR(tmp)) {
		fd = get_unused_fd_flags(flags);
		if (fd >= 0) {
			struct file *f = do_filp_open(dfd, tmp, &op, lookup);
			if (IS_ERR(f)) {
				put_unused_fd(fd);
				fd = PTR_ERR(f);
			} else {
				fsnotify_open(f);
				fd_install(fd, f);
			}
		}
		putname(tmp);
	}
	return fd;
}

SYSCALL_DEFINE3(open, const char __user *, filename, int, flags, umode_t, mode)
{
	long ret;

	if (force_o_largefile())
		flags |= O_LARGEFILE;

	ret = do_sys_open(AT_FDCWD, filename, flags, mode);
	/* avoid REGPARM breakage on x86: */
	asmlinkage_protect(3, ret, filename, flags, mode);
	return ret;
}

SYSCALL_DEFINE4(openat, int, dfd, const char __user *, filename, int, flags,
		umode_t, mode)
{
	long ret;

	if (force_o_largefile())
		flags |= O_LARGEFILE;

	ret = do_sys_open(dfd, filename, flags, mode);
	/* avoid REGPARM breakage on x86: */
	asmlinkage_protect(4, ret, dfd, filename, flags, mode);
	return ret;
}

#ifndef __alpha__

/*
 * For backward compatibility?  Maybe this should be moved
 * into arch/i386 instead?
 */
SYSCALL_DEFINE2(creat, const char __user *, pathname, umode_t, mode)
{
	return sys_open(pathname, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

#endif

/*
 * "id" is the POSIX thread ID. We use the
 * files pointer for this..
 */
int filp_close(struct file *filp, fl_owner_t id)
{
	int retval = 0;

	if (!file_count(filp)) {
		printk(KERN_ERR "VFS: Close: file count is 0\n");
		return 0;
	}

	if (filp->f_op && filp->f_op->flush)
		retval = filp->f_op->flush(filp, id);

	if (likely(!(filp->f_mode & FMODE_PATH))) {
		dnotify_flush(filp, id);
		locks_remove_posix(filp, id);
	}
	security_file_close(filp);
	fput(filp);
	return retval;
}

EXPORT_SYMBOL(filp_close);

/*
 * Careful here! We test whether the file pointer is NULL before
 * releasing the fd. This ensures that one clone task can't release
 * an fd while another clone is opening it.
 */
SYSCALL_DEFINE1(close, unsigned int, fd)
{
	int retval = __close_fd(current->files, fd);

	/* can't restart close syscall because file table entry was cleared */
	if (unlikely(retval == -ERESTARTSYS ||
		     retval == -ERESTARTNOINTR ||
		     retval == -ERESTARTNOHAND ||
		     retval == -ERESTART_RESTARTBLOCK))
		retval = -EINTR;

	return retval;
}
EXPORT_SYMBOL(sys_close);

/*
 * This routine simulates a hangup on the tty, to arrange that users
 * are given clean terminals at login time.
 */
SYSCALL_DEFINE0(vhangup)
{
	if (capable(CAP_SYS_TTY_CONFIG)) {
		tty_vhangup_self();
		return 0;
	}
	return -EPERM;
}

/*
 * Called when an inode is about to be open.
 * We use this to disallow opening large files on 32bit systems if
 * the caller didn't specify O_LARGEFILE.  On 64bit systems we force
 * on this flag in sys_open.
 */
int generic_file_open(struct inode * inode, struct file * filp)
{
	if (!(filp->f_flags & O_LARGEFILE) && i_size_read(inode) > MAX_NON_LFS)
		return -EOVERFLOW;
	return 0;
}

EXPORT_SYMBOL(generic_file_open);

/*
 * This is used by subsystems that don't want seekable
 * file descriptors. The function is not supposed to ever fail, the only
 * reason it returns an 'int' and not 'void' is so that it can be plugged
 * directly into file_operations structure.
 */
int nonseekable_open(struct inode *inode, struct file *filp)
{
	filp->f_mode &= ~(FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE);
	return 0;
}

EXPORT_SYMBOL(nonseekable_open);
