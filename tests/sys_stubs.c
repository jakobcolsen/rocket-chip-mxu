#include <sys/stat.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>

void _exit(int code) {
  while (1);
}

int _kill(int pid, int sig) {
  return -1;
}

int _getpid(void) {
  return 1;
}

ssize_t _write(int file, const void *ptr, size_t len) {
  return len;
}

ssize_t _read(int file, void *ptr, size_t len) {
  return 0;
}

void *_sbrk(ptrdiff_t incr) {
  return (void *)-1;
}

int _close(int file) {
  return -1;
}

int _fstat(int file, struct stat *st) {
  return -1;
}

int _isatty(int file) {
  return 0;
}

off_t _lseek(int file, off_t ptr, int dir) {
  return 0;
}

void handle_trap(uintptr_t cause, uintptr_t epc, uintptr_t regs[32]) {
    while(1);
}
