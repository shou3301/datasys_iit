/**
 * fusionfs.c
 *
 * Author: dongfang@ieee.org
 *
 * Update history:
 *  07/15/2012:
 *  	- _lookup() service interface changed again!!! This is only a tentative solution by
 *  		directly calling c_zht_lookup2(). It will be replaced by zht_lookup() once
 *  		Xiaobing fixed the char** argument.
 * 	07/14/2012:
 * 		- zht_lookup() interface changed
 * 	07/08/2012:
 * 		- bug fixed - char[PATH_MAX] initilized to 0
 * 		- basic directory create/remove working fine
 *  07/05/2012: hsearch replaced by ZHT
 * 	06/27/2012: read with UDT
 * 	06/01/2012: read/write with LFTP
 * 	05/22/2012: read/write with SCP
 *
 * To compile for a single file:
 * 		gcc -g -Wall `pkg-config fuse --cflags` -c fusionfs.c -L./udt4_c/ffsnet -lffsnet_bridger
 */
 
#include "params.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <stdbool.h>

#include "log.h"
#include "./zht/inc/c_zhtclient.h"

int ffs_recvfile_c(const char *proto, const char *remote_ip, const char *server_port, const char *remote_filename, const char *local_filename);
int ffs_sendfile_c(const char *proto, const char *remote_ip, const char *server_port, const char *local_filename, const char *remote_filename);

/**
 * Update <key, oldval> with <key, val>
 */
int zht_update(const char *key, const char *val)
{
//	/*DFZ debug*/
//	char oldval[PATH_MAX] = {0};
//	int status = zht_lookup(key, oldval);
//	log_msg("DFZ debug: zht_update() - status = %d. \n\n", status);
//	if (ZHT_LOOKUP_FAIL == status)
//		log_msg("DFZ debug: zht_update() - key %s not found. \n\n", key);
//	else
//		log_msg("DFZ debug: zht_update() - key = %s, oldval = %s. \n\n", key, oldval);

	int remove_res = zht_remove(key);
	log_msg("DFZ debug: zht_update() - remove_res = %d. \n\n", remove_res);

//	/*DFZ debug*/
//	status = zht_lookup(key, oldval);
//	if (ZHT_LOOKUP_FAIL == status)
//		log_msg("DFZ debug: zht_update() - key %s not found. \n\n", key);
//	else
//		log_msg("DFZ debug: zht_update() - key = %s, oldval = %s. \n\n", key, oldval);
//
//	log_msg("DFZ debug: zht_update() - key = %s, val =  %s. \n\n", key, val);

	int insert_res = zht_insert(key, val);
	log_msg("DFZ debug: zht_update() - insert_res = %d. \n\n", insert_res);

	/*DFZ debug*/
//	char newval[PATH_MAX] = {0};
//	int status = zht_lookup(key, newval);
	char *newval = NULL;
	int status = c_zht_lookup2(key, &newval);

	log_msg("DFZ debug: zht_update() - status = %d. \n\n", status);

	if (ZHT_LOOKUP_FAIL == status)
		log_msg("DFZ debug: zht_update() - key %s not found. \n\n", key);
	else
		log_msg("DFZ debug: zht_update() - key = %s, newval = %s. \n\n", key, newval);

	return 0;
}

/**
 * <key, oldval> -> <key, (oldval + val)>
 */
int zht_append(const char *key, const char *val)
{
	char newval[PATH_MAX] = {0};
//	char oldval[PATH_MAX] = {0};
//	zht_lookup(key, oldval);

	char *oldval = NULL;
	c_zht_lookup2(key, &oldval);

	strcpy(newval, oldval);
	strcat(newval, val);
	strcat(newval, " ");

//	/*DFZ debug*/
//	log_msg("DFZ debug: zht_append() - key = %s, newval = %s. \n\n", key, newval);

	zht_update(key, newval);

	return 0;
}

/**
 * <key, oldval> -> <key, (oldval - val)>
 */
int zht_delete(const char *key, const char *val)
{
	char newval[PATH_MAX] = {0};
	char search[PATH_MAX] = {0};

//	char oldval[PATH_MAX] = {0};
//	zht_lookup(key, oldval);
	char *oldval = NULL;
	c_zht_lookup2(key, &oldval);

	strcpy(search, " ");
	strcat(search, val);
	strcat(search, " ");

	char *pch = strstr(oldval, search);
	strncpy(newval, oldval, pch - oldval);
	strcat(newval, " ");
	strcat(newval, pch + strlen(search));

	zht_update(key, newval);

	return 0;
}

/**
 * Report errors to logfile and give -errno to caller
 *
 */
static int fusion_error(char *str)
{
	int ret = -errno;

	log_msg("    ERROR %s: %s\n", str, strerror(errno));

	return ret;
}

// Check whether the given user is permitted to perform the given operation on the given 

//  All the paths I see are relative to the root of the mounted
//  filesystem.  In order to get to the underlying filesystem, I need to
//  have the mountpoint.  I'll save it away early on in main(), and then
//  whenever I need a path for something I'll call this to construct
//  it.
static void fusion_fullpath(char fpath[PATH_MAX], const char *path)
{
	strcpy(fpath, FUSION_DATA->rootdir);
	strncat(fpath, path, PATH_MAX); // ridiculously long paths will
	// break here

	log_msg(
			"    fusion_fullpath:  rootdir = \"%s\", path = \"%s\", fpath = \"%s\"\n",
			FUSION_DATA->rootdir, path, fpath);
}


static void fusion_vpath(char vpath[PATH_MAX], const char *path)
{
	strcpy(vpath, "/home/dongfang/fusionFS/src/udt4_c/ffsnet/ffsroot");
	strncat(vpath, path, PATH_MAX); // ridiculously long paths will
	// break here

	log_msg(
			"    fusion_vpath:  path = \"%s\", vpath = \"%s\"\n",
			path, vpath);
}

///**
// * Given a virtual path <path>, return the local copy <lpath> on physical FS
// */
//static void fusion_lpath(char lpath[PATH_MAX], const char *path)
//{
//	strcpy(lpath, FUSION_DATA->rootdir);
//	strncat(lpath, "/tmp", PATH_MAX);
//}

