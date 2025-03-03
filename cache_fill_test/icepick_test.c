#include <fcntl.h>
#include <linux/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEVICE "/dev/cachemem"
#define DATA_SIZE (256 * 1024)
#define ICEPICK_IOC_MAGIC 'k'
#define ICEPICK_FLUSH_CACHE _IOW(ICEPICK_IOC_MAGIC, 1, char *)

/*
 * simple test for `ICEPICK` character device by writing, reading, flushing cache data
 * @return : 0 on success or 1 on failure
 */
int main(void) {
  int fd, i;
  char input[32];
  char read_buf[DATA_SIZE];

  char *toy_data = malloc(DATA_SIZE);
  if (!toy_data) {
    perror("[ICEPICK] [!]    failed to allocate toy data\n");
    return 1;
  } else {
    printf("[ICEPICK] toy data allocated\n");
  }

  for (i = 0; i < DATA_SIZE; i++) {
    toy_data[i] = (char)(i % 256);
  }

  fd = open(DEVICE, O_RDWR);
  if (fd < 0) {
    perror("[ICEPICK] [!]    failed to open device\n");
    free(toy_data);
    return 1;
  } else {
    printf("[ICEPICK] opened character device: '/dev/cachemem' \n");
  }

  if (write(fd, toy_data, DATA_SIZE) < 0) {
    perror("[ICEPICK] [!]    failed to write toy data");
    close(fd);
    free(toy_data);
    return 1;
  }

  for (i = 0; i < 5; i++) {
    lseek(fd, 0, SEEK_SET);
    printf("[ICEPICK] reading data...\n");
    if (read(fd, read_buf, DATA_SIZE) < 0) {
      perror("[ICEPICK] [!]     failed to read toy data");
    } else if (memcmp(toy_data, read_buf, DATA_SIZE) == 0) {
      printf("[ICEPICK] read %d: data matches\n", i);
    } else {
      printf("[ICEPICK] [!]     read %d: data mismatch\n", i);
    }
    sleep(1);
  }

  printf("type 'please flush my cachelines' to flush: ");
  fgets(input, sizeof(input), stdin);
  input[strcspn(input, "\n")] = '\0';

  if (ioctl(fd, ICEPICK_FLUSH_CACHE, input) < 0) {
    perror("[ICEPICK] [!]    failed to flush cache lines");
  } else {
    printf("[ICEPICK] cache lines flushed\n");
  }

  close(fd);
  free(toy_data);
  return 0;
}
