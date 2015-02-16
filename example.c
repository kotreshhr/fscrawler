#include "fscrawler-common.h"

int
filter (struct stat *buf)
{
        /*NOTE: It is just dummy, as it is job of consumer to provide this
         *      callback. It just decides whether to include this file or
         *      not based on stat data.
         **/
        return 1;
}

/* call_back routine:
 *        The call_back function is called for every x number of entries
 *        found during crawling where x is variable passed as argument.
 */

void
call_back (struct xdirent *entries, int count)
{
        static int call_count = 0;
        int i = 0;

        call_count++;

        /* printf ("*******CALL COUNT: %d ***********\n", call_count); */
        for (i=0; i<count; i++)
                printf ("%s\n", entries[i].xd_name);
        free (entries);
        return;
}

int
main (int argc, char *argv[])
{
        struct options opt = {0,};

        opt.thread_count = 2;
        opt.buffer_len = 200;
        return fscrawl (argv[1], &filter, &call_back, &opt);
}
