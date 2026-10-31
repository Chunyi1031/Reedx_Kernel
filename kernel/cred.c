// SPDX-License-Identifier: GPL-2.0-only
/**
 * kernel/cred.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 进程身份
 */
#include <cred.h>
#include <syscalls.h>
#include <vfs.h>

void CredRoot(cred_t *c){
    c->uid = c->euid = c->suid = CRED_ROOT_ID;
    c->gid = c->egid = c->sgid = CRED_ROOT_ID;
    c->umask = CRED_UMASK_DEF;
    c->ngroups = 0;
}

//当前身份是否属于gid
_Bool CredInGroup(const cred_t *c, uint32_t gid){
    if(!c)return 0;
    if(c->egid == gid)return 1;
    for(uint32_t i = 0;i < c->ngroups && i < CRED_NGROUPS_MAX;i++){
        if(c->groups[i] == gid)return 1;
    }
    return 0;
}

//setgroups(2)
int CredSetGroups(cred_t *c, uint32_t n, const uint32_t *list){
    if(!c)return -EPERM;
    if(!CredIsRoot(c))return -EPERM;
    if(n > CRED_NGROUPS_MAX)return -EINVAL;
    for(uint32_t i = 0;i < n;i++)c->groups[i] = list[i];
    c->ngroups = n;
    return 0;
}

void CredCopy(cred_t *dst, const cred_t *src){
    *dst = *src;
}

_Bool CredIsRoot(const cred_t *c){
    return (c->euid == CRED_ROOT_ID);
}

_Bool CredMaySetId(const cred_t *c, uint32_t id){
    if(CredIsRoot(c))return true;//为root身份，可以切换
    return ((id == c->uid) || (id == c->euid) || (id == c->suid));//普通用户，只能切换到自己的三份ID之一
}

int CredSetUid(cred_t *c, uint32_t uid){
    //root权限，三份一起改
    if(CredIsRoot(c)){
        c->uid = c->euid = c->suid = uid;
        return 0;
    }
    //普通用户，把有效ID换成自己的真实ID或保存ID
    if(uid == c->uid || uid == c->suid){
        c->euid = uid;
        return 0;
    }
    return -EPERM;
}

int CredSetGid(cred_t *c, uint32_t gid){
    if(CredIsRoot(c)){
        c->gid = c->egid = c->sgid = gid;
        return 0;
    }
    if(gid == c->gid || gid == c->sgid){
        c->egid = gid;
        return 0;
    }
    return -EPERM;
}

int CredSetReUid(cred_t *c, uint32_t ruid, uint32_t euid){
    //目标ID不为-1且当前身份无权限时拒绝
    if(ruid != CRED_NO_CHANGE && !CredMaySetId(c,ruid))return -EPERM;
    if(euid != CRED_NO_CHANGE && !CredMaySetId(c,euid))return -EPERM;
    //目标ID不为-1时修改
    if(ruid != CRED_NO_CHANGE)c->uid = ruid;
    if(euid != CRED_NO_CHANGE)c->euid = euid;
    if(ruid != CRED_NO_CHANGE || euid != CRED_NO_CHANGE)c->suid = c->euid;//改过就重记保存ID
    return 0;
}

int CredSetReGid(cred_t *c, uint32_t rgid, uint32_t egid){
    if(rgid != CRED_NO_CHANGE && !CredMaySetId(c, rgid))return -EPERM;
    if(egid != CRED_NO_CHANGE && !CredMaySetId(c, egid))return -EPERM;
    if(rgid != CRED_NO_CHANGE)c->gid = rgid;
    if(egid != CRED_NO_CHANGE)c->egid = egid;
    if(rgid != CRED_NO_CHANGE || egid != CRED_NO_CHANGE)c->sgid = c->egid;
    return 0;
}

int CredSetResUid(cred_t *c, uint32_t ruid, uint32_t euid, uint32_t suid){
    //目标ID不为-1且当前身份无权限时拒绝
    if(ruid != CRED_NO_CHANGE && !CredMaySetId(c, ruid))return -EPERM;
    if(euid != CRED_NO_CHANGE && !CredMaySetId(c, euid))return -EPERM;
    if(suid != CRED_NO_CHANGE && !CredMaySetId(c, suid))return -EPERM;
    //目标ID不为-1时修改
    if(ruid != CRED_NO_CHANGE)c->uid  = ruid;
    if(euid != CRED_NO_CHANGE)c->euid = euid;
    if(suid != CRED_NO_CHANGE)c->suid = suid;
    return 0;
}

int CredSetResGid(cred_t *c, uint32_t rgid, uint32_t egid, uint32_t sgid){
    if(rgid != CRED_NO_CHANGE && !CredMaySetId(c, rgid))return -EPERM;
    if(egid != CRED_NO_CHANGE && !CredMaySetId(c, egid))return -EPERM;
    if(sgid != CRED_NO_CHANGE && !CredMaySetId(c, sgid))return -EPERM;
    if(rgid != CRED_NO_CHANGE)c->gid  = rgid;
    if(egid != CRED_NO_CHANGE)c->egid = egid;
    if(sgid != CRED_NO_CHANGE)c->sgid = sgid;
    return 0;
}

int CredCheckAccess(const cred_t *c, uint16_t mode, uint32_t i_uid, uint32_t i_gid, int mask){
    if(!c)return -EPERM;
    if(!mask)return 0;
    //root读写都放行
    if(CredIsRoot(c)){
        if((mask & MAY_EXEC) && !(mode & 0111))return -EACCES;//执行要有x位
        return 0;
    }
    //属主看own位,属组看group位,其余看other位
    uint32_t bits;
    if(c->euid == i_uid)bits = (uint32_t)((mode >> 6) & 7);
    else if(CredInGroup(c, i_gid))bits = (uint32_t)((mode >> 3) & 7);
    else bits = (uint32_t)(mode & 7);
    //按要求的访问类型检查对应的权限位
    if((mask & MAY_READ) && !(bits & 4))return -EACCES;
    if((mask & MAY_WRITE) && !(bits & 2))return -EACCES;
    if((mask & MAY_EXEC) && !(bits & 1))return -EACCES;
    return 0;
}
