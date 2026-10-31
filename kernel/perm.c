// SPDX-License-Identifier: GPL-2.0-only
/**
 * kernel/perm.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * VFS通用权限检查
 */
#include <vfs.h>
#include <cred.h>
#include <task.h>
#include <syscalls.h>

const cred_t *CurrentCred(void){
    static const cred_t boot_cred = {
        .uid = 0,
        .euid = 0,
        .suid = 0,
        .gid = 0,
        .egid = 0,
        .sgid = 0,
        .umask = CRED_UMASK_DEF
    };
    if(!current_task)return &boot_cred;
    return &current_task->cred;
}

int InodePermission(struct inode *inode, int mask){
    if(!inode)return -ENOENT;
    if(!mask)return 0;
    return CredCheckAccess(CurrentCred(), inode->i_mode, inode->i_uid, inode->i_gid, mask);
}
