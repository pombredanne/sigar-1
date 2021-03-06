/*
 * Copyright (c) 2004-2009 Hyperic, Inc.
 * Copyright (c) 2009 SpringSource, Inc.
 * Copyright (c) 2009-2010 VMware, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* pull in time.h before resource.h does w/ _KERNEL */
#include <sys/time.h>
#define _KERNEL 1
#include <sys/file.h>     /* for struct file */
#include <sys/resource.h> /* for rlimit32 in 64-bit mode */
#undef  _KERNEL

#include "sigar.h"
#include "sigar_private.h"
#include "sigar_util.h"
#include "sigar_os.h"

#include <dlfcn.h>
#include <nlist.h>
#include <pthread.h>
#include <stdio.h>
#include <utmp.h>
#include <libperfstat.h>
#include <pthread.h>

#include <sys/statfs.h>
#include <sys/systemcfg.h>
#include <sys/sysinfo.h>
#include <sys/var.h>
#include <sys/vminfo.h>
#include <sys/mntctl.h>
#include <sys/stat.h>
#include <sys/user.h>
#include <sys/utsname.h>
#include <sys/vmount.h>
#include <sys/proc.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>

/* for proc_port */
#include <netinet/in_pcb.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/socketvar.h>

/* for net_connection_list */
#include <netinet/ip_var.h>
#include <netinet/tcp_timer.h>
#include <netinet/tcp_var.h>
#include <netinet/tcp_fsm.h>

/* for odm api */
#include <sys/cfgodm.h>
#include <sys/cfgdb.h>
#include <cf.h>

#include <sys/ldr.h>

/* for net_interface_config ipv6 */
#include <sys/ioctl.h>
#include <netinet/in6_var.h>

/* for getkerninfo */
#include <sys/kinfo.h>

/* not defined in aix 4.3 */
#ifndef SBITS
#define SBITS 16
#endif

#ifndef PTHRDSINFO_RUSAGE_START
#define PTHRDSINFO_RUSAGE_START   0x00000001
#define PTHRDSINFO_RUSAGE_STOP    0x00000002
#define PTHRDSINFO_RUSAGE_COLLECT 0x00000004
#endif

/*
 * from libperfstat.h:
 * "To calculate the load average, divide the numbers by (1<<SBITS).
 *  SBITS is defined in <sys/proc.h>."
 */
#define FIXED_TO_DOUBLE(x) (((double)x) / (1<<SBITS))

/* these offsets wont change so just lookup them up during open */
static int get_koffsets(sigar_t *sigar)
{
    int i;
    /* see man knlist and nlist.h */
    struct nlist klist[] = {
        {"avenrun", 0, 0, 0, 0, 0}, /* KOFFSET_LOADAVG */
        {"v", 0, 0, 0, 0, 0}, /* KOFFSET_VAR */
        {"sysinfo", 0, 0, 0, 0, 0}, /* KOFFSET_SYSINFO */
        {"ifnet", 0, 0, 0, 0, 0}, /* KOFFSET_IFNET */
        {"vmminfo", 0, 0, 0, 0, 0}, /* KOFFSET_VMINFO */
        {"cpuinfo", 0, 0, 0, 0, 0}, /* KOFFSET_CPUINFO */
        {"tcb", 0, 0, 0, 0, 0}, /* KOFFSET_TCB */
        {"arptabsize", 0, 0, 0, 0, 0}, /* KOFFSET_ARPTABSIZE */
        {"arptabp", 0, 0, 0, 0, 0}, /* KOFFSET_ARPTABP */
        {NULL, 0, 0, 0, 0, 0}
    };

    if (knlist(klist,
               sizeof(klist) / sizeof(klist[0]),
               sizeof(klist[0])) != 0)
    {
        return errno;
    }

    for (i=0; i<KOFFSET_MAX; i++) {
        sigar->koffsets[i] = klist[i].n_value;
    }

    return SIGAR_OK;
}

static int kread(sigar_t *sigar, void *data, int size, long offset)
{
    if (sigar->kmem < 0) {
        return SIGAR_EPERM_KMEM;
    }

    if (lseek(sigar->kmem, offset, SEEK_SET) != offset) {
        return errno;
    }

    if (read(sigar->kmem, data, size) != size) {
        return errno;
    }

    return SIGAR_OK;
}

static int sigar_thread_rusage(struct rusage *usage, int mode)
{
    return pthread_getrusage_np(pthread_self(), usage, mode);
}

static int sigar_perfstat_memory(perfstat_memory_total_t *memory)
{
    return perfstat_memory_total(NULL, memory, sizeof(*memory), 1);
}

