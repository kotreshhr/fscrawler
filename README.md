# FSCrawler
  A glusterfs backend brick filesystem crawler.

# Compilation
 1. Generate position independent object code.
    -  gcc -o fscrawler.o -c fscrawler.c -Wall -Werror -fpic -O0 -g
 2. Make it as a dynamic shared library.
    -  gcc -shared -o libfscrawler.so fscrawler.o
 3. Link example.c with shared library to generate executable.
    -  gcc -o example example.c -L. -Wl,-rpath=. -lfscrawler -pthread -Wall -O0 -g
