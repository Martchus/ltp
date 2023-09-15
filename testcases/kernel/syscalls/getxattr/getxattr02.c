// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2011  Red Hat, Inc.
 * Copyright (c) 2023 Marius Kittler <mkittler@suse.de>
 */

/*\
 * [Description]
 *
 * In the user.* namespace, only regular files and directories can
 * have extended attributes. Otherwise getxattr(2) will return -1
 * and set errno to ENODATA.
 *
 * There are 4 test cases:
 * 1. Get attribute from a FIFO, setxattr(2) should return -1 and
 *    set errno to ENODATA
 * 2. Get attribute from a char special file, setxattr(2) should
 *    return -1 and set errno to ENODATA
 * 3. Get attribute from a block special file, setxattr(2) should
 *    return -1 and set errno to ENODATA
 * 4. Get attribute from a UNIX domain socket, setxattr(2) should
 *    return -1 and set errno to ENODATA
 */

#include "config.h"
#include <sys/sysmacros.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_SYS_XATTR_H
# include <sys/xattr.h>
#endif

#include "tst_test.h"
#include "tst_test_macros.h"

#define MNTPOINT "mntpoint"
#define XATTR_TEST_KEY "user.testkey"

#define FIFO "getxattr02fifo"
#define CHR  "getxattr02chr"
#define BLK  "getxattr02blk"
#define SOCK "getxattr02sock"

static char *workdir;

static char *tcases[] = {
	FIFO,			/* case 00, get attr from fifo */
	CHR,			/* case 01, get attr from char special */
	BLK,			/* case 02, get attr from block special */
	SOCK,			/* case 03, get attr from UNIX domain socket */
};

static void run(unsigned int i)
{
#ifdef HAVE_SYS_XATTR_H
	SAFE_CHDIR(workdir);

	if (i == 0) {
		/* Test for xattr support in the current filesystem */
		int fd = SAFE_CREAT("testfile", 0644);
		close(fd);
		TEST(setxattr("testfile", "user.test", "test", 4, XATTR_CREATE));
		if (TST_RET < 0) {
			if (TST_ERR == ENOTSUP)
				tst_brk(TCONF, "no xattr support in filesystem");
			tst_brk(TBROK | TTERRNO, "unexpected setxattr() return code");
			return;
		}
		unlink("testfile");

		/* Create test files */
		if (mknod(FIFO, S_IFIFO | 0777, 0) < 0)
			tst_brk(TBROK | TERRNO, "create FIFO(%s) failed", FIFO);
		dev_t dev = makedev(1, 3);
		if (mknod(CHR, S_IFCHR | 0777, dev) < 0)
			tst_brk(TBROK | TERRNO, "create char special(%s)"
				" failed", CHR);
		if (mknod(BLK, S_IFBLK | 0777, 0) < 0)
			tst_brk(TBROK | TERRNO, "create block special(%s)"
				" failed", BLK);
		if (mknod(SOCK, S_IFSOCK | 0777, 0) < 0)
			tst_brk(TBROK | TERRNO, "create socket(%s) failed",
				SOCK);
	}

	int exp_eno = ENODATA;
	char buf[BUFSIZ];
	TEST(getxattr(tcases[i], XATTR_TEST_KEY, buf, BUFSIZ));
	if (TST_RET == -1 && TST_ERR == exp_eno)
		tst_res(TPASS | TTERRNO, "expected return value");
	else
		tst_res(TFAIL | TTERRNO, "unexpected return value"
				" - expected errno %d - got", exp_eno);
#endif
}

static void setup(void)
{
#ifdef HAVE_SYS_XATTR_H
	char *cwd = SAFE_GETCWD(NULL, 0);
	workdir = SAFE_MALLOC(strlen(cwd) + strlen(MNTPOINT) + 2);
	sprintf(workdir, "%s/%s", cwd, MNTPOINT);
	free(cwd);
#else
	tst_brk(TCONF, "<sys/xattr.h> does not exist.");
#endif
}

static struct tst_test test = {
#ifdef HAVE_SYS_XATTR_H
	.all_filesystems = 1,
	.needs_root = 1,
	.mntpoint = MNTPOINT,
	.mount_device = 1,
	.skip_filesystems = (const char *const []) {
			"exfat",
			"tmpfs",
			"ramfs",
			"nfs",
			"vfat",
			NULL
	},
#endif
	.setup = setup,
	.test = run,
	.tcnt = ARRAY_SIZE(tcases)
};