static int sigar_perfstat_cpu(perfstat_cpu_total_t *cpu_total)
{
    return perfstat_cpu_total(NULL, cpu_total, sizeof(*cpu_total), 1);
}

int sigar_os_open(sigar_t **sigar)
{
    int status, i;
    int kmem = -1;
    struct utsname name;

    kmem = open("/dev/kmem", O_RDONLY);

    *sigar = malloc(sizeof(**sigar));

    (*sigar)->getprocfd = NULL; /*XXX*/
    (*sigar)->kmem = kmem;
    (*sigar)->pagesize = 0;
    (*sigar)->ticks = sysconf(_SC_CLK_TCK);
    (*sigar)->boot_time = 0;
    (*sigar)->last_pid = -1;
    (*sigar)->pinfo = NULL;
    (*sigar)->cpuinfo = NULL;
    (*sigar)->cpuinfo_size = 0;
    SIGAR_ZERO(&(*sigar)->swaps);

    i = getpagesize();
    while ((i >>= 1) > 0) {
        (*sigar)->pagesize++;
    }

    if (kmem > 0) {
        if ((status = get_koffsets(*sigar)) != SIGAR_OK) {
            /* libperfstat only mode (aix 6) */
            close((*sigar)->kmem);
            (*sigar)->kmem = -1;
        }
    }

    (*sigar)->cpu_mhz = -1;

    (*sigar)->model[0] = '\0';

    uname(&name);

    (*sigar)->aix_version = atoi(name.version);

    (*sigar)->thrusage = PTHRDSINFO_RUSAGE_STOP;

    (*sigar)->diskmap = NULL;

    return SIGAR_OK;
}

static void swaps_free(swaps_t *swaps);

int sigar_os_close(sigar_t *sigar)
{
    swaps_free(&sigar->swaps);
    if (sigar->kmem > 0) {
        close(sigar->kmem);
    }
    if (sigar->pinfo) {
        free(sigar->pinfo);
    }
    if (sigar->cpuinfo) {
        free(sigar->cpuinfo);
    }
    if (sigar->diskmap) {
        sigar_cache_destroy(sigar->diskmap);
    }
    if (sigar->thrusage == PTHRDSINFO_RUSAGE_START) {
        struct rusage usage;
        sigar_thread_rusage(&usage,
                            PTHRDSINFO_RUSAGE_STOP);
    }
    free(sigar);
    return SIGAR_OK;
}

char *sigar_os_error_string(sigar_t *sigar, int err)
{
    switch (err) {
      case SIGAR_EPERM_KMEM:
        return "Failed to open /dev/kmem for reading";
      default:
        return NULL;
    }
}

#define PAGESHIFT(v) \
    ((v) << sigar->pagesize)

int sigar_mem_get(sigar_t *sigar, sigar_mem_t *mem)
{
    int status;
    perfstat_memory_total_t minfo;
    sigar_uint64_t kern;

    if (sigar_perfstat_memory(&minfo) == 1) {
        mem->total = PAGESHIFT(minfo.real_total);
        mem->free  = PAGESHIFT(minfo.real_free);
        kern = PAGESHIFT(minfo.numperm); /* number of pages in file cache */
    }
    else {
        return errno;
    }

    mem->used = mem->total - mem->free;
    mem->actual_used = mem->used - kern;
    mem->actual_free = mem->free + kern;

    sigar_mem_calc_ram(sigar, mem);

    return SIGAR_OK;
}

static void swaps_free(swaps_t *swaps)
{
    if (swaps->num) {
        int i;

        for (i=0; i<swaps->num; i++) {
            free(swaps->devs[i]);
        }

        free(swaps->devs);

        swaps->num = 0;
    }
}

/*
 * there is no public api for parsing this file.
 * well, there is something, but its super ugly and requires
 * linking 2 static libraries (libodm and something else)
 * maybe will switch to that if it can add value elsewhere too.
 */
#define SWAPSPACES "/etc/swapspaces"

