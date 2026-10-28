/*
 * sweetdrm: the subset of the libdrm API that Mesa's freedreno (gallium driver,
 * winsys and src/freedreno/drm, msm backend) calls, for iOS, where there is no
 * DRM. Written for ios-sweet (plan Stage 7.0/7.1) from the call sites in Mesa
 * 26.2.5; the implementation (7.1) turns the msm ioctls into SweetGPU user
 * client calls. The kernel uapi structs and ioctl numbers come from Mesa's own
 * include/drm-uapi (MIT).
 */
#ifndef SWEETDRM_XF86DRM_H
#define SWEETDRM_XF86DRM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "drm-uapi/drm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRM_NODE_PRIMARY        0
#define DRM_NODE_CONTROL        1
#define DRM_NODE_RENDER         2
#define DRM_NODE_MAX            3

/* flags for drmPrimeHandleToFD; only passed through */
#ifndef DRM_CLOEXEC
#define DRM_CLOEXEC             0x00080000
#endif
#ifndef DRM_RDWR
#define DRM_RDWR                0x00000002
#endif

typedef struct _drmVersion {
	int version_major;
	int version_minor;
	int version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

/* Opens the SweetGPU user client; returns a descriptor-like handle (>= 0) or -1 */
int drmOpenWithType(const char *name, const char *busid, int type);

drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);

/* request is a full DRM_IOCTL_* number */
int drmIoctl(int fd, unsigned long request, void *arg);
/* index is a driver ioctl index (DRM_MSM_*), as in libdrm */
int drmCommandWrite(int fd, unsigned long index, void *data, unsigned long size);
int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size);

/* dma-buf export/import: not available, these fail with ENOSYS */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);

/* syncobjs: not available (DRM_CAP_SYNCOBJ reads 0), these fail with ENOSYS */
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);

/* BO mapping, used through os_mmap/os_munmap when SWEETDRM is defined */
void *sweetdrm_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int sweetdrm_munmap(void *addr, size_t length);

#ifdef __cplusplus
}
#endif

#endif