///////////////////////////////////////////////////////////
//
// Prototypes for all these functions, and the C-style comments,
// come indirectly from /usr/include/fuse.h
//
/** Get file attributes.
 *
 * Similar to stat().  The 'st_dev' and 'st_blksize' fields are
 * ignored.  The 'st_ino' field is ignored except if the 'use_ino'
 * mount option is given.
 *
 * DFZ: This is the first function to be called whenever the user tries to
 * 		get access to any file, even if only to its meta data. Two cases:
 * 			1) if the file exists, i.e. it's stored in ZHT, then this file is
 * 				transfered to the local node first and 'lstat' the local copy
 * 			2) if the file doesn't exist, FUSE will pass the control to
 * 				_create()
 *
 *
 */
int fusion_getattr(const char *path, struct stat *statbuf)
{
	int retstat = 0;
	char fpath[PATH_MAX] = {0};

	log_msg("\nfusion_getattr(path=\"%s\", statbuf=0x%08x)\n", path, statbuf);
	fusion_fullpath(fpath, path);

	/* DFZ: construct the remote pathname */
//	const char *remote_node = "dongfang@fusion.cs.iit.edu:~/fusionFS";
//	char remote_pathname[PATH_MAX];
//	strcpy(remote_pathname, remote_node);
//	strcat(remote_pathname, path);
//	/* DFZ: construct the local temp file */
//	const char *local_tmp_dir = "/tmp";
//	char local_pathname[PATH_MAX];
//	strcpy(local_pathname, FUSION_DATA->rootdir);
//	strcat(local_pathname, local_tmp_dir);
//	strcat(local_pathname, path);

	/* DFZ: construct the command of remote copy */
//	char cmd_scp[PATH_MAX << 2];
//	strcpy(cmd_scp, "scp ");
//	strcat(cmd_scp, remote_pathname);
//	strcat(cmd_scp, " ");
//	strcat(cmd_scp, local_pathname);

//	/* DFZ: copy the file to local tmp directory via SCP */
//	 system(cmd_scp);
//
//	/* DFZ: construct the command to remove the local copy */
//	char cmd_rm[PATH_MAX];
//	strcpy(cmd_rm, "rm ");
//	strcat(cmd_rm, local_pathname);

//	system(cmd_rm);

//	/* DFZ: construct FTP cmd */
//	char cmd_ftp[PATH_MAX];
//	strcpy(cmd_ftp,
//			"lftp fusion.cs.iit.edu -u dongfang,zdf81IIT -e 'get fusionFS");
//	strcat(cmd_ftp, path);
//	strcat(cmd_ftp, " -o ");
//	strcat(cmd_ftp, local_pathname);
//	strcat(cmd_ftp, ";bye'");
//
//	/* DFZ: test udt4 transfer */
//	system(cmd_ftp);
//	char remote_pathname[PATH_MAX];
//	strcpy(remote_pathname, "ffsroot");
//	strcat(remote_pathname, path);
//	ffs_recvfile_c("udt", "fusion.cs.iit.edu", "9999", remote_pathname, local_pathname);

//	/* DFZ: debug info */
//	log_msg("\n =====DFZ debug: cmd_ftp = %s \n", cmd_ftp);
//
//	/* DFZ: lstat the local temp file */
//	retstat = lstat(local_pathname, statbuf);

	/* DFZ: try to download the file to local */
//	unlink(fpath); /* we need to remove the local cache before getting the new update */
//	char res[PATH_MAX] = {0};
//	int status = zht_lookup(path, res);
	char *res = NULL;
	int status = c_zht_lookup2(path, &res);

	if (ZHT_LOOKUP_FAIL == status) { /* if not found */
		log_msg("\n ===========DFZ debug: _getattr() %s does not exist \n\n", path);
	}
	else { /* if file exists */
		log_msg("\n ===========DFZ debug: _getattr() zht_lookup() = %s. \n\n", res);

		char vpath[PATH_MAX] = {0};
		fusion_vpath(vpath, path);
		/* DFZ TODO: the following localhost will be replaced when tested on multiple nodes based on Tony's mod method
		 */
		/* DFZ TODO: since there are no physical directories in current design, fpath will need to be updated
		 * 		to only locate in the local root folder, rather than the fully fledged hierarchy.
		 *
		 * 		Or, maybe we can just keep the full hierarchy in the local node
		 * */
		ffs_recvfile_c("udt", "localhost", "9000", vpath, fpath);
	}

	retstat = lstat(fpath, statbuf);

	if (retstat != 0)
		retstat = fusion_error("fusion_getattr lstat");

	log_stat(statbuf);

	return retstat;
}

/** Read the target of a symbolic link
 *
 * The buffer should be filled with a null terminated string.  The
 * buffer size argument includes the space for the terminating
 * null character.  If the linkname is too long to fit in the
 * buffer, it should be truncated.  The return value should be 0
 * for success.
 */
// Note the system readlink() will truncate and lose the terminating
// null.  So, the size passed to to the system readlink() must be one
// less than the size passed to fusion_readlink()
// fusion_readlink() code by Bernardo F Costa (thanks!)
int fusion_readlink(const char *path, char *link, size_t size) {
	int retstat = 0;
	char fpath[PATH_MAX] = {0};

	log_msg("fusion_readlink(path=\"%s\", link=\"%s\", size=%d)\n", path, link,
			size);
	fusion_fullpath(fpath, path);

	retstat = readlink(fpath, link, size - 1);
	if (retstat < 0)
		retstat = fusion_error("fusion_readlink readlink");
	else {
		link[retstat] = '\0';
		retstat = 0;
	}

	return retstat;
}

/** Create a file node
 *
 * There is no create() operation, mknod() will be called for
 * creation of all non-directory, non-symlink nodes.
 */