static int swaps_get(swaps_t *swaps)
{
    FILE *fp;
    char buf[512];
    char *ptr;
    struct stat statbuf;

    if (stat(SWAPSPACES, &statbuf) < 0) {
        return errno;
    }

    /* only re-parse if file has changed */
    if (swaps->mtime == statbuf.st_mtime) {
        return 0;
    }

    swaps->mtime = statbuf.st_mtime;

    /* easier to just start from scratch */
    swaps_free(swaps);

    if (!(fp = fopen(SWAPSPACES, "r"))) {
        return errno;
    }

    while ((ptr = fgets(buf, sizeof(buf), fp))) {
        if (!isalpha(*ptr)) {
            continue;
        }

        if (strchr(ptr, ':')) {
            int len;

            ptr = fgets(buf, sizeof(buf), fp);

            while (isspace(*ptr)) {
                ++ptr;
            }

            if (strncmp(ptr, "dev", 3)) {
                continue;
            }
            ptr += 3;
            while (isspace(*ptr) || (*ptr == '=')) {
                ++ptr;
            }

            len = strlen(ptr);
            ptr[len-1] = '\0'; /* -1 == chomp \n */

            swaps->devs = realloc(swaps->devs, swaps->num+1 * sizeof(char *));
            swaps->devs[swaps->num] = malloc(len);
            memcpy(swaps->devs[swaps->num], ptr, len);

            swaps->num++;
        }
    }

    fclose(fp);

    return 0;
}

/*
 * documented in aix tech ref,
 * but this prototype is not in any friggin header file.
 * struct pginfo is in sys/vminfo.h
 */

int swapqry(char *path, struct pginfo *info);

static int sigar_swap_get_swapqry(sigar_t *sigar, sigar_swap_t *swap)
{
    int status, i;

    if ((status = swaps_get(&sigar->swaps)) != SIGAR_OK) {
        return status;
    }

    if (SIGAR_LOG_IS_DEBUG(sigar)) {
        sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                         "[swap] pagesize=%d, shift=%d",
                         getpagesize(), sigar->pagesize);
    }

    swap->total = swap->free = 0;

    for (i=0; i<sigar->swaps.num; i++) {
        struct pginfo info;

        status = swapqry(sigar->swaps.devs[i], &info);

        if (status != 0) {
            if (SIGAR_LOG_IS_DEBUG(sigar)) {
                sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                                 "[swap] swapqry(%s) failed: %s",
                                 sigar->swaps.devs[i],
                                 sigar_strerror(sigar, errno));
            }
            continue;
        }

        if (SIGAR_LOG_IS_DEBUG(sigar)) {
            sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                             "[swap] %s total=%d/%d, free=%d/%d",
                             sigar->swaps.devs[i],
                             info.size, PAGESHIFT(info.size),
                             info.free, PAGESHIFT(info.free));
        }

        swap->total += PAGESHIFT(info.size); /* lsps -a */
        swap->free  += PAGESHIFT(info.free);
    }

    swap->used = swap->total - swap->free;

    return SIGAR_OK;
}

#define SWAP_DEV(ps) \
   ((ps.type == LV_PAGING) ? \
     ps.u.lv_paging.vgname : \
     ps.u.nfs_paging.filename)

#define SWAP_MB_TO_BYTES(v) ((v) * (1024 * 1024))

int sigar_swap_get(sigar_t *sigar, sigar_swap_t *swap)
{
    perfstat_memory_total_t minfo;
    perfstat_pagingspace_t ps;
    perfstat_id_t id;

    id.name[0] = '\0';

    SIGAR_ZERO(swap);

    do {
        if (perfstat_pagingspace(&id, &ps, sizeof(ps), 1) != 1) {
            if (SIGAR_LOG_IS_DEBUG(sigar)) {
                sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                                 "[swap] dev=%s query failed: %s",
                                 SWAP_DEV(ps),
                                 sigar_strerror(sigar, errno));
            }
            continue;
        }
        if (SIGAR_LOG_IS_DEBUG(sigar)) {
            sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                             "[swap] dev=%s: active=%s, "
                             "total=%lluMb, used=%lluMb",
                             SWAP_DEV(ps),
                             ((ps.active == 1) ? "yes" : "no"),
                             ps.mb_size, ps.mb_used);
        }
        if (ps.active != 1) {
            continue;
        }
        /* convert MB sizes to bytes */
        swap->total += SWAP_MB_TO_BYTES(ps.mb_size);
        swap->used  += SWAP_MB_TO_BYTES(ps.mb_used);
    } while (id.name[0] != '\0');

    swap->free = swap->total - swap->used;

    if (sigar_perfstat_memory(&minfo) == 1) {
        swap->page_in = minfo.pgins;
        swap->page_out = minfo.pgouts;
    }
    else {
        swap->page_in = swap->page_out = -1;
    }
    return SIGAR_OK;
}

