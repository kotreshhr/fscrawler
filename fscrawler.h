/*
  Copyright (c) 2015 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#ifndef _FSCRAWLER_H
#define _FSCRAWLER_H

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <attr/xattr.h>
#include <assert.h>

#include "fscrawler-common.h"

#define THREAD_MAX 32

#define err(x ...) fprintf(stderr, x)
#define out(x ...) fprintf(stdout, x)
#define dbg(x ...) do { if (DEBUG) fprintf(stdout, x); } while (0)
#define tout(x ...) do { out("[%ld] ", pthread_self()); out(x); } while (0)
#define terr(x ...) do { err("[%ld] ", pthread_self()); err(x); } while (0)
#define tdbg(x ...) do { dbg("[%ld] ", pthread_self()); dbg(x); } while (0)


#ifdef STATISTICS 
#define INC(name, val) do {				\
	if (!STATS)				        \
		break;					\
	pthread_spin_lock(&stats_lock);			\
	{						\
		stats_total.cnt_##name += val;		\
	}						\
	pthread_spin_unlock(&stats_lock);		\
	} while (0)


#define BUMP(name) INC(name, 1)
#endif

#define DEFAULT_WORKERS 4
#define DEFAULT_BUF_SIZE 5000

int DEBUG = 0;
int WORKERS = 0;


#define NEW(x) {                              \
        x = calloc (1, sizeof (typeof (*x))); \
        }

/*TODO: Conver variable types according to gluster code*/

struct dirjob {
	struct list_head    list;

	char               *dirname;

	struct dirjob      *parent;
	int                 ret;    /* final status of this subtree */
	int                 refcnt; /* how many dirjobs have this as parent */

	int                 filecnt;
	int                 dircnt;

	struct xdirent     *entries;
	struct list_head    files;  /* xdirents of shortlisted files */
	struct list_head    dirs;   /* xdirents of shortlisted dirs */

	pthread_spinlock_t  lock;
};

struct args {
        int (*filter) (struct stat *buf);
        void (*callback) (struct xdirent *entries, int count);
        struct options *opt;
};

struct xwork {
	pthread_t        cthreads[THREAD_MAX]; /* crawler threads */
	int              count;
        int              buf_len;
	int              idle;
	int              stop;

	struct dirjob    crawl;

	struct dirjob   *rootjob; /* to verify completion in xwork_fini() */

	pthread_mutex_t  mutex;
	pthread_cond_t   cond;
 
        int (*filter) (struct stat *buf);
        void (*callback) (struct xdirent *entries, int count);
};

struct buf_accounting {
        pthread_spinlock_t alloc_lock;
        struct xdirent *entries; 
        int allocated;
        int count;
}buf_accnt;

#endif /* _FSCRAWLER_H */
