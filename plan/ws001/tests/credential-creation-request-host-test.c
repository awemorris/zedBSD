/*
 * WS001 p015: user creation authorization/snapshot locking contract.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */

#include <kern/cred.h>
#include <kern/inode.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

#define CHECK(expression)                                                   \
	do {                                                                 \
		checks++;                                                    \
		if (!(expression)) {                                        \
			fprintf(stderr,                                        \
			    "ws001-p015 creation request: failed at %s:%d: %s\n", \
			    __FILE__, __LINE__, #expression);                  \
			exit(EXIT_FAILURE);                                   \
		}                                                            \
	} while (0)

#define CHECK_ERROR(expression, wanted)                                    \
	do {                                                                 \
		int result_ = (expression);                                  \
		int wanted_ = (wanted);                                      \
		checks++;                                                    \
		if (result_ != wanted_) {                                    \
			fprintf(stderr,                                        \
			    "ws001-p015 creation request: failed at %s:%d: "  \
			    "got %d wanted %d\n", __FILE__, __LINE__,         \
			    result_, wanted_);                                  \
			exit(EXIT_FAILURE);                                   \
		}                                                            \
	} while (0)

static struct inode *expected_parent;
static unsigned lock_count;
static unsigned unlock_count;
static unsigned authorization_count;
static int metadata_locked;
static int authorization_result;
static int replace_parent_metadata_during_authorization;

void
mutex_lock(struct mutex *mutex)
{
	CHECK(expected_parent != NULL);
	CHECK(mutex == &expected_parent->i_io_lock);
	CHECK(metadata_locked == 0);
	metadata_locked = 1;
	lock_count++;
}

void
mutex_unlock(struct mutex *mutex)
{
	CHECK(expected_parent != NULL);
	CHECK(mutex == &expected_parent->i_io_lock);
	CHECK(metadata_locked == 1);
	metadata_locked = 0;
	unlock_count++;
}

int
vfs_may_create(const struct inode *parent, const struct ucred *credential)
{
	CHECK(parent == expected_parent);
	CHECK(credential != NULL);
	CHECK(metadata_locked == 1);
	authorization_count++;
	/* This hook models the state observed by authorization.  The request must
	 * consume this complete replacement before releasing the same lock. */
	if (replace_parent_metadata_during_authorization) {
		expected_parent->i_mode = S_IFDIR | 0755U;
		expected_parent->i_gid = 333U;
	}
	return authorization_result;
}

static void
reset_contract(struct inode *parent)
{
	expected_parent = parent;
	lock_count = 0;
	unlock_count = 0;
	authorization_count = 0;
	metadata_locked = 0;
	authorization_result = 0;
	replace_parent_metadata_during_authorization = 0;
}

static void
check_setgid_snapshot(void)
{
	struct inode parent;
	struct ucred credential;
	struct inode_creation_request request;

	memset(&parent, 0, sizeof(parent));
	memset(&credential, 0, sizeof(credential));
	parent.i_type = INODE_DIR;
	parent.i_mode = S_IFDIR | S_ISGID | 0770U;
	parent.i_gid = 222U;
	credential.euid = 123U;
	credential.egid = 456U;
	reset_contract(&parent);
	CHECK_ERROR(inode_creation_request_user(&parent, &credential,
	    INODE_DIR, 0750U, 0, NULL, &request), 0);
	CHECK(lock_count == 1U);
	CHECK(unlock_count == 1U);
	CHECK(authorization_count == 1U);
	CHECK(metadata_locked == 0);
	CHECK(request.origin == INODE_CREATION_USER);
	CHECK(request.type == INODE_DIR);
	CHECK(request.uid == 123U);
	CHECK(request.gid == 222U);
	CHECK(request.mode == (0750U | S_ISGID));
}

static void
check_authorized_snapshot_is_one_lock_domain(void)
{
	struct inode parent;
	struct ucred credential;
	struct inode_creation_request request;

	memset(&parent, 0, sizeof(parent));
	memset(&credential, 0, sizeof(credential));
	parent.i_type = INODE_DIR;
	parent.i_mode = S_IFDIR | S_ISGID | 0770U;
	parent.i_gid = 222U;
	credential.euid = 123U;
	credential.egid = 456U;
	reset_contract(&parent);
	replace_parent_metadata_during_authorization = 1;
	CHECK_ERROR(inode_creation_request_user(&parent, &credential,
	    INODE_REG, 0640U, 0, NULL, &request), 0);
	CHECK(lock_count == 1U);
	CHECK(unlock_count == 1U);
	CHECK(authorization_count == 1U);
	CHECK(request.gid == 456U);
	CHECK(request.mode == 0640U);
	CHECK((request.mode & S_ISGID) == 0);
}

static void
check_denial_unlocks_without_a_request(void)
{
	struct inode parent;
	struct ucred credential;
	struct inode_creation_request request;

	memset(&parent, 0, sizeof(parent));
	memset(&credential, 0, sizeof(credential));
	memset(&request, 0xa5, sizeof(request));
	parent.i_type = INODE_DIR;
	parent.i_mode = S_IFDIR | 0777U;
	credential.euid = 123U;
	credential.egid = 456U;
	reset_contract(&parent);
	authorization_result = EACCES;
	CHECK_ERROR(inode_creation_request_user(&parent, &credential,
	    INODE_REG, 0600U, 0, NULL, &request), EACCES);
	CHECK(lock_count == 1U);
	CHECK(unlock_count == 1U);
	CHECK(authorization_count == 1U);
	CHECK(metadata_locked == 0);
	CHECK(request.origin == INODE_CREATION_INVALID);
}

int
main(void)
{
	check_setgid_snapshot();
	check_authorized_snapshot_is_one_lock_domain();
	check_denial_unlocks_without_a_request();
	printf("ws001-p015 creation authorization/snapshot: PASS (%u checks)\n",
	    checks);
	return EXIT_SUCCESS;
}