int sigar_cpu_get(sigar_t *sigar, sigar_cpu_t *cpu)
{
    int i, status;
    struct sysinfo data;
    perfstat_cpu_total_t cpu_data;

    if (sigar_perfstat_cpu(&cpu_data) == 1) {
        cpu->user  = SIGAR_TICK2MSEC(cpu_data.user);
        cpu->nice  = SIGAR_FIELD_NOTIMPL; /* N/A */
        cpu->sys   = SIGAR_TICK2MSEC(cpu_data.sys);
        cpu->idle  = SIGAR_TICK2MSEC(cpu_data.idle);
        cpu->wait  = SIGAR_TICK2MSEC(cpu_data.wait);
        cpu->irq = 0; /*N/A*/
        cpu->soft_irq = 0; /*N/A*/
        cpu->stolen = 0; /*N/A*/
        cpu->total = cpu->user + cpu->sys + cpu->idle + cpu->wait;
        return SIGAR_OK;
    }
    else {
        return errno;
    }
}

/*
 * other possible metrics we could add:
 * struct cpuinfo {
 *       long    cpu[CPU_NTIMES];
 *       long    pswitch;
 *       long    syscall;
 *       long    sysread;
 *       long    syswrite;
 *       long    sysfork;
 *       long    sysexec;
 *       long    readch;
 *       long    writech;
 *       long    iget;
 *       long    namei;
 *       long    dirblk;
 *       long    msg;
 *       long    sema;
 *       long    bread;
 *       long    bwrite;
 *       long    lread;
 *       long    lwrite;
 *       long    phread;
 *       long    phwrite;
 * };
 */


static int boot_time(sigar_t *sigar, time_t *time)
{
    int fd;
    struct utmp data;

    if ((fd = open(UTMP_FILE, O_RDONLY)) < 0) {
        return errno;
    }

    do {
        if (read(fd, &data, sizeof(data)) != sizeof(data)) {
            int status = errno;
            close(fd);
            return status;
        }
    } while (data.ut_type != BOOT_TIME);

    *time = data.ut_time;

    close(fd);

    return SIGAR_OK;
}

#define WHOCPY(dest, src) \
    SIGAR_SSTRCPY(dest, src); \
    if (sizeof(src) < sizeof(dest)) \
        dest[sizeof(dest)-1] = '\0'

static int sigar_who_utmp(sigar_t *sigar,
                          sigar_who_list_t *wholist)
{
    struct utmp ut;
    FILE *fp;

    if (!(fp = fopen(UTMP_FILE, "r"))) {
        return errno;
    }

    while (fread(&ut, sizeof(ut), 1, fp) == 1) {
        sigar_who_t *who;

        if (*ut.ut_name == '\0') {
            continue;
        }

        if (ut.ut_type != USER_PROCESS) {
            continue;
        }

        SIGAR_WHO_LIST_GROW(wholist);
        who = &wholist->data[wholist->number++];

        WHOCPY(who->user, ut.ut_user);
        WHOCPY(who->device, ut.ut_line);
        WHOCPY(who->host, ut.ut_host);

        who->time = ut.ut_time;
    }

    fclose(fp);

    return SIGAR_OK;
}

int sigar_os_proc_list_get(sigar_t *sigar,
                           sigar_proc_list_t *proclist)
{
    pid_t pid = 0;
    struct procsinfo info;

    for (;;) {
        int num = getprocs(&info, sizeof(info),
                           NULL, 0, &pid, 1);

        if (num == 0) {
            break;
        }

        SIGAR_PROC_LIST_GROW(proclist);

        proclist->data[proclist->number++] = info.pi_pid;
    }

    return SIGAR_OK;
}

static int sigar_getprocs(sigar_t *sigar, sigar_pid_t pid)
{
    int status, num;
    time_t timenow = time(NULL);

    if (sigar->pinfo == NULL) {
        sigar->pinfo = malloc(sizeof(*sigar->pinfo));
    }

    if (sigar->last_pid == pid) {
        if ((timenow - sigar->last_getprocs) < SIGAR_LAST_PROC_EXPIRE) {
            return SIGAR_OK;
        }
    }

    sigar->last_pid = pid;
    sigar->last_getprocs = timenow;

    num = getprocs(sigar->pinfo, sizeof(*sigar->pinfo),
                   NULL, 0, &pid, 1);

    if (num != 1) {
        return ESRCH;
    }

    return SIGAR_OK;
}

