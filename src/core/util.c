#include "util.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int read_text_file(const char *path, char *buf, int size)
{
	int fd, n;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, buf, size - 1);
	close(fd);
	if (n < 0)
		return -1;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\0'))
		n--;
	buf[n] = '\0';
	return n;
}

int read_int_file(const char *path, int *val)
{
	char buf[32], *end;
	long v;

	if (read_text_file(path, buf, sizeof(buf)) <= 0)
		return -1;
	v = strtol(buf, &end, 10);
	if (end == buf)
		return -1;
	*val = (int)v;
	return 0;
}

int write_int_file(const char *path, int val)
{
	char buf[32];
	int fd, n, ret;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = snprintf(buf, sizeof(buf), "%d\n", val);
	ret = write(fd, buf, n) == n ? 0 : -1;
	close(fd);
	return ret;
}
