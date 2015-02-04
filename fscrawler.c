/*
  Copyright (c) 2015 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#define _GNU_SOURCE

#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <attr/xattr.h>
#include <assert.h>

#include "list.h"

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

int DEBUG = 1;
int WORKERS = 0;

#define NEW(x) {                              \
        x = calloc (1, sizeof (typeof (*x))); \
        }

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


struct xwork {
	pthread_t        cthreads[THREAD_MAX]; /* crawler threads */
	int              count;
	int              idle;
	int              stop;

	struct dirjob    crawl;

	struct dirjob   *rootjob; /* to verify completion in xwork_fini() */

	pthread_mutex_t  mutex;
	pthread_cond_t   cond;
};


struct dirjob *
dirjob_ref (struct dirjob *job)
{
	pthread_spin_lock (&job->lock);
	{
		job->refcnt++;
	}
	pthread_spin_unlock (&job->lock);

	return job;
}


void
dirjob_free (struct dirjob *job)
{
	assert (list_empty (&job->list));

	pthread_spin_destroy (&job->lock);
	free (job->dirname);
	if (job->entries)
		free (job->entries);
	free (job);
}

void
dirjob_ret (struct dirjob *job, int err)
{
	int            ret = 0;
	int            refcnt = 0;
	struct dirjob *parent = NULL;

	pthread_spin_lock (&job->lock);
	{
		refcnt = --job->refcnt;
		job->ret = (job->ret || err);
	}
	pthread_spin_unlock (&job->lock);

	if (refcnt == 0) {
		ret = job->ret;

		if (ret)
			terr ("Failed: %s (%d)\n", job->dirname, ret);
		else
			tdbg ("Finished: %s\n", job->dirname);

		parent = job->parent;
		if (parent)
			dirjob_ret (parent, ret);

		dirjob_free (job);
		job = NULL;
	}
}


struct dirjob *
dirjob_new (const char *dir, struct dirjob *parent)
{
	struct dirjob *job = NULL;

	NEW(job);
	if (!job)
		return NULL;

	job->dirname = strdup (dir);
	if (!job->dirname) {
		free (job);
		return NULL;
	}

	INIT_LIST_HEAD(&job->list);
	INIT_LIST_HEAD(&job->files);
	INIT_LIST_HEAD(&job->dirs);
	pthread_spin_init (&job->lock, PTHREAD_PROCESS_PRIVATE);
	job->ret = 0;

	if (parent)
		job->parent = dirjob_ref (parent);

	job->refcnt = 1;

	return job;
}

void
xwork_addcrawl (struct xwork *xwork, struct dirjob *job)
{
	pthread_mutex_lock (&xwork->mutex);
	{
		list_add_tail (&job->list, &xwork->crawl.list);
		pthread_cond_broadcast (&xwork->cond);
	}
	pthread_mutex_unlock (&xwork->mutex);
}

int
xwork_add (struct xwork *xwork, const char *dir, struct dirjob *parent)
{
	struct dirjob *job = NULL;

	job = dirjob_new (dir, parent);
	if (!job)
		return -1;

	xwork_addcrawl (xwork, job);

	return 0;
}


struct dirjob *
xwork_pick (struct xwork *xwork, int block)
{
	struct dirjob *job = NULL;
	struct list_head *head = NULL;

	head = &xwork->crawl.list;

	pthread_mutex_lock (&xwork->mutex);
	{
		for (;;) {
			if (xwork->stop)
				break;

			if (!list_empty (head)) {
				job = list_entry (head->next, typeof(*job),
						  list);
				list_del_init (&job->list);
				break;
			}

			if (((xwork->count * 2) == xwork->idle) &&
			    list_empty (&xwork->crawl.list)) {
				/* no outstanding jobs, and no
				   active workers
				*/
				tdbg ("Jobless. Terminating\n");
				xwork->stop = 1;
				pthread_cond_broadcast (&xwork->cond);
				break;
			}

			if (!block)
				break;

			xwork->idle++;
			pthread_cond_wait (&xwork->cond, &xwork->mutex);
			xwork->idle--;
		}
	}
	pthread_mutex_unlock (&xwork->mutex);

	return job;
}