int sigar_proc_mem_get(sigar_t *sigar, sigar_pid_t pid,
                       sigar_proc_mem_t *procmem)
{
    int status = sigar_getprocs(sigar, pid);
    struct procsinfo64 *pinfo = sigar->pinfo;

    if (status != SIGAR_OK) {
        return status;
    }

    procmem->size  = PAGESHIFT(pinfo->pi_size); /* XXX fold in pi_dvm ? */
    procmem->share = PAGESHIFT(pinfo->pi_sdsize);
    procmem->resident = PAGESHIFT(pinfo->pi_drss + pinfo->pi_trss);

    procmem->minor_faults = pinfo->pi_minflt;
    procmem->major_faults = pinfo->pi_majflt;
    procmem->page_faults =
        procmem->minor_faults +
        procmem->major_faults;

    return SIGAR_OK;
}

int sigar_proc_time_get(sigar_t *sigar, sigar_pid_t pid,
                        sigar_proc_time_t *proctime)
{
    int status = sigar_getprocs(sigar, pid);
    struct procsinfo64 *pinfo = sigar->pinfo;

    if (status != SIGAR_OK) {
        return status;
    }

    proctime->start_time = pinfo->pi_start;
    proctime->start_time *= SIGAR_MSEC; /* convert to ms */
    proctime->user = pinfo->pi_utime * SIGAR_MSEC;
    proctime->sys  = pinfo->pi_stime * SIGAR_MSEC;
    proctime->total = proctime->user + proctime->sys;

    return SIGAR_OK;
}

int sigar_proc_state_get(sigar_t *sigar, sigar_pid_t pid,
                         sigar_proc_state_t *procstate)
{
    int status = sigar_getprocs(sigar, pid);
    struct procsinfo64 *pinfo = sigar->pinfo;
    tid_t tid = 0;
    struct thrdsinfo64 thrinfo;

    if (status != SIGAR_OK) {
        return status;
    }

    if (getthrds(pid, &thrinfo, sizeof(thrinfo), &tid, 1) == 1) {
        procstate->processor = thrinfo.ti_affinity;
    }
    else {
        procstate->processor = SIGAR_FIELD_NOTIMPL;
    }

    SIGAR_SSTRCPY(procstate->name, pinfo->pi_comm);
    procstate->ppid = pinfo->pi_ppid;
    procstate->nice = pinfo->pi_nice;
    procstate->tty  = pinfo->pi_ttyd;
    procstate->priority = pinfo->pi_pri;
    procstate->threads = pinfo->pi_thcount;

    switch (pinfo->pi_state) {
      case SACTIVE:
        procstate->state = 'R';
        break;
      case SIDL:
        procstate->state = 'D';
        break;
      case SSTOP:
        procstate->state = 'S';
        break;
      case SZOMB:
        procstate->state = 'Z';
        break;
      case SSWAP:
        procstate->state = 'S';
        break;
    }

    return SIGAR_OK;
}

static int sigar_proc_modules_local_get(sigar_t *sigar,
                                        sigar_proc_modules_t *procmods)
{
    struct ld_info *info;
    char *buffer;
    int size = 2048, status;
    unsigned int offset;

    buffer = malloc(size);
    while ((loadquery(L_GETINFO, buffer, size) == -1) &&
           (errno == ENOMEM))
    {
        size += 2048;
        buffer = realloc(buffer, size);
    }

    info = (struct ld_info *)buffer;

    do {
        char *name = info->ldinfo_filename;

        status =
            procmods->module_getter(procmods->data, name, strlen(name));

        if (status != SIGAR_OK) {
            /* not an error; just stop iterating */
            free(buffer);
            return status;
        }

        offset = info->ldinfo_next;
        info = (struct ld_info *)((char*)info + offset);
    } while(offset);

    free(buffer);

    return SIGAR_OK;
}


#define SIGAR_MICROSEC2NANO(s) \
    ((sigar_uint64_t)(s) * (sigar_uint64_t)1000)

#define TIME_NSEC(t) \
    (SIGAR_SEC2NANO((t).tv_sec) + SIGAR_MICROSEC2NANO((t).tv_usec))


int sigar_os_fs_type_get(sigar_file_system_t *fsp)
{
    return fsp->type;
}

#ifndef MNT_NFS4
/* another one documented in aix tech ref
 * with no friggin prototype in any header file...
 * ...but added in 5.2
 */
int mntctl(int command, int size, char *buffer);
#endif

