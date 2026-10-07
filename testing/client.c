/* Transport only: JSON is parsed and materialized by the kernel module. */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    size_t used = 2, capacity = 4096;
    char *buf = malloc(capacity);
    int fd;
    ssize_t n;
    if (argc != 5) return 2;
    fd = open(argv[1], O_WRONLY);
    if (fd < 0) { perror("open endpoint"); return 126; }
    if (setgroups(0, NULL) || setgid(strtoul(argv[3], NULL, 10)) ||
        setuid(strtoul(argv[2], NULL, 10))) { perror("drop privileges"); return 126; }
    umask(strtoul(argv[4], NULL, 8));
    if (!buf) return 126;
    buf[0] = '.'; buf[1] = 0;
    for (;;) {
        if (used == capacity) {
            capacity *= 2;
            char *next = realloc(buf, capacity);
            if (!next) return 126;
            buf = next;
        }
        n = read(STDIN_FILENO, buf + used, capacity - used);
        if (n < 0) { if (errno == EINTR) continue; perror("read"); return 126; }
        if (!n) break;
        used += n;
    }
    n = write(fd, buf, used);
    if (n < 0) perror("json2dir");
    else if ((size_t)n != used) fprintf(stderr, "json2dir: short write\n");
    free(buf);
    close(fd);
    return n == (ssize_t)used ? 0 : 1;
}
