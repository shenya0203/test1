// Implement __*time64 symbols by wrapping syscalls directly
// This avoids conflicts with musl's __REDIR macros and symbol versioning.
// We call the underlying syscalls directly instead of libc functions.

#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/select.h>

// Direct syscall wrappers - these implement the actual functionality
// without going through libc (which has the symbol redirection issues)

// Syscall: nanosleep (SYS_nanosleep)
// int nanosleep(const struct timespec *req, struct timespec *rem)
int __nanosleep_time64(const struct timespec *req, struct timespec *rem) {
    return syscall(SYS_nanosleep, req, rem);
}

// Syscall: select (SYS_select or pselect6)
// int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
int __select_time64(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    return syscall(SYS_select, nfds, readfds, writefds, exceptfds, timeout);
}

// Syscall: time (SYS_time)
// time_t time(time_t *tloc)
time_t __time64(time_t *tloc) {
    time_t result = syscall(SYS_time, tloc);
    return result;
}

// Syscall: gettimeofday (SYS_gettimeofday)
// int gettimeofday(struct timeval *tv, struct timezone *tz)
int __gettimeofday_time64(struct timeval *tv, struct timezone *tz) {
    return syscall(SYS_gettimeofday, tv, tz);
}

// For localtime, we need to read /etc/localtime or use environment
// This is more complex - we'll use the actual libc localtime but wrapped
// to avoid symbol issues
extern struct tm *localtime(const time_t *);
struct tm *__localtime64(const time_t *timep) {
    return localtime(timep);
}

// For mktime, we also need the actual libc implementation
extern time_t mktime(struct tm *);
time_t __mktime64(struct tm *tm) {
    return mktime(tm);
}

// Syscall: settimeofday (SYS_settimeofday)
// int settimeofday(const struct timeval *tv, const struct timezone *tz)
int __settimeofday_time64(const struct timeval *tv, const struct timezone *tz) {
    return syscall(SYS_settimeofday, tv, tz);
}

// For difftime, calculate manually from time_t values
// double difftime(time_t time1, time_t time0)
double __difftime64(time_t time1, time_t time0) {
    return (double)(time1 - time0);
}
