#include "qemu/osdep.h"

#if !defined(__ANDROID_API__) || (__ANDROID_API__ < 36)
__attribute__((visibility("default")))
int pthread_getaffinity_np(pthread_t thread, size_t cpusetsize, cpu_set_t *cpuset);
__attribute__((visibility("default")))
int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize, const cpu_set_t *cpuset);
#endif

__attribute__((visibility("default")))
int pthread_getaffinity_np(pthread_t thread, size_t cpusetsize, cpu_set_t *cpuset)
{
    pid_t tid = pthread_gettid_np(thread);

    if (sched_getaffinity(tid, cpusetsize, cpuset) < 0) {
        return errno;
    }

    return 0;
}

__attribute__((visibility("default")))
int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize, const cpu_set_t *cpuset)
{
    pid_t tid = pthread_gettid_np(thread);

    if (sched_setaffinity(tid, cpusetsize, cpuset) < 0) {
        return errno;
    }

    return 0;
}
