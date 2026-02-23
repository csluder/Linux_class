#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

#define OP_WRITE 1
#define OP_READ 2
int main(int argc, const char *argv[])
{    
	const char *filepath = "/dev/rramjam";
	char buf[8192];
	char buf2[8192];
	int cnt = 0;
	int operation = OP_READ | OP_WRITE;
	int offset = 0;
	int fd;


	if (argc > 1) {
		operation = atoi(argv[1]);
	}

	if (argc >2) {
		offset = atoi(argv[2]);
	}

	if (argc > 3) {
		cnt = atoi(argv[3]);
	} else {
		cnt = 8192;
	}



	fd = open(filepath, O_RDWR, (mode_t)0600);
    
	if (fd == -1)
	{
		perror("Error opening file for writing");
		exit(EXIT_FAILURE);
	}        
    
	char *map = mmap(0, (512*1024*1024), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
	{
		close(fd);
		perror("Error mmapping the file");
	        exit(EXIT_FAILURE);
    	}

	if (operation & OP_WRITE ) {
		cnt = read(0, buf, sizeof(buf));
		memcpy(map + offset, buf, cnt);
	}
    
	if (operation & OP_READ ) {
		memcpy(buf2, map + offset, cnt);
		write(1, buf2, cnt);
	}

    
    
	if (munmap(map, (512*1024*1024)))
	{
		close(fd);
		perror("Error un-mmapping the file");
		exit(EXIT_FAILURE);
	}

	close(fd);
    
	return 0;
}
