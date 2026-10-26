// SPDX-License-Identifier: GPL-2.0-only
/*
 * Export the initialized UX3320B1 register image using the AOT4221NK
 * original Linux 5.4.55 BOSA ABI. Command 3 only reads registers under
 * the vendor semaphore; it writes the bank selector, not laser controls.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "This tool implements the AOT4221NK little-endian vendor ABI"
#endif

/* ioctl advertises a 4-byte argument, but the driver copies 0x2214 bytes. */
#define VENDOR_IOCTL	0x40044265UL
#define ARG_SIZE	0x2214
#define IMAGE_OFFSET	0x210
#define IMAGE_SIZE	0x300
#define LUT_SIZE	105

static bool uniform(const uint8_t *data, size_t size, uint8_t value)
{
	size_t i;

	for (i = 0; i < size; i++)
		if (data[i] != value)
			return false;
	return true;
}

int main(int argc, char **argv)
{
	uint8_t arg[ARG_SIZE] = {}, *image = arg + IMAGE_OFFSET;
	uint32_t command = 3;
	const char *device;
	size_t pos = 0;
	ssize_t n;
	int fd, ret;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "Usage: %s OUTPUT [DEVICE, default /dev/bosa]\n", argv[0]);
		return 2;
	}
	device = argc == 3 ? argv[2] : "/dev/bosa";
	memcpy(arg + 4, &command, sizeof(command));
	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(device);
		return 1;
	}
	ret = ioctl(fd, VENDOR_IOCTL, arg);
	if (ret < 0)
		perror("stock BOSA register dump");
	close(fd);
	if (ret < 0)
		return 1;
	if (image[0x100] != 0xaa || !memcmp(image + 0x175, "UX3320S", 7) ||
	    ((image[0x109] & 0xe0) != 0xc0 && (image[0x109] & 0xe0) != 0xe0) ||
	    uniform(image + 0x180, LUT_SIZE, 0) ||
	    uniform(image + 0x180, LUT_SIZE, 0xff) ||
	    uniform(image + 0x200, LUT_SIZE, 0) ||
	    uniform(image + 0x200, LUT_SIZE, 0xff)) {
		fprintf(stderr, "Response is not an initialized UX3320 register image\n");
		return 1;
	}
	fd = open(argv[1], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	while (pos < IMAGE_SIZE) {
		n = write(fd, image + pos, IMAGE_SIZE - pos);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			goto write_error;
		pos += n;
	}
	if (fsync(fd))
		goto write_error;
	if (close(fd)) {
		perror("close output");
		unlink(argv[1]);
		return 1;
	}
	printf("Saved %u calibrated register bytes to %s\n", IMAGE_SIZE, argv[1]);
	return 0;

write_error:
	perror("write output");
	close(fd);
	unlink(argv[1]);
	return 1;
}
