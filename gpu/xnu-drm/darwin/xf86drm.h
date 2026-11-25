/* SPDX-License-Identifier: MIT */
/*
 * The part of libdrm's xf86drm.h that Mesa's Venus driver (vn_renderer_virtgpu.c) uses, for macOS: libxnudrm implements
 * it over kexts/XnuDrm, which forwards to the Linux host's virtio-gpu render node (gpu/xnu-drm).
 */
#ifndef XNUDRM_XF86DRM_H
#define XNUDRM_XF86DRM_H
#include <stdint.h>
#include <stddef.h>
#include "drm-uapi/drm.h"
#ifdef __cplusplus
extern "C" {
#endif

#define DRM_NODE_PRIMARY 0
#define DRM_NODE_CONTROL 1
#define DRM_NODE_RENDER 2
#define DRM_NODE_MAX 3
#define DRM_BUS_PCI 0
#define DRM_BUS_USB 1
#define DRM_BUS_PLATFORM 2
#define DRM_BUS_HOST1X 3

typedef struct _drmVersion {
	int version_major, version_minor, version_patchlevel;
	int name_len; char *name;
	int date_len; char *date;
	int desc_len; char *desc;
} drmVersion, *drmVersionPtr;

typedef struct _drmPciBusInfo { uint16_t domain; uint8_t bus, dev, func; } drmPciBusInfo, *drmPciBusInfoPtr;
typedef struct _drmPciDeviceInfo {
	uint16_t vendor_id, device_id, subvendor_id, subdevice_id;
	uint8_t revision_id;
} drmPciDeviceInfo, *drmPciDeviceInfoPtr;
typedef struct _drmPlatformBusInfo { char fullname[512]; } drmPlatformBusInfo, *drmPlatformBusInfoPtr;
typedef struct _drmPlatformDeviceInfo { char **compatible; } drmPlatformDeviceInfo, *drmPlatformDeviceInfoPtr;

typedef struct _drmDevice {
	char **nodes;
	int available_nodes;
	int bustype;
	union { drmPciBusInfoPtr pci; drmPlatformBusInfoPtr platform; } businfo;
	union { drmPciDeviceInfoPtr pci; drmPlatformDeviceInfoPtr platform; } deviceinfo;
} drmDevice, *drmDevicePtr;

int drmIoctl(int fd, unsigned long request, void *arg);
int drmCommandRead(int fd, unsigned long drmCommandIndex, void *data, unsigned long size);
int drmCommandWrite(int fd, unsigned long drmCommandIndex, void *data, unsigned long size);
int drmCommandWriteRead(int fd, unsigned long drmCommandIndex, void *data, unsigned long size);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices);
void drmFreeDevices(drmDevicePtr devices[], int count);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr v);

int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
		   int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles,
			     uint64_t *points, uint32_t handle_count);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points,
			   unsigned num_handles, int64_t timeout_nsec,
			   unsigned flags, uint32_t *first_signaled);
int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count);
int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count, uint32_t flags);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
		       uint32_t src_handle, uint64_t src_point, uint32_t flags);

#ifdef __cplusplus
}
#endif
#endif