// shouldn't that comment be "if" there is no.... ?
int fusion_mknod(const char *path, mode_t mode, dev_t dev) {
	int retstat = 0;
	char fpath[PATH_MAX] = {0};

	log_msg("\nfusion_mknod(path=\"%s\", mode=0%3o, dev=%lld)\n", path, mode, dev);
	fusion_fullpath(fpath, path);

	// On Linux this could just be 'mknod(path, mode, rdev)' but this
	//  is more portable
	if (S_ISREG(mode)) {
		retstat = open(fpath, O_CREAT | O_EXCL | O_WRONLY, mode);
		if (retstat < 0)
			retstat = fusion_error("fusion_mknod open");
		else {
			retstat = close(retstat);
			if (retstat < 0)
				retstat = fusion_error("fusion_mknod close");
		}
	} else if (S_ISFIFO(mode)) {
		retstat = mkfifo(fpath, mode);
		if (retstat < 0)
			retstat = fusion_error("fusion_mknod mkfifo");
	} else {
		retstat = mknod(fpath, mode, dev);
		if (retstat < 0)
			retstat = fusion_error("fusion_mknod mknod");
	}

	return retstat;
}

/**
 * Create a directory
 *
 * 		DFZ: updated for ZHT
 */
int fusion_mkdir(const char *path, mode_t mode)
{
	int retstat = 0;
	char fpath[PATH_MAX] = {0};

	log_msg("\nfusion_mkdir(path=\"%s\", mode=0%3o)\n", path, mode);
	fusion_fullpath(fpath, path);

	retstat = mkdir(fpath, mode);
	if (retstat < 0)
		retstat = fusion_error("fusion_mkdir mkdir");


	/*DFZ: update ZHT with dir changes */
	char parentpath[PATH_MAX] = {0};
	char curpath[PATH_MAX] = {0};
	char fullpath[PATH_MAX] = {0};
	char *pch = strrchr(path, '/');
	strncpy(parentpath, path, pch - path + 1);
	strcpy(curpath, pch + 1);
	strcat(curpath, "/");
	strcpy(fullpath, path);
	strcat(fullpath, "/");
	log_msg("\n==========DFZ debug: fusion_mkdir() parentpath = %s, curpath = %s.\n\n", parentpath, curpath);

	zht_insert(fullpath, " "); //so this is succesful
	zht_append(parentpath, curpath); //and this failed.

//	/*DFZ debug*/
//	char res[PATH_MAX] = {0};
//	int status = zht_lookup(parentpath, res);
//	if (ZHT_LOOKUP_FAIL == status)
//		log_msg("\n==========DFZ debug: fusion_mkdir() %s not found.\n\n", parentpath);
//	else
//		log_msg("\n==========DFZ debug: fusion_mkdir() parentpath = %s, res = %s.\n\n", parentpath, res);
//
//	memset(res, 0, PATH_MAX);
//	status = zht_lookup(fullpath, res);
//	if (ZHT_LOOKUP_FAIL == status)
//		log_msg("\n==========DFZ debug: fusion_mkdir() %s not found.\n\n", fullpath);
//	else
//		log_msg("\n==========DFZ debug: fusion_mkdir() fullpath = %s, res = %s.\n\n", fullpath, res);

	return retstat;
}

/**
 * Remove a directory
 *
 * 		DFZ: updated for ZHT
 */
int fusion_rmdir(const char *path)
{
	int retstat = 0;
	char fpath[PATH_MAX] = {0};

	log_msg("fusion_rmdir(path=\"%s\")\n", path);
	fusion_fullpath(fpath, path);

	retstat = rmdir(fpath);
	if (retstat < 0)
		retstat = fusion_error("fusion_rmdir rmdir");

	/* DFZ: update ZHT */
	char parentpath[PATH_MAX] = {0};
	char curpath[PATH_MAX] = {0};
	char fullpath[PATH_MAX] = {0};
	char *pch = strrchr(path, '/');
	strncpy(parentpath, path, pch - path + 1);
	strcpy(curpath, pch + 1);
	strcat(curpath, "/");
	strcpy(fullpath, path);
	strcat(fullpath, "/");
	log_msg("\n==========DFZ debug: fusion_rmdir() parentpath = %s, curpath = %s \n\n", parentpath, curpath);

	zht_delete(parentpath, curpath);
	zht_remove(fullpath);

	return retstat;
}

/**
 * Remove a file
 *
 * DFZ TODO: two ways to unlink remote files:
 * 		1) add a "unlink request" service type in the ffsnetd daemon process
 * 		2) don't touch the remote file when updating the meta data, but do a batch job in each node to
 * 			self-check dangling files
 */
int fusion_unlink(const char *path)
{
	int retstat = 0;
	char fpath[PATH_MAX] = {0};

	log_msg("fusion_unlink(path=\"%s\")\n", path);
	fusion_fullpath(fpath, path);

	/*DFZ: remove the file from its parent dir in ZHT*/
	char dirname[PATH_MAX] = {0}, fname[PATH_MAX] = {0};
	char *pch = strrchr(path, '/');
	strncpy(dirname, path, pch - path + 1);
	strcpy(fname, pch + 1);
//	log_msg("\n========DFZ debug: fusion_unlink dirname = %s, fname = %s \n", dirname, fname);
	zht_delete(dirname, fname);

	/* DFZ: remove the file entry from ZHT */
	zht_remove(path);

	/* DFZ TODO: how can I unlink a remote file */
	retstat = unlink(fpath);
	if (retstat < 0)
		retstat = fusion_error("fusion_unlink unlink");

	return retstat;
}

/** Create a symbolic link */
// The parameters here are a little bit confusing, but do correspond
// to the symlink() system call.  The 'path' is where the link points,
// while the 'link' is the link itself.  So we need to leave the path
// unaltered, but insert the link into the mounted directory.
int fusion_symlink(const char *path, const char *link) {
	int retstat = 0;
	char flink[PATH_MAX] = {0};

	log_msg("\nfusion_symlink(path=\"%s\", link=\"%s\")\n", path, link);
	fusion_fullpath(flink, link);

	retstat = symlink(path, flink);
	if (retstat < 0)
		retstat = fusion_error("fusion_symlink symlink");

	return retstat;
}

