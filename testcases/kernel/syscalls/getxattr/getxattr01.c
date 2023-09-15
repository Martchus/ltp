// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2011 Red Hat, Inc.
 * Copyright (c) 2023 Marius Kittler <mkittler@suse.de>
 */

/*\
 * [Description]
 *
 * Basic tests for getxattr(2), there are 3 test cases:
 * 1. Get an non-existing attribute,
 *    getxattr(2) should return -1 and set errno to ENODATA.
 * 2. Buffer size is smaller than attribute value size,
 *    getxattr(2) should return -1 and set errno to ERANGE.
 * 3. getxattr(2) should succeed and return the same value we set
 *    before.
 */

#include "config.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>

#include "tst_test.h"

#ifdef HAVE_SYS_XATTR_H
#include <sys/xattr.h>

#include "tst_test_macros.h"

#define MNTPOINT "mntpoint"
#define FNAME MNTPOINT"/getxattr01testfile"
#define XATTR_TEST_KEY "user.testkey"
#define XATTR_TEST_VALUE "this is a test value"
#define XATTR_TEST_VALUE_SIZE 20
#define BUFFSIZE 64

static struct test_case {
	char *key;
	char value[BUFFSIZE];
	size_t size;
	int exp_err;
} tcases[] = {
	{ /* case 00, get non-existing attribute */
	 .key = "user.nosuchkey",
	 .value = {0},
	 .size = BUFFSIZE - 1,
	 .exp_err = ENODATA,
	},
	{ /* case 01, small value buffer */
	 .key = XATTR_TEST_KEY,
	 .value = {0},
	 .size = 1,
	 .exp_err = ERANGE,
	},
	{ /* case 02, get existing attribute */
	 .key = XATTR_TEST_KEY,
	 .value = {0},
	 .size = BUFFSIZE - 1,
	 .exp_err = 0,
	},
};

static void run(unsigned int i)
{
	struct test_case *tc = &tcases[i];

	/* read xattr back */
	TEST(getxattr(FNAME, tc->key, tc->value, tc->size));
	if (TST_ERR == tc->exp_err) {
		tst_res(TPASS | TTERRNO, "expected getxattr() return code");
	} else {
		tst_res(TFAIL | TTERRNO, "unexpected getxattr() return code"
				" - expected errno %d", tc->exp_err);
	}

	/* verify the value for non-error test cases */
	if (tc->exp_err)
		return;
	TST_EXP_EQ_LI(TST_RET, XATTR_TEST_VALUE_SIZE);
	if (memcmp(tc->value, XATTR_TEST_VALUE, XATTR_TEST_VALUE_SIZE))
		tst_res(TFAIL, "wrong value, expected \"%s\" got \"%s\"",
				XATTR_TEST_VALUE, tc->value);
	else
		tst_res(TPASS, "right value");
}

static void setup(void)
{
	SAFE_TOUCH(FNAME, 0644, NULL);
	SAFE_SETXATTR(FNAME, XATTR_TEST_KEY, XATTR_TEST_VALUE,
				  strlen(XATTR_TEST_VALUE), 0);
}

static struct tst_test test = {
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
	.setup = setup,
	.test = run,
	.tcnt = ARRAY_SIZE(tcases)
};

#else
TST_TEST_TCONF("System doesn't have <sys/xattr.h>");
#endif
