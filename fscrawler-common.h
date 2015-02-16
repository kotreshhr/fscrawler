/*
  Copyright (c) 2015 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include "list.h"

struct options {
        int thread_count;
        int buffer_len;
        int throttle;
        int xattrs;
};

struct xdirent {
	struct list_head list;
	ino_t            xd_ino;
	struct stat      xd_stbuf;
	char             xd_name[NAME_MAX+1];
};

int
filter (struct stat *buf);
void
callback (struct xdirent *entries, int count);
int
fscrawl (char *brickpath, int (*filter) (struct stat *buf),
         void (*callback) (struct xdirent *entries, int count), 
         struct options *opt);