/** Rename a file */
// both path and newpath are fs-relative
int fusion_rename(const char *path, const char *newpath) {
	int retstat = 0;
	char fpath[PATH_MAX] = {0};
	char fnewpath[PATH_MAX] = {0};

	log_msg("\nfusion_rename(fpath=\"%s\", newpath=\"%s\")\n", path, newpath);
	fusion_fullpath(fpath, path);
	fusion_fullpath(fnewpath, newpath);

	retstat = rename(fpath, fnewpath);
	if (retstat < 0)
		retstat = fusion_error("fusion_rename rename");

	return retstat;
}

/** Create a hard link to a file */
int fusion_link(const char *path, const char *newpath) {
	int retstat = 0;
	char fpath[PATH_MAX] = {0}, fnewpath[PATH_MAX] = {0};

	log_msg("\nfusion_link(path=\"%s\", newpath=\"%s\")\n", path, newpath);
	fusion_fullpath(fpath, path);
	fusion_fullpath(fnewpath, newpath);

	retstat = link(fpath, fnewpath);
	if (retstat < 0)
		retstat = fusion_error("fusion_link link");

	return retstat;
}

/** Change the permission bits of a file */
int fusion_chmod(const char *path, mode_t mode) {
	int retstat = 0;
	char fpath[PATH_MAX] = {0};

	log_msg("\nfusion_chmod(fpath=\"%s\", mode=0%03o)\n", path, mode);
	fusion_fullpath(fpath, path);

	retstat = chmod(fpath, mode);
	if (retstat < 0)
		retstat = fusion_error("fusion_chmod chmod");

	return retstat;
}

/** Change the owner and group of a file */
int fusion_chown(const char *path, uid_t uid, gid_t gid)

{
	int retstat = 0;
	char fpath[PATH_MAX] = {0};

	log_msg("\nfusion_chown(path=\"%s\", uid=%d, gid=%d)\n", path, uid, gid);
	fusion_fullpath(fpath, path);

	retstat = chown(fpath, uid, gid);
	if (retstat < 0)
		retstat = fusion_error("fusion_chown chown");

	return retstat;
}

/** Change the size of a file */
int fusion_truncate(const char *path, off_t newsize) {
	int retstat = 0;
	char fpath[PATH_MAX] = {0};

	log_msg("\nfusion_truncate(path=\"%s\", newsize=%lld)\n", path, newsize);
	fusion_fullpath(fpath, path);

	retstat = truncate(fpath, newsize);
	if (retstat < 0)
		fusion_error("fusion_truncate truncate");

	return retstat;
}

/** Change the access and/or modification times of a file */
/* note -- I'll want to change this as soon as 2.6 is in debian testing */
int fusion_utime(const char *path, struct utimbuf *ubuf) {
	int retstat = 0;
	char fpath[PATH_MAX] = {0};

	log_msg("\nfusion_utime(path=\"%s\", ubuf=0x%08x)\n", path, ubuf);
	fusion_fullpath(fpath, path);

	retstat = utime(fpath, ubuf);
	if (retstat < 0)
		retstat = fusion_error("fusion_utime utime");

	return retstat;
}

/** File open operation
 *
 * No creation, or truncation flags (O_CREAT, O_EXCL, O_TRUNC)
 * will be passed to open().  Open should check if the operation
 * is permitted for the given flags.  Optionally open may also
 * return an arbitrary filehandle in the fuse_file_info structure,
 * which will be passed to all file operations.
 *
 * Changed in version 2.2
 *
 * DFZ: we don't need special handling here in _open() because
 * 		1) if the file exists, then the _getattr() should already
 * 			transfer the file from <vpath> to <fpath>
 *		2) if the file doesn't exist, then _create() should be called
 *			rather than _open()
 */
int fusion_open(const char *path, struct fuse_file_info *fi)
{
	int retstat = 0;
	int fd;
	char fpath[PATH_MAX] = {0};

	log_msg("\nfusion_open(path\"%s\", fi=0x%08x)\n", path, fi);
	fusion_fullpath(fpath, path);

//	/* DFZ: construct the local temp file */
//	const char *local_tmp_dir = "/tmp";
//	char local_pathname[PATH_MAX];
//	strcpy(local_pathname, FUSION_DATA->rootdir);
//	strcat(local_pathname, local_tmp_dir);
//	strcat(local_pathname, path);
//
//	/* DFZ: open the local temp file */
//	fd = open(local_pathname, fi->flags);

	/* DFZ: download the file to local FS */
//	char vpath[PATH_MAX];
//	fusion_vpath(vpath, path);
//	//unlink(fpath);
//	ffs_recvfile_c("udt", "localhost", "9000", vpath, fpath);


	fd = open(fpath, fi->flags);
	if (fd < 0)
		retstat = fusion_error("fusion_open open");

	fi->fh = fd;
	log_fi(fi);

	return retstat;
}

/** Read data from an open file
 *
 * Read should return exactly the number of bytes requested except
 * on EOF or error, otherwise the rest of the data will be
 * substituted with zeroes.  An exception to this is when the
 * 'direct_io' mount option is specified, in which case the return
 * value of the read system call will reflect the return value of
 * this operation.
 *
 * Changed in version 2.2
 */
// I don't fully understand the documentation above -- it doesn't
// match the documentation for the read() system call which says it
// can return with anything up to the amount of data requested. nor
// with the fusexmp code which returns the amount of data also
// returned by read.
int fusion_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi) {
	int retstat = 0;

	log_msg(
			"\nfusion_read(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n",
			path, buf, size, offset, fi);
	// no need to get fpath on this one, since I work from fi->fh not the path
	log_fi(fi);

	retstat = pread(fi->fh, buf, size, offset);
	if (retstat < 0)
		retstat = fusion_error("fusion_read read");

	return retstat;
}

/** Write data to an open file
 *
 * Write should return exactly the number of bytes requested
 * except on error.  An exception to this is when the 'direct_io'
 * mount option is specified (see read operation).
 *
 * Changed in version 2.2
 */