int sigar_file_system_list_get(sigar_t *sigar,
                               sigar_file_system_list_t *fslist)
{
    int i, size, num;
    char *buf, *mntlist;

    /* get required size */
    if (mntctl(MCTL_QUERY, sizeof(size), (char *)&size) < 0) {
        return errno;
    }

    mntlist = buf = malloc(size);

    if ((num = mntctl(MCTL_QUERY, size, buf)) < 0) {
        free(buf);
        return errno;
    }

    sigar_file_system_list_create(fslist);

    for (i=0; i<num; i++) {
        char *devname;
        const char *typename = NULL;
        sigar_file_system_t *fsp;
        struct vmount *ent = (struct vmount *)mntlist;

        mntlist += ent->vmt_length;

        SIGAR_FILE_SYSTEM_LIST_GROW(fslist);

        fsp = &fslist->data[fslist->number++];

        switch (ent->vmt_gfstype) {
          case MNT_AIX:
            typename = "aix";
            fsp->type = SIGAR_FSTYPE_LOCAL_DISK;
            break;
          case MNT_JFS:
            typename = "jfs";
            fsp->type = SIGAR_FSTYPE_LOCAL_DISK;
            break;
          case MNT_NFS:
          case MNT_NFS3:
            typename = "nfs";
            fsp->type = SIGAR_FSTYPE_NETWORK;
            break;
          case MNT_CDROM:
            fsp->type = SIGAR_FSTYPE_CDROM;
            break;
          case MNT_SFS:
          case MNT_CACHEFS:
          case MNT_AUTOFS:
          default:
            if (ent->vmt_flags & MNT_REMOTE) {
                fsp->type = SIGAR_FSTYPE_NETWORK;
            }
            else {
                fsp->type = SIGAR_FSTYPE_NONE;
            }
        }

        SIGAR_SSTRCPY(fsp->dir_name, vmt2dataptr(ent, VMT_STUB));
        SIGAR_SSTRCPY(fsp->options, vmt2dataptr(ent, VMT_ARGS));

        devname = vmt2dataptr(ent, VMT_OBJECT);

        if (fsp->type == SIGAR_FSTYPE_NETWORK) {
            char *hostname   = vmt2dataptr(ent, VMT_HOSTNAME);
#if 0
            /* XXX: these do not seem reliable */
            int hostname_len = vmt2datasize(ent, VMT_HOSTNAME)-1; /* -1 == skip '\0' */
            int devname_len  = vmt2datasize(ent, VMT_OBJECT);     /* includes '\0' */
#else
            int hostname_len = strlen(hostname);
            int devname_len = strlen(devname) + 1;
#endif
            int total_len    = hostname_len + devname_len + 1;    /* 1 == strlen(":") */

            if (total_len > sizeof(fsp->dev_name)) {
                /* justincase - prevent overflow.  chances: slim..none */
                SIGAR_SSTRCPY(fsp->dev_name, devname);
            }
            else {
                /* sprintf(fsp->devname, "%s:%s", hostname, devname) */
                char *ptr = fsp->dev_name;

                memcpy(ptr, hostname, hostname_len);
                ptr += hostname_len;

                *ptr++ = ':';

                memcpy(ptr, devname, devname_len);
            }
        }
        else {
            SIGAR_SSTRCPY(fsp->dev_name, devname);
        }

        /* we set fsp->type, just looking up sigar.c:fstype_names[type] */
        sigar_fs_type_get(fsp);

        if (typename == NULL) {
            typename = fsp->type_name;
        }

        SIGAR_SSTRCPY(fsp->sys_type_name, typename);
    }

    free(buf);

    return SIGAR_OK;
}

typedef struct {
    char name[IDENTIFIER_LENGTH];
    long addr;
} aix_diskio_t;

static int create_diskmap(sigar_t *sigar)
{
    int i, total, num;
    perfstat_disk_t *disk;
    perfstat_id_t id;

    total = perfstat_disk(NULL, NULL, sizeof(*disk), 0);
    if (total < 1) {
        return ENOENT;
    }

    disk = malloc(total * sizeof(*disk));
    id.name[0] = '\0';

    num = perfstat_disk(&id, disk, sizeof(*disk), total);
    if (num < 1) {
        free(disk);
        return ENOENT;
    }

    sigar->diskmap = sigar_cache_new(25);

    odm_initialize();

    for (i=0; i<num; i++) {
        char query[256];
        struct CuDv *dv, *ptr;
        struct listinfo info;
        sigar_cache_entry_t *ent;
        int j;

        snprintf(query, sizeof(query),
                 "parent = '%s'", disk[i].vgname);

        ptr = dv = odm_get_list(CuDv_CLASS, query, &info, 256, 1);
        if ((int)dv == -1) {
            continue; /* XXX */
        }

        for (j=0; j<info.num; j++, ptr++) {
            struct CuAt *attr;
            int num, retval;
            struct stat sb;

            if ((attr = getattr(ptr->name, "label", 0, &num))) {
                retval = stat(attr->value, &sb);

                if (retval == 0) {
                    aix_diskio_t *diskio = malloc(sizeof(*diskio));
                    SIGAR_SSTRCPY(diskio->name, disk[i].name);
                    diskio->addr = -1;
                    ent = sigar_cache_get(sigar->diskmap, SIGAR_FSDEV_ID(sb));
                    ent->value = diskio;
                }

                free(attr);
            }
        }

        odm_free_list(dv, &info);
    }

    free(disk);
    odm_terminate();

    return SIGAR_OK;
}


