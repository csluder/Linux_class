#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

int main(int argc, const char* argv[])
{
    const char* filepath = "/dev/rramjam";
    char buf[8192];
    char buf2[8192];
    int cnt = 0;

    int fd = open(filepath, O_RDWR, (mode_t)0600);

    if (fd == -1)
    {
        perror("Error opening file for writing");
        exit(EXIT_FAILURE);
    }

    char* map = mmap(0, (512 * 1024 * 1024), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED)
    {
        close(fd);
        perror("Error mmapping the file");
        exit(EXIT_FAILURE);
    }

    cnt = read(0, buf, sizeof(buf));
    memcpy(map, buf, cnt);

    memcpy(buf2, map, cnt);
    write(1, buf2, cnt);



    // Don't forget to free the mmapped memory
    if (munmap(map, (512 * 1024 * 1024)))
    {
        close(fd);
        perror("Error un-mmapping the file");
        exit(EXIT_FAILURE);
    }

    // Un-mmaping doesn't close the file, so we still need to do that.
    close(fd);

    return 0;
}