// As  with read(), the documentation above is inconsistent with the
// documentation for the write() system call.
int fusion_write(const char *path, const char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi) {
	int retstat = 0;

	log_msg(
			"\nfusion_write(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n",
			path, buf, size, offset, fi);
	// no need to get fpath on this one, since I work from fi->fh not the path
	log_fi(fi);

	retstat = pwrite(fi->fh, buf, size, offset);
	if (retstat < 0)
		retstat = fusion_error("fusion_write pwrite");

	return retstat;
}

/** Get file system statistics
 *
 * The 'f_frsize', 'f_favail', 'f_fsid' and 'f_flag' fields are ignored
 *
 * Replaced 'struct statfs' parameter with 'struct statvfs' in
 * version 2.5
 */
int fusion_statfs(const char *path, struct statvfs *statv) {
	int retstat = 0;
	char fpath[PATH_MAX] = {0};

	log_msg("\nfusion_statfs(path=\"%s\", statv=0x%08x)\n", path, statv);
	fusion_fullpath(fpath, path);

	// get stats for underlying filesystem
	retstat = statvfs(fpath, statv);
	if (retstat < 0)
		retstat = fusion_error("fusion_statfs statvfs");

	log_statvfs(statv);

	return retstat;
}

/** Possibly flush cached data
 *
 * BIG NOTE: This is not equivalent to fsync().  It's not a
 * request to sync dirty data.
 *
 * Flush is called on each close() of a file descriptor.  So if a
 * filesystem wants to return write errors in close() and the file
 * has cached dirty data, this is a good place to write back data
 * and return any errors.  Since many applications ignore close()
 * errors this is not always useful.
 *
 * NOTE: The flush() method may be called more than once for each
 * open().  This happens if more than one file descriptor refers
 * to an opened file due to dup(), dup2() or fork() calls.  It is
 * not possible to determine if a flush is final, so each flush
 * should be treated equally.  Multiple write-flush sequences are
 * relatively rare, so this shouldn't be a problem.
 *
 * Filesystems shouldn't assume that flush will always be called
 * after some writes, or that if will be called at all.
 *
 * Changed in version 2.2
 */
int fusion_flush(const char *path, struct fuse_file_info *fi)
{
	int retstat = 0;

	log_msg("\nfusion_flush(path=\"%s\", fi=0x%08x)\n", path, fi);
	// no need to get fpath on this one, since I work from fi->fh not the path
	log_fi(fi);

	return retstat;
}

/** Release an open file
 *
 * Release is called when there are no more references to an open
 * file: all file descriptors are closed and all memory mappings
 * are unmapped.
 *
 * For every open() call there will be exactly one release() call
 * with the same flags and file descriptor.  It is possible to
 * have a file opened more than once, in which case only the last
 * release will mean, that no more reads/writes will happen on the
 * file.  The return value of release is ignored.
 *
 * Changed in version 2.2
 */
int fusion_release(const char *path, struct fuse_file_info *fi)
{
	int retstat = 0;

	log_msg("\nfusion_release(path=\"%s\", fi=0x%08x)\n", path, fi);
	log_fi(fi);

	// We need to close the file.  Had we allocated any resources
	// (buffers etc) we'd need to free them here as well.
	retstat = close(fi->fh);

	// /* DFZ: construct the local temp file */
//	const char *local_tmp_dir = "/tmp";
//	char local_pathname[PATH_MAX];
//	strcpy(local_pathname, FUSION_DATA->rootdir);
//	strcat(local_pathname, local_tmp_dir);
//	strcat(local_pathname, path);

	// /* DFZ: construct the remote pathname */
	// const char *remote_node = "dongfang@fusion.cs.iit.edu:~/fusionFS";
	// char remote_pathname[PATH_MAX];
	// strcpy(remote_pathname, remote_node);
	// strcat(remote_pathname, path);

	// /* DFZ: construct the command of remote copy */
	// char cmd_scp[PATH_MAX << 2];
	// strcpy(cmd_scp, "scp ");
	// strcat(cmd_scp, local_pathname);
	// strcat(cmd_scp, " ");
	// strcat(cmd_scp, remote_pathname);

	// /* DFZ: move the temp file from local temp to remote node*/
	// system(cmd_scp);

	/* DFZ: construct the lftp command */
//	char cmd_lftp[PATH_MAX];
//	strcpy(cmd_lftp,
//			"lftp -u dongfang,zdf81IIT fusion.cs.iit.edu -e 'rm fusionFS");
//	strcat(cmd_lftp, path);
//	strcat(cmd_lftp, "; put ");
//	strcat(cmd_lftp, local_pathname);
//	strcat(cmd_lftp, " -o fusionFS");
//	strcat(cmd_lftp, path);
//	strcat(cmd_lftp, "'");

	/* DFZ: debug info */
//	log_msg("\n =====DFZ debug: cmd_lftp = %s \n", cmd_lftp);

//	system(cmd_lftp);

	/*DFZ: update the remote copy*/
	char fpath[PATH_MAX] = {0}, vpath[PATH_MAX] = {0};
	fusion_fullpath(fpath, path);
	fusion_vpath(vpath, path);

	/*DFZ TODO: similar to _getattr(), both localhost and fpath will need to be updated */
	ffs_sendfile_c("udt", "localhost", "9000", fpath, vpath);
	unlink(fpath);

	return retstat;
}

/** Synchronize file contents
 *
 * If the datasync parameter is non-zero, then only the user data
 * should be flushed, not the meta data.
 *
 * Changed in version 2.2
 */
int fusion_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
	int retstat = 0;

	log_msg("\nfusion_fsync(path=\"%s\", datasync=%d, fi=0x%08x)\n", path, datasync,
			fi);
	log_fi(fi);

	if (datasync)
		retstat = fdatasync(fi->fh);
	else
		retstat = fsync(fi->fh);

	if (retstat < 0)
		fusion_error("fusion_fsync fsync");

	return retstat;
}

