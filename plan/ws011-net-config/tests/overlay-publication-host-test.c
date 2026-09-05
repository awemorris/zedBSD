/*
 * WS011 p009: production overlay lower-only replacement fault/order gate.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Reuse the maintained programmable WS001 backend and private layout mirrors.
 * This is deliberately not a UFS/FAT integration test: backend namespace,
 * references, and mutexes are synthetic.  The linked overlay fsync, close,
 * rename, publication, and error branches are the production implementation.
 */
#define main ws001_fixture_main
#define inode_lookup ws001_fixture_inode_lookup
#define inode_get ws001_fixture_inode_get
#define inode_rename ws001_fixture_inode_rename
#define mount_sync ws001_fixture_mount_sync
#define file_fsync ws001_fixture_file_fsync
#define file_close ws001_fixture_file_close
#include "../../ws001-posix/tests/credential-vfs-overlay-fault-host-test.c"
#undef main
#undef inode_lookup
#undef inode_get
#undef inode_rename
#undef mount_sync
#undef file_fsync
#undef file_close

extern int ws011_overlay_rename(struct inode *, const struct componentname *,
    struct inode *, const struct componentname *, unsigned);
extern int ws011_overlay_regular_fsync(struct file *);
extern int ws011_overlay_regular_close(struct file *);
extern int ws011_overlay_sync_mount(struct mount *);

struct publication_file_info { struct file *real; };

static const struct componentname temporary_name = {
    "net.conf.tmp.18", sizeof("net.conf.tmp.18") - 1U, COMPONENT_LAST
};
static const struct componentname destination_name = {
    "net.conf", sizeof("net.conf") - 1U, COMPONENT_LAST
};
static struct inode publication_source, publication_target;
static struct fixture_overlay_inode_info publication_source_info;
static struct fixture_overlay_inode_info publication_target_info;
static struct file publication_file;
static int data_sync_error, journal_sync_error, close_error;
static char events[64];
static unsigned event_count;

static void
observe(char event)
{
    CHECK(event_count + 1U < sizeof(events));
    events[event_count++] = event;
    events[event_count] = '\0';
}

int
inode_get(struct mount *mount, ino_t ino, struct inode **result)
{
    if (mount == &overlay_mount && ino == publication_source.i_ino) {
        *result = &publication_source;
        return 0;
    }
    if (mount == &overlay_mount && ino == publication_target.i_ino &&
        (publication_target.i_flags & INODE_DEAD) == 0U) {
        *result = &publication_target;
        return 0;
    }
    *result = NULL;
    return ENOENT;
}

int
inode_lookup(struct inode *directory, const struct componentname *name,
    struct inode **result)
{
    if (directory == &materialized_sub &&
        component_is(name, temporary_name.cn_nameptr) && temp_entry) {
        *result = &temp_inode;
        return 0;
    }
    if (directory == &materialized_sub &&
        component_is(name, destination_name.cn_nameptr) && final_entry) {
        *result = &temp_inode;
        return 0;
    }
    if (directory == &lower_sub &&
        component_is(name, destination_name.cn_nameptr)) {
        *result = &lower_file;
        return 0;
    }
    return ENOENT;
}

int
inode_rename(struct inode *old_directory, const struct componentname *old_name,
    struct inode *new_directory, const struct componentname *new_name,
    unsigned flags)
{
    CHECK(old_directory == &materialized_sub);
    CHECK(new_directory == old_directory);
    CHECK(component_is(old_name, temporary_name.cn_nameptr));
    CHECK(component_is(new_name, destination_name.cn_nameptr));
    CHECK(flags == 0U);
    CHECK(temp_entry == 1);
    CHECK(final_entry == 0);
    observe('R');
    rename_calls++;
    if (rename_error != 0)
        return rename_error;
    temp_entry = 0;
    final_entry = 1;
    return 0;
}

int
mount_sync(struct mount *mount)
{
    if (mount == &overlay_mount)
        return ws011_overlay_sync_mount(mount);
    CHECK(mount == &upper_mount);
    CHECK(mount_calls < sizeof(mount_results) / sizeof(mount_results[0]));
    observe('M');
    return mount_calls < mount_result_count ?
        mount_results[mount_calls++] : (mount_calls++, 0);
}

int
file_fsync(struct file *file)
{
    CHECK(file == &destination_file || file == &journal_file);
    if (file == &destination_file) {
        observe('D');
        return data_sync_error;
    }
    observe('J');
    return journal_sync_error;
}

int
file_close(struct file *file)
{
    CHECK(file == &destination_file || file == &source_file);
    observe('C');
    file_closes++;
    return close_error;
}