/* from sys/systemcfg.h, not defined in 4.3 headers */
#ifndef POWER_4
#define POWER_4		0x0800
#endif
#ifndef POWER_MPC7450
#define POWER_MPC7450	0x1000
#endif
#ifndef POWER_5
#define POWER_5		0x2000
#endif

static char *sigar_get_odm_model(sigar_t *sigar)
{
    if (sigar->model[0] == '\0') {
        struct CuAt *odm_obj;
        int num;

        odm_initialize();

        if ((odm_obj = getattr("proc0", "type", 0, &num))) {
            SIGAR_SSTRCPY(sigar->model, odm_obj->value);
            free(odm_obj);
        }

        odm_terminate();
    }

    return sigar->model;
}

#define SIGAR_CPU_CACHE_SIZE \
  (_system_configuration.L2_cache_size / 1024)

static int sigar_get_cpu_mhz(sigar_t *sigar)
{
    if (sigar->cpu_mhz == SIGAR_FIELD_NOTIMPL) {
        perfstat_cpu_total_t data;

        if (sigar_perfstat_cpu(&data) == 1) {
            sigar->cpu_mhz = data.processorHZ / 1000000;
        }
        else {
            sigar_log_printf(sigar, SIGAR_LOG_ERROR,
                             "perfstat_cpu_total failed: %s",
                             sigar_strerror(sigar, errno));
        }
    }

    return sigar->cpu_mhz;
}

static char *get_cpu_arch(void)
{
    switch (_system_configuration.architecture) {
        case POWER_RS:
            return "Power Classic";
        case POWER_PC:
            return "PowerPC";
        case IA64:
            return "IA64";
        default:
            return "PowerPC"; /* what else could it be */
    }
}

static char *get_ppc_cpu_model(void)
{
    switch (_system_configuration.implementation) {
        case POWER_RS1:
            return "RS1";
        case POWER_RSC:
            return "RSC";
        case POWER_RS2:
            return "RS2";
        case POWER_601:
            return "601";
        case POWER_603:
            return "603";
        case POWER_604:
            return "604";
        case POWER_620:
            return "620";
        case POWER_630:
            return "630";
        case POWER_A35:
            return "A35";
        case POWER_RS64II:
            return "RS64-II";
        case POWER_RS64III:
            return "RS64-III";
        case POWER_4:
            return "POWER4";
        case POWER_MPC7450:
            return "MPC7450";
        case POWER_5:
            return "POWER5";
        default:
            return "Unknown";
    }
}

static char *get_ia64_cpu_model(void)
{
    switch (_system_configuration.implementation) {
        case IA64_M1:
            return "M1";
        case IA64_M2:
            return "M2";
        default:
            return "Unknown";
    }
}

static char *get_cpu_model(void)
{
    if (_system_configuration.architecture == IA64) {
        return get_ia64_cpu_model();
    }
    else {
        return get_ppc_cpu_model();
    }
}

/* XXX net_route_list copy-n-pasted from darwin_sigar.c; only diff is getkerninfo instead of sysctl */
#define rt_s_addr(sa) ((struct sockaddr_in *)(sa))->sin_addr.s_addr

#ifndef SA_SIZE
#define SA_SIZE(sa)                                             \
    (  (!(sa) || ((struct sockaddr *)(sa))->sa_len == 0) ?      \
        sizeof(long)            :                               \
        1 + ( (((struct sockaddr *)(sa))->sa_len - 1) | (sizeof(long) - 1) ) )
#endif