/** Set extended attributes */
int fusion_setxattr(const char *path, const char *name, const char *value,
		size_t size, int flags) {
	int retstat = 0;
	char fpath[PATH_MAX] = {0};

	log_msg(
			"\nfusion_setxattr(path=\"%s\", name=\"%s\", value=\"%s\", size=%d, flags=0x%08x)\n",
			path, name, value, size, flags);
	fusion_fullpath(fpath, path);

	retstat = lsetxattr(fpath, name, value, size, flags);
	if (retstat < 0)
		retstat = fusion_error("fusion_setxattr lsetxattr");

	return retstat;
}

/** Get extended attributes */
int fusion_getxattr(const char *path, const char *name, char *value, size_t size) {
	int retstat = 0;
	char fpath[PATH_MAX] = {0};

	log_msg(
			"\nfusion_getxattr(path = \"%s\", name = \"%s\", value = 0x%08x, size = %d)\n",
			path, name, value, size);
	fusion_fullpath(fpath, path);

	retstat = lgetxattr(fpath, name, value, size);
	if (retstat < 0)
		retstat = fusion_error("fusion_getxattr lgetxattr");
	else
		log_msg("    value = \"%s\"\n", value);

	return retstat;
}

/** List extended attributes */
int fusion_listxattr(const char *path, char *list, size_t size) {
	int retstat = 0;
	char fpath[PATH_MAX] = {0};
	char *ptr;

	log_msg("fusion_listxattr(path=\"%s\", list=0x%08x, size=%d)\n", path, list,
			size);
	fusion_fullpath(fpath, path);

	retstat = llistxattr(fpath, list, size);
	if (retstat < 0)
		retstat = fusion_error("fusion_listxattr llistxattr");

	log_msg("    returned attributes (length %d):\n", retstat);
	for (ptr = list; ptr < list + retstat; ptr += strlen(ptr) + 1)
		log_msg("    \"%s\"\n", ptr);

	return retstat;
}

/** Remove extended attributes */
int fusion_removexattr(const char *path, const char *name) {
	int retstat = 0;
	char fpath[PATH_MAX] = {0};

	log_msg("\nfusion_removexattr(path=\"%s\", name=\"%s\")\n", path, name);
	fusion_fullpath(fpath, path);

	retstat = lremovexattr(fpath, name);
	if (retstat < 0)
		retstat = fusion_error("fusion_removexattr lrmovexattr");

	return retstat;
}

/** Open directory
 *
 * This method should check if the open operation is permitted for
 * this  directory
 *
 * Introduced in version 2.3
 */
int fusion_opendir(const char *path, struct fuse_file_info *fi)
{

	/*DFZ TODO: this will check if <path> is a key in ZHT */


	DIR *dp;
	int retstat = 0;
	char fpath[PATH_MAX] = {0};

	log_msg("\nfusion_opendir(path=\"%s\", fi=0x%08x)\n", path, fi);
	fusion_fullpath(fpath, path);

	dp = opendir(fpath);
	if (dp == NULL)
		retstat = fusion_error("fusion_opendir opendir");

	fi->fh = (intptr_t) dp;

	log_fi(fi);

	return retstat;
}

/** Read directory
 *
 * This supersedes the old getdir() interface.  New applications
 * should use this.
 *
 * The filesystem may choose between two modes of operation:
 *
 * 1) The readdir implementation ignores the offset parameter, and
 * passes zero to the filler function's offset.  The filler
 * function will not return '1' (unless an error happens), so the
 * whole directory is read in a single readdir operation.  This
 * works just like the old getdir() method.
 *
 * 2) The readdir implementation keeps track of the offsets of the
 * directory entries.  It uses the offset parameter and always
 * passes non-zero offset to the filler function.  When the buffer
 * is full (or an error happens) the filler function will return
 * '1'.
 *
 * Introduced in version 2.3
 */
int fusion_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi)
{
	log_msg(
			"\nfusion_readdir(path=\"%s\", buf=0x%08x, filler=0x%08x, offset=%lld, fi=0x%08x)\n",
			path, buf, filler, offset, fi);

	int retstat = 0;

	/* DFZ: you need to append a '/' if it's not the root directory */
	char dirname[PATH_MAX] = {0};
	strcpy(dirname, path);
	if (strcmp("/", dirname)) {
		strcat(dirname, "/");
	}

//	char filelist[PATH_MAX] = {0};
//	int stat = zht_lookup(dirname, filelist);
	char *filelist = NULL;
	int stat = c_zht_lookup2(dirname, &filelist);

	if (ZHT_LOOKUP_FAIL == stat)
		log_msg("\n ===========DFZ debug: fusion_readdir() filelist not found in ZHT \n\n");
	else
		log_msg("\n ===========DFZ debug: fusion_readdir() filelist = %s. \n\n", filelist);

	char *pch = strtok((char*)filelist, " ");
	while (pch) {

		log_msg("calling filler with name %s\n", pch);
		if (filler(buf, pch, NULL, 0) != 0) {
			log_msg("    ERROR fusion_readdir filler:  buffer full");
			return -ENOMEM;
		}

		pch = strtok(NULL, " ");
	}


//
//	DIR *dp;
//	struct dirent *de;
//
//
//	// once again, no need for fullpath -- but note that I need to cast fi->fh
//	dp = (DIR *) (uintptr_t) fi->fh;
//
//	// Every directory contains at least two entries: . and ..  If my
//	// first call to the system readdir() returns NULL I've got an
//	// error; near as I can tell, that's the only condition under
//	// which I can get an error from readdir()
//	de = readdir(dp);
//	if (de == 0) {
//		retstat = fusion_error("fusion_readdir readdir");
//		return retstat;
//	}
//
//	// This will copy the entire directory into the buffer.  The loop exits
//	// when either the system readdir() returns NULL, or filler()
//	// returns something non-zero.  The first case just means I've
//	// read the whole directory; the second means the buffer is full.
//	do {
//		log_msg("calling filler with name %s\n", de->d_name);
//		if (filler(buf, de->d_name, NULL, 0) != 0) {
//			log_msg("    ERROR fusion_readdir filler:  buffer full");
//			return -ENOMEM;
//		}
//	} while ((de = readdir(dp)) != NULL);
//
//	/*
//	 * DFZ: use hash table to maintain meta data
//	 *
//	 * it seems the global hashtable in <search.h> is not persistent across
//	 * multiple POSIX calls. E.g. I insert an entry in _create() but I cannot find it
//	 * in _readdir(). This hashtable might need to be stored in the ->DATA status, and
//	 * need a dedicated hashtable data structure.
//	 */
//
////	ep = ht_search("/tmpfile");
////	if ((ENTRY*)0 != ep)
////		log_msg("\n =====DFZ debug: ep->key = %s, ep->data = %s (after 'ht_search')\n", ep->key, ep->data);
////	else
////		log_msg("\n =====DFZ debug: not found '/tmpfile' \n ");
//	/*
//	 * DFZ: end hash table
//	 */
//
////	filler(buf, ep->data, NULL, 0);

	log_fi(fi);

	return retstat;
}