int
skip_name (const char *dirname, const char *name)
{
	if (strcmp (name, ".") == 0)
		return 1;

	if (strcmp (name, "..") == 0)
		return 1;

	if (strcmp (name, ".glusterfs") == 0)
		return 1;
/* TODO:
	if (strcmp (dirname, ".") == 0)
		// skip even/odd entries from replicas
		if ((dumbhash (name) % REPLICA) != (INDEX % REPLICA)) {
			tdbg ("Skipping ./%s\n", name);
			return 1;
		}
*/

        if (strcmp (name, "changelogs") == 0)
                return 1;

        if (strcmp (name, "health_check") == 0)
                return 1;

        if (strcmp (name, "indices") == 0)
                return 1;

        if (strcmp (name, "landfill") == 0)
                return 1;

	return 0;
}


int
skip_mode (struct stat *stat)
{
	if (S_ISREG (stat->st_mode) &&
	    ((stat->st_mode & 07777) == 01000) &&
	    stat->st_size == 0)
		/* linkfile */
		return 1;
	return 0;
}


struct xdirent {
	struct list_head list;
	ino_t            xd_ino;
	struct stat      xd_stbuf;
	char             xd_name[NAME_MAX+1];
};


int
skip_stat (struct dirjob *job, const char *name)
{
        if (job == NULL)
                return 0;

        if (strcmp (job->dirname, ".glusterfs") == 0) {
                tdbg ("Directly adding directories under .glusterfs "
                      "to global list: %s\n", name);
                return 1;
        }

        if (job->parent != NULL) {
                if (strcmp (job->parent->dirname, ".glusterfs") == 0) {
                        tdbg ("Directly adding directories under .glusterfs/XX "
                              "to global list: %s\n", name);
                        return 1;
                }
        }

        return 0;
}

struct buf_accounting {
        pthread_spinlock_t alloc_lock;
        struct xdirent *entries; 
        int allocated;
        int count;
}buf_accnt;

int call_back (struct xdirent *entries, int count)
{
        static int call_count = 0;
        int i = 0;

        call_count++;

        printf ("*******HRKO CALL COUNT: %d ***********\n", call_count);
        for (i=0; i<count; i++)
                printf ("hrko_:%lu::%s\n", entries[i].xd_ino, entries[i].xd_name);
        free (entries);
        return 0;
}