static void
initialize_publication(void)
{
    struct publication_file_info *info;

    reset_fixture();
    memset(&publication_source, 0, sizeof(publication_source));
    memset(&publication_target, 0, sizeof(publication_target));
    memset(&publication_source_info, 0, sizeof(publication_source_info));
    memset(&publication_target_info, 0, sizeof(publication_target_info));
    memset(&publication_file, 0, sizeof(publication_file));
    memset(events, 0, sizeof(events));
    event_count = 0U;
    data_sync_error = journal_sync_error = close_error = 0;
    sub_info.upper.p_mount = &upper_mount;
    sub_info.upper.p_inode = &materialized_sub;
    strcpy(sub_info.path, "etc");
    materialized_sub.i_mount = &upper_mount;
    lower_sub.i_mount = &lower_mount;
    temp_inode.i_mount = &upper_mount;
    temp_inode.i_linkcount = 1U;
    temp_entry = 1;
    lower_file.i_mount = &lower_mount;
    lower_file.i_linkcount = 1U;
    publication_source.i_mount = publication_target.i_mount = &overlay_mount;
    publication_source.i_type = publication_target.i_type = INODE_REG;
    publication_source.i_ino = 100U;
    publication_target.i_ino = 101U;
    publication_source.i_data = &publication_source_info;
    publication_target.i_data = &publication_target_info;
    publication_source_info.upper.p_mount = &upper_mount;
    publication_source_info.upper.p_inode = &temp_inode;
    publication_source_info.identity_index = 0U;
    strcpy(publication_source_info.path, "etc/net.conf.tmp.18");
    publication_target_info.lower.p_mount = &lower_mount;
    publication_target_info.lower.p_inode = &lower_file;
    publication_target_info.identity_index = 1U;
    strcpy(publication_target_info.path, "etc/net.conf");
    state.identities[0].state = state.identities[1].state = 1U;
    state.identities[0].ino = publication_source.i_ino;
    state.identities[1].ino = publication_target.i_ino;
    strcpy(state.identities[0].path, publication_source_info.path);
    strcpy(state.identities[1].path, publication_target_info.path);
    state.next_ino = 102U;
    publication_file.f_inode = &publication_source;
    info = kern_malloc(sizeof(*info));
    CHECK(info != NULL);
    info->real = &destination_file;
    publication_file.f_data = info;
}

/* The atomic-writer and confirmed-commit fixtures separately prove
 * write/fflush and DISARM order.
 * Here a prepared same-directory temporary file crosses the actual overlay
 * durability/close/rename boundary.  Failed sync still closes the descriptor,
 * but never enters rename. */
static int
publish(void)
{
    int error = ws011_overlay_regular_fsync(&publication_file);
    int close_result = ws011_overlay_regular_close(&publication_file);

    CHECK(publication_file.f_data == NULL);
    if (error != 0)
        return error;
    if (close_result != 0)
        return close_result;
    return ws011_overlay_rename(&overlay_sub, &temporary_name, &overlay_sub,
        &destination_name, 0U);
}

static void
check_old_visible(void)
{
    CHECK(final_entry == 0);
    CHECK(temp_entry == 1);
    CHECK(publication_target_info.lower.p_inode == &lower_file);
    CHECK((publication_target.i_flags & INODE_DEAD) == 0U);
    CHECK(state.identities[1].state == 1U);
    CHECK(strcmp(publication_source_info.path, "etc/net.conf.tmp.18") == 0);
    CHECK(lower_file.i_linkcount == 1U);
    CHECK(journal_writes == 0U);
}

static void
check_new_visible(void)
{
    CHECK(final_entry == 1);
    CHECK(temp_entry == 0);
    CHECK(publication_source_info.upper.p_inode == &temp_inode);
    CHECK(publication_source_info.lower.p_inode == NULL);
    CHECK(strcmp(publication_source_info.path, "etc/net.conf") == 0);
    CHECK(strcmp(state.identities[0].path, "etc/net.conf") == 0);
    CHECK((publication_target.i_flags & INODE_DEAD) != 0U);
    CHECK(state.identities[1].state == 2U);
    CHECK(lower_file.i_linkcount == 1U);
    CHECK(journal_writes == 0U);
    CHECK(cache_removes == 2U);
}

int
main(void)
{
    initialize_publication();
    CHECK_ERROR(publish(), 0);
    CHECK(strcmp(events, "DJMCRM") == 0);
    check_new_visible();

    initialize_publication();
    data_sync_error = EIO;
    CHECK_ERROR(publish(), EIO);
    CHECK(strcmp(events, "DC") == 0);
    CHECK(rename_calls == 0U);
    check_old_visible();

    initialize_publication();
    journal_sync_error = ENOSPC;
    CHECK_ERROR(publish(), ENOSPC);
    CHECK(strcmp(events, "DJC") == 0);
    CHECK(rename_calls == 0U);
    check_old_visible();

    initialize_publication();
    mount_results[0] = EIO;
    mount_result_count = 1U;
    CHECK_ERROR(publish(), EIO);
    CHECK(strcmp(events, "DJMC") == 0);
    CHECK(rename_calls == 0U);
    check_old_visible();

    initialize_publication();
    close_error = ENOSPC;
    CHECK_ERROR(publish(), ENOSPC);
    CHECK(strcmp(events, "DJMC") == 0);
    CHECK(rename_calls == 0U);
    check_old_visible();

    initialize_publication();
    rename_error = EIO;
    CHECK_ERROR(publish(), EIO);
    CHECK(strcmp(events, "DJMCR") == 0);
    check_old_visible();

    /* A failed post-rename sync must return the exact error, while retaining
     * the already committed namespace publication rather than stale cache. */
    initialize_publication();
    mount_results[1] = ENOSPC;
    mount_result_count = 2U;
    CHECK_ERROR(publish(), ENOSPC);
    CHECK(strcmp(events, "DJMCRM") == 0);
    check_new_visible();

    printf("ws011-p009 production overlay publication: PASS (%u checks; "
        "synthetic backend/locks)\n", checks);
    return EXIT_SUCCESS;
}