/** Release directory
 *
 * Introduced in version 2.3
 */
int fusion_releasedir(const char *path, struct fuse_file_info *fi) {
	int retstat = 0;

	log_msg("\nfusion_releasedir(path=\"%s\", fi=0x%08x)\n", path, fi);
	log_fi(fi);

	closedir((DIR *) (uintptr_t) fi->fh);

	return retstat;
}

/** Synchronize directory contents
 *
 * If the datasync parameter is non-zero, then only the user data
 * should be flushed, not the meta data
 *
 * Introduced in version 2.3
 */
// when exactly is this called?  when a user calls fsync and it
// happens to be a directory? ???
int fusion_fsyncdir(const char *path, int datasync, struct fuse_file_info *fi) {
	int retstat = 0;

	log_msg("\nfusion_fsyncdir(path=\"%s\", datasync=%d, fi=0x%08x)\n", path,
			datasync, fi);
	log_fi(fi);

	return retstat;
}

/**
 * Initialize filesystem
 *
 * The return value will passed in the private_data field of
 * fuse_context to all file operations and as a parameter to the
 * destroy() method.
 *
 * Introduced in version 2.3
 * Changed in version 2.6
 */
// Undocumented but extraordinarily useful fact:  the fuse_context is
// set up before this function is called, and
// fuse_get_context()->private_data returns the user_data passed to
// fuse_main().  Really seems like either it should be a third
// parameter coming in here, or else the fact should be documented
// (and this might as well return void, as it did in older versions of
// FUSE).
void *fusion_init(struct fuse_conn_info *conn) {

	log_msg("\nfusion_init()\n");


	return FUSION_DATA;
}

/**
 * Clean up filesystem
 *
 * Called on filesystem exit.
 *
 * Introduced in version 2.3
 */
void fusion_destroy(void *userdata) {
	log_msg("\nfusion_destroy(userdata=0x%08x)\n", userdata);

	zht_free();
}

/**
 * Check file access permissions
 *
 * This will be called for the access() system call.  If the
 * 'default_permissions' mount option is given, this method is not
 * called.
 *
 * This method is not called under Linux kernel versions 2.4.x
 *
 * Introduced in version 2.5
 */
int fusion_access(const char *path, int mask) {
	int retstat = 0;
	char fpath[PATH_MAX] = {0};

	log_msg("\nfusion_access(path=\"%s\", mask=0%o)\n", path, mask);
	fusion_fullpath(fpath, path);

	retstat = access(fpath, mask);

	if (retstat < 0)
		retstat = fusion_error("fusion_access access");

	return retstat;
}

/**
 * FUSE document:
 *
 * Create and open a file
 *
 * If the file does not exist, first create it with the specified
 * mode, and then open it.
 *
 * If this method is not implemented or under Linux kernel
 * versions earlier than 2.6.15, the mknod() and open() methods
 * will be called instead.
 *
 * Introduced in version 2.5
 *
 * *****************************************************************
 * DFZ:
 * 	In fusionFS, a file creation includes the following tasks:
 *		1) create a local temp file for possible editing
 *		2) upload this local temp file to the remote node
 *		3) insert a <path, vpath> to ZHT
 *		4) update the ZHT entry of <parent path> by appending filename
 */
int fusion_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	int retstat = 0;
	char fpath[PATH_MAX] = {0};
	int fd;

	log_msg("\nfusion_create(path=\"%s\", mode=0%03o, fi=0x%08x)\n", path, mode,
			fi);
	fusion_fullpath(fpath, path);
	fd = creat(fpath, mode);

	if (fd < 0)
		retstat = fusion_error("fusion_create creat");

	fi->fh = fd;

	log_fi(fi);

	/*DFZ: add the last filename of <path> too its parent path in ZHT*/
	char dirname[PATH_MAX] = {0};
	char *pch = strrchr(path, '/');
	strncpy(dirname, path, pch - path + 1);
	log_msg("\n================DFZ debug: dirname = %s \n", dirname);

//	char oldval[PATH_MAX] = {0};
//	int stat = zht_lookup(dirname, oldval);
	char *oldval = NULL;
	int stat = c_zht_lookup2(dirname, &oldval);

	if (ZHT_LOOKUP_FAIL == stat) {
		log_msg("\n================DFZ ERROR: no parent path exists. \n");
		return retstat;
	}

	log_msg("\n================DFZ debug: oldval = %s \n", oldval);
	zht_append(dirname, pch + 1);


	/*DFZ: insert <physicalpath, virtualpath> into ZHT */
	zht_insert(path, " "); /*DFZ TODO: it's fine to add empty val for now, because remote path is constant */

//	char newval[PATH_MAX] = {0};
//	strcpy(newval, oldval);
//	strcat(newval, pch + 1);
//	strcat(newval, " ");
//	log_msg("\n================DFZ debug: newval = %s \n", newval);
//
//
//	zht_remove(dirname);
//	zht_insert(dirname, newval);
//
//	const char *test_newval = zht_lookup(dirname);
//	log_msg("\n================DFZ debug: test_newval = %s \n", test_newval);