int sigar_net_interface_ipv6_config_get(sigar_t *sigar, const char *name,
                                        sigar_net_interface_config_t *ifconfig)
{
    int sock;
    struct in6_ifreq ifr;

    if ((sock = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
        return errno;
    }

    SIGAR_SSTRCPY(ifr.ifr_name, name);

    if (ioctl(sock, SIOCGIFADDR6, &ifr) == 0) {
        struct in6_addr *addr = SIGAR_SIN6_ADDR(&ifr.ifr_Addr);

        sigar_net_address6_set(ifconfig->address6, addr);
        sigar_net_interface_scope6_set(ifconfig, addr);

        if (ioctl(sock, SIOCGIFNETMASK6, &ifr) == 0) {
            addr = SIGAR_SIN6_ADDR(&ifr.ifr_Addr);
            ifconfig->prefix6_length = SIGAR_SIN6(&ifr.ifr_Addr)->sin6_len; /*XXX*/
        }
    }

    close(sock);
    return SIGAR_OK;
}

#define IS_TCP_SERVER(state, flags) \
    ((flags & SIGAR_NETCONN_SERVER) && (state == TCPS_LISTEN))

#define IS_TCP_CLIENT(state, flags) \
    ((flags & SIGAR_NETCONN_CLIENT) && (state != TCPS_LISTEN))

static int net_conn_get_tcp(sigar_net_connection_walker_t *walker)
{
    sigar_t *sigar = walker->sigar;
    int flags = walker->flags;
    int status;
    struct inpcb tcp_inpcb;
    struct tcpcb tcpcb;
    struct inpcb *entry;

    status = kread(sigar, &tcp_inpcb, sizeof(tcp_inpcb),
                   sigar->koffsets[KOFFSET_TCB]);

    if (status != SIGAR_OK) {
        return status;
    }

    entry = tcp_inpcb.inp_next;
    while (entry) {
        struct inpcb pcb;
        int state;

        status = kread(sigar, &pcb, sizeof(pcb), (long)entry);
        if (status != SIGAR_OK) {
            return status;
        }
        status = kread(sigar, &tcpcb, sizeof(tcpcb), (long)pcb.inp_ppcb);
        if (status != SIGAR_OK) {
            return status;
        }

        state = tcpcb.t_state;
        if ((IS_TCP_SERVER(state, flags) ||
             IS_TCP_CLIENT(state, flags)))
        {
            sigar_net_connection_t conn;

            SIGAR_ZERO(&conn);

            conn.type = SIGAR_NETCONN_TCP;

            sigar_net_address_set(conn.local_address,
                                  pcb.inp_laddr.s_addr);

            sigar_net_address_set(conn.remote_address,
                                  pcb.inp_faddr.s_addr);

            conn.local_port  = ntohs(pcb.inp_lport);
            conn.remote_port = ntohs(pcb.inp_fport);

            conn.send_queue = conn.receive_queue = SIGAR_FIELD_NOTIMPL;

            switch (state) {
              case TCPS_CLOSED:
                conn.state = SIGAR_TCP_CLOSE;
                break;
              case TCPS_LISTEN:
                conn.state = SIGAR_TCP_LISTEN;
                break;
              case TCPS_SYN_SENT:
                conn.state = SIGAR_TCP_SYN_SENT;
                break;
              case TCPS_SYN_RECEIVED:
                conn.state = SIGAR_TCP_SYN_RECV;
                break;
              case TCPS_ESTABLISHED:
                conn.state = SIGAR_TCP_ESTABLISHED;
                break;
              case TCPS_CLOSE_WAIT:
                conn.state = SIGAR_TCP_CLOSE_WAIT;
                break;
              case TCPS_FIN_WAIT_1:
                conn.state = SIGAR_TCP_FIN_WAIT1;
                break;
              case TCPS_CLOSING:
                conn.state = SIGAR_TCP_CLOSING;
                break;
              case TCPS_LAST_ACK:
                conn.state = SIGAR_TCP_LAST_ACK;
                break;
              case TCPS_FIN_WAIT_2:
                conn.state = SIGAR_TCP_FIN_WAIT2;
                break;
              case TCPS_TIME_WAIT:
                conn.state = SIGAR_TCP_TIME_WAIT;
                break;
              default:
                conn.state = SIGAR_TCP_UNKNOWN;
                break;
            }

            if (walker->add_connection(walker, &conn) != SIGAR_OK) {
                break;
            }
        }

        entry = pcb.inp_next;
        if (entry == tcp_inpcb.inp_next) {
            break;
        }
    }

    return SIGAR_OK;
}

int sigar_net_connection_walk(sigar_net_connection_walker_t *walker)
{
    int status;

    if (walker->flags & SIGAR_NETCONN_TCP) {
        status = net_conn_get_tcp(walker);

        if (status != SIGAR_OK) {
            return status;
        }
    }
#if 0
    if (walker->flags & SIGAR_NETCONN_UDP) {
        status = net_conn_get_udp(walker);

        if (status != SIGAR_OK) {
            return status;
        }
    }
#endif
    return SIGAR_OK;
}