int
xworker_do_crawl (struct xwork *xwork, struct dirjob *job)
{
	DIR            *dirp = NULL;
	int             ret = -1;
	int             boff;
	int             plen;
	struct dirent  *result;
	char            dbuf[512];
	char           *path = NULL;
        struct xdirent *copy_ptr = NULL;
        int             copy_count = NULL;
	struct xdirent *entries = NULL;
	struct xdirent *entry = NULL;
	struct xdirent *rentries = NULL;
	int             ecount = 0;
	int             esize = 0;
	int             i = 0;
	struct dirjob  *cjob = NULL;
//	int             filecnt = 0;
	int             dircnt = 0;
 //       struct stat     statbuf = {0,};
//	char            gfid_path[4096] = {0,};


	plen = strlen (job->dirname) + 256 + 2;
	path = alloca (plen);

	tdbg ("Entering: %s\n", job->dirname);

	dirp = opendir (job->dirname);
	if (!dirp) {
		terr ("opendir failed on %s (%s)\n", job->dirname,
		     strerror (errno));
		goto out;
	}


	for (;;) {
		ret = readdir_r (dirp, (struct dirent *)dbuf, &result);
		if (ret) {
			err ("readdir_r(%s): %s\n", job->dirname,
			     strerror (errno));
			goto out;
		}

		if (!result) /* EOF */
			break;

		if (result->d_ino == 0)
			continue;

		if (skip_name (job->dirname, result->d_name))
			continue;

                pthread_spin_lock(&buf_accnt.alloc_lock);
                {
                        if (!buf_accnt.allocated) {
                                buf_accnt.entries = calloc (5000, sizeof (*buf_accnt.entries));
                                if (buf_accnt.entries == NULL) {
                                        printf ("calloc failed: %s", strerror(errno)); 
                                        exit (1);
                                }
                                buf_accnt.allocated = 1;
                        }
                        buf_accnt.entries[buf_accnt.count].xd_ino = result->d_ino;
		        strncpy (buf_accnt.entries[buf_accnt.count].xd_name, result->d_name, NAME_MAX);
                        buf_accnt.count++;
                        if (buf_accnt.count == 5000) {
                                copy_ptr = buf_accnt.entries; 
                                copy_count = buf_accnt.count;
                                buf_accnt.allocated = 0;
                                buf_accnt.count = 0;
                        }
                }
                pthread_spin_unlock(&buf_accnt.alloc_lock);

                /* Once copied the pointer to local variable, sort it according
                 * inode and call call_back routine. */
          
                if (copy_ptr) {
        	        int xd_cmp (const void *a, const void *b)
	                {
		                const struct xdirent *xda = a;
		                const struct xdirent *xdb = b;

		                return (xda->xd_ino - xdb->xd_ino);
	                }

	                qsort (copy_ptr, copy_count, sizeof (*copy_ptr), xd_cmp);

                        /* Call registered callback */
                        call_back (copy_ptr, copy_count);
                        copy_ptr = NULL;
                        copy_count = 0;
                }
                
		if (!esize) {
			esize = 1024;
			entries = calloc (esize, sizeof (*entries));
			if (!entries) {
				err ("calloc failed\n");
				goto out;
			}
			job->entries = entries;
		} else if (esize == ecount) {
			esize += 1024;
			rentries = realloc (entries, esize * sizeof (*entries));
			if (!rentries) {
				err ("realloc failed\n");
				goto out;
			}
			entries = rentries;
			job->entries = entries;
		}
                
		entry = &entries[ecount];
		entry->xd_ino = result->d_ino;
		strncpy (entry->xd_name, result->d_name, NAME_MAX);
		INIT_LIST_HEAD (&entry->list);
		ecount++;
	}

	int xd_cmp (const void *a, const void *b)
	{
		const struct xdirent *xda = a;
		const struct xdirent *xdb = b;

		return (xda->xd_ino - xdb->xd_ino);
	}

	qsort (entries, ecount, sizeof (*entries), xd_cmp);

	boff = sprintf (path, "%s/", job->dirname);

	for (i = 0; i < ecount; i++) {
		entry = &entries[i];
		ret = fstatat (dirfd (dirp), entry->xd_name,
			       &entry->xd_stbuf,
			       AT_SYMLINK_NOFOLLOW);
		if (ret) {
			terr ("fstatat(%s): %s\n", path, strerror (errno));
			closedir (dirp);
			return -1;
		}

		if (S_ISDIR (entry->xd_stbuf.st_mode)) {
			dircnt++;
		}

		if (skip_mode (&entry->xd_stbuf))
			continue;

		if (S_ISDIR (entry->xd_stbuf.st_mode)) {
			list_add_tail (&entry->list, &job->dirs);
		//	BUMP(shortlist_dirs);
		}
	}

	job->dircnt = dircnt;

	//INC(encountered_dirs, dircnt);
	//INC(encountered_files, filecnt);

	list_for_each_entry (entry, &job->dirs, list) {
		strncpy (path + boff, entry->xd_name, (plen-boff));

		cjob = dirjob_new (path, job);
		if (!cjob) {
			err ("dirjob_new(%s): %s\n",
			     path, strerror (errno));
			ret = -1;
			goto out;
		}

		if (entry->xd_stbuf.st_nlink == 2) {
			/* leaf node */
			xwork_addcrawl (xwork, cjob);
		///	BUMP(encountered_leafs);
		} else {
			ret = xworker_do_crawl (xwork, cjob);
			dirjob_ret (cjob, ret);
			if (ret)
				goto out;
		}
	}

	ret = 0;
out:
	if (dirp)
		closedir (dirp);

//	BUMP(scanned_dirs);

	return ret;
}


void *
xworker_crawl (void *data)
{
	struct xwork *xwork = data;
	struct dirjob *job = NULL;
	int            ret = -1;

	while ((job = xwork_pick (xwork, 0))) {
		ret = xworker_do_crawl (xwork, job);
		dirjob_ret (job, ret);
	}

	return NULL;
}