//	/*
//	 * DFZ: insert into hash table, with <search.h>
//	 */
//	ht_insert(path, path);
//	e.key = (char *)path;
//	e.data = (void *)path;
//	ep = hsearch(e, ENTER);

///*
// * some basic tests on zht
// */
//	log_msg("\n==============DFZ debug: fusion_create(): before zht_insert() ");
//	zht_remove("key");
//	zht_insert("key", "value changed");
//	log_msg("\n==============DFZ debug: fusion_create(): after zht_insert() ");
//
//	const char *value = zht_lookup("key");
//	log_msg("\n==============DFZ debug: fusion_create(): zht_lookup('key') = %s \n", value);

	return retstat;
}

/**
 * Change the size of an open file
 *
 * This method is called instead of the truncate() method if the
 * truncation was invoked from an ftruncate() system call.
 *
 * If this method is not implemented or under Linux kernel
 * versions earlier than 2.6.15, the truncate() method will be
 * called instead.
 *
 * Introduced in version 2.5
 */
int fusion_ftruncate(const char *path, off_t offset, struct fuse_file_info *fi) {
	int retstat = 0;

	log_msg("\nfusion_ftruncate(path=\"%s\", offset=%lld, fi=0x%08x)\n", path,
			offset, fi);
	log_fi(fi);

	retstat = ftruncate(fi->fh, offset);
	if (retstat < 0)
		retstat = fusion_error("fusion_ftruncate ftruncate");

	return retstat;
}

/**
 * Get attributes from an open file
 *
 * This method is called instead of the getattr() method if the
 * file information is available.
 *
 * Currently this is only called after the create() method if that
 * is implemented (see above).  Later it may be called for
 * invocations of fstat() too.
 *
 * Introduced in version 2.5
 */
// Since it's currently only called after fusion_create(), and fusion_create()
// opens the file, I ought to be able to just use the fd and ignore
// the path...
int fusion_fgetattr(const char *path, struct stat *statbuf,
		struct fuse_file_info *fi) {
	int retstat = 0;

	log_msg("\nfusion_fgetattr(path=\"%s\", statbuf=0x%08x, fi=0x%08x)\n", path,
			statbuf, fi);
	log_fi(fi);

	retstat = fstat(fi->fh, statbuf);
	if (retstat < 0)
		retstat = fusion_error("fusion_fgetattr fstat");

	log_stat(statbuf);

	return retstat;
}

struct fuse_operations fusion_oper = {
		.getattr = fusion_getattr,
		.readlink = fusion_readlink,
		// no .getdir -- that's deprecated
		.getdir = NULL,
		.mknod = fusion_mknod,
		.mkdir = fusion_mkdir,
		.unlink = fusion_unlink,
		.rmdir = fusion_rmdir,
		.symlink = fusion_symlink,
		.rename = fusion_rename,
		.link = fusion_link,
		.chmod = fusion_chmod,
		.chown = fusion_chown,
		.truncate = fusion_truncate,
		.utime = fusion_utime,
		.open = fusion_open,
		.read = fusion_read,
		.write = fusion_write,
		/** Just a placeholder, don't set */ // huh???
		.statfs = fusion_statfs,
		.flush = fusion_flush,
		.release = fusion_release,
		.fsync = fusion_fsync,
		.setxattr = fusion_setxattr,
		.getxattr = fusion_getxattr,
		.listxattr = fusion_listxattr,
		.removexattr = fusion_removexattr,
		.opendir = fusion_opendir,
		.readdir = fusion_readdir,
		.releasedir = fusion_releasedir,
		.fsyncdir = fusion_fsyncdir,
		.init = fusion_init,
		.destroy = fusion_destroy,
		.access = fusion_access,
		.create = fusion_create,
		.ftruncate = fusion_ftruncate,
		.fgetattr = fusion_fgetattr };

void fusion_usage() {
	fprintf(stderr, "usage:  fusionfs rootDir mountPoint\n");
	abort();
}

int main(int argc, char *argv[]) {
	int i;
	int fuse_stat;
	struct fusion_state *fusion_data;

	// fusionfs doesn't do any access checking on its own (the comment
	// blocks in fuse.h mention some of the functions that need
	// accesses checked -- but note there are other functions, like
	// chown(), that also need checking!).  Since running fusionfs as root
	// will therefore open Metrodome-sized holes in the system
	// security, we'll check if root is trying to mount the filesystem
	// and refuse if it is.  The somewhat smaller hole of an ordinary
	// user doing it with the allow_other flag is still there because
	// I don't want to parse the options string.
	if ((getuid() == 0) || (geteuid() == 0)) {
		fprintf(stderr,
				"Running BBFS as root opens unnacceptable security holes\n");
		return 1;
	}

	fusion_data = calloc(sizeof(struct fusion_state), 1);
	if (fusion_data == NULL) {
		perror("main calloc");
		abort();
	}

	fusion_data->logfile = log_open();

	// libfuse is able to do most of the command line parsing; all I
	// need to do is to extract the rootdir; this will be the first
	// non-option passed in.  I'm using the GNU non-standard extension
	// and having realpath malloc the space for the path
	// the string.
	for (i = 1; (i < argc) && (argv[i][0] == '-'); i++)
		if (argv[i][1] == 'o')
			i++; // -o takes a parameter; need to
	// skip it too.  This doesn't
	// handle "squashed" parameters

	if ((argc - i) != 2)
		fusion_usage();

	fusion_data->rootdir = realpath(argv[i], NULL);

	argv[i] = argv[i + 1];
	argc--;

	/* DFZ: initialize the hash table */
	//hcreate(MAX_HT_ENTRY);
	zht_init();

	/*DFZ: add root dir in ZHT*/
	zht_insert("/", " ");

	fprintf(stderr, "about to call fuse_main\n");
	fuse_stat = fuse_main(argc, argv, &fusion_oper, fusion_data);
	fprintf(stderr, "fuse_main returned %d\n", fuse_stat);

	/* DFZ: destruct the hash table */
	//hdestroy();
	zht_free();

	return fuse_stat;
}
