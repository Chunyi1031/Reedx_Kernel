// SPDX-License-Identifier: GPL-2.0-only
/**
 * include/cred.h
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * Reedx进程身份
 */
#ifndef _CRED_H_
#define _CRED_H_

#include <klib.h>

#define CRED_ROOT_ID    0U            //root的uid与gid
#define CRED_NO_CHANGE  ((uint32_t)-1)//保持不变
#define CRED_UMASK_DEF  022U          //内核与init的默认umask
#define CRED_NGROUPS_MAX 16U          //附加组个数上限

//进程身份
typedef struct cred {
    uint32_t uid,euid,suid;//真实/有效/保存的用户ID
    uint32_t gid,egid,sgid;//真实/有效/保存的组ID
    uint32_t umask;//新建文件时屏蔽掉的权限位
    uint32_t ngroups;//附加组个数
    uint32_t groups[CRED_NGROUPS_MAX];//附加组列表
} cred_t;

void CredRoot(cred_t *c);//把身份设成root的默认值
void CredCopy(cred_t *dst, const cred_t *src);//复制一份身份
_Bool CredIsRoot(const cred_t *c);//获取有效用户ID是不是root
_Bool CredInGroup(const cred_t *c, uint32_t gid);//当前身份是否属于gid
int CredSetGroups(cred_t *c, uint32_t n, const uint32_t *list);//setgroups(2)
_Bool CredMaySetId(const cred_t *c, uint32_t id);//获取当前身份能不能切换到id
int CredCheckAccess(const cred_t *c, uint16_t mode, uint32_t i_uid, uint32_t i_gid, int mask);//按属主/属组/其他的权限位判断能否访问
int CredSetUid(cred_t *c, uint32_t uid);//setuid(2)
int CredSetGid(cred_t *c, uint32_t gid);//setgid(2)
int CredSetReUid(cred_t *c, uint32_t ruid, uint32_t euid);//setreuid(2)
int CredSetReGid(cred_t *c, uint32_t rgid, uint32_t egid);//setregid(2)
int CredSetResUid(cred_t *c, uint32_t ruid, uint32_t euid, uint32_t suid);//setresuid(2)
int CredSetResGid(cred_t *c, uint32_t rgid, uint32_t egid, uint32_t sgid);//setresgid(2)

#endif