int
xwork_fini (struct xwork *xwork, int stop)
{
	int i = 0;
	int ret = 0;
	void *tret = 0;

	pthread_mutex_lock (&xwork->mutex);
	{
		xwork->stop = (xwork->stop || stop);
		pthread_cond_broadcast (&xwork->cond);
	}
	pthread_mutex_unlock (&xwork->mutex);

	for (i = 0; i < xwork->count; i++) {
		pthread_join (xwork->cthreads[i], &tret);
		tdbg ("CThread id %ld returned %p\n",
		      xwork->cthreads[i], tret);
	}

        if (buf_accnt.count != 5000) {
                int xd_cmp (const void *a, const void *b)
                {
                        const struct xdirent *xda = a;
                        const struct xdirent *xdb = b;

                        return (xda->xd_ino - xdb->xd_ino);
                }

                qsort (buf_accnt.entries, buf_accnt.count, sizeof (*buf_accnt.entries), xd_cmp);
                call_back (buf_accnt.entries, buf_accnt.count);
        }

	if (DEBUG) {
		assert (xwork->rootjob->refcnt == 1);
		dirjob_ret (xwork->rootjob, 0);
	}

	return ret;
}


int
xwork_init (struct xwork *xwork, int count)
{
	int  i = 0;
	int  ret = 0;
	struct dirjob *rootjob = NULL;

	pthread_mutex_init (&xwork->mutex, NULL);
	pthread_cond_init (&xwork->cond, NULL);

	INIT_LIST_HEAD (&xwork->crawl.list);

	rootjob = dirjob_new (".", NULL);
	if (DEBUG)
		xwork->rootjob = dirjob_ref (rootjob);

	xwork_addcrawl (xwork, rootjob);

	xwork->count = count;
	for (i = 0; i < count; i++) {
		ret = pthread_create (&xwork->cthreads[i], NULL,
				      xworker_crawl, xwork);
		if (ret)
			break;
		tdbg ("Spawned crawler %d thread %ld\n", i,
		      xwork->cthreads[i]);
	}

	return ret;
}


int
xfind (const char *basedir)
{
	struct xwork xwork;
	int          ret = 0;
	char         *cwd = NULL;

	ret = chdir (basedir);
	if (ret) {
		err ("%s: %s\n", basedir, strerror (errno));
		return ret;
	}

	cwd = getcwd (0, 0);
	if (!cwd) {
		err ("getcwd(): %s\n", strerror (errno));
		return -1;
	}

	tdbg ("Working directory: %s\n", cwd);
	free (cwd);

	memset (&xwork, 0, sizeof (xwork));

	ret = xwork_init (&xwork, WORKERS);
	if (ret == 0)
		xworker_crawl (&xwork);

	ret = xwork_fini (&xwork, ret);
//	stats_dump ();

	return ret;
}

static char *
parse_and_validate_args (int argc, char *argv[])
{
	char        *basedir = NULL;
	struct stat  d = {0, };
	int          ret = -1;
	unsigned char volume_id[16];

	if (argc != 3) {
		err ("Usage: %s <DIR> <CRAWL-THREAD-COUNT>\n",
                      argv[0]);
		return NULL;
	}

	basedir = argv[1];
	ret = lstat (basedir, &d);
	if (ret) {
		err ("%s: %s\n", basedir, strerror (errno));
		return NULL;
	}

	ret = lgetxattr (basedir, "trusted.glusterfs.volume-id",
			 volume_id, 16);
	if (ret != 16) {
		err ("%s:Not a valid brick path.\n", basedir);
		return NULL;
	}


        WORKERS = atoi(argv[2]);
        if (WORKERS <= 0)
                WORKERS = DEFAULT_WORKERS;

	return basedir;
}

int
main (int argc, char *argv[])
{
	char *basedir = NULL;

        pthread_spin_init (&buf_accnt.alloc_lock, PTHREAD_PROCESS_PRIVATE);

	basedir = parse_and_validate_args (argc, argv);
	if (!basedir)
		return 1;

	xfind (basedir);

        pthread_spin_destroy (&buf_accnt.alloc_lock);

	return 0;
}
