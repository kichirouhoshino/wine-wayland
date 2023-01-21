/* WAYLANDDRV Vulkan remote implementation
 *
 * Copyright 2022 Leandro Ribeiro
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include "waylanddrv.h"

#include "wine/debug.h"

#define VK_NO_PROTOTYPES
#define WINE_VK_HOST

#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"

#include "vulkan_remote.h"

#include <drm_fourcc.h>
#include <stdlib.h>

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

struct vk_funcs
{
    PFN_vkCreateImage p_vkCreateImage;
    PFN_vkDestroyImage p_vkDestroyImage;
    PFN_vkAllocateMemory p_vkAllocateMemory;
    PFN_vkFreeMemory p_vkFreeMemory;
    PFN_vkBindImageMemory p_vkBindImageMemory;
    PFN_vkGetImageMemoryRequirements p_vkGetImageMemoryRequirements;
    PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties;
    PFN_vkImportSemaphoreFdKHR p_vkImportSemaphoreFdKHR;
    PFN_vkImportFenceFdKHR p_vkImportFenceFdKHR;
    PFN_vkGetMemoryFdKHR p_vkGetMemoryFdKHR;
    PFN_vkGetImageSubresourceLayout p_vkGetImageSubresourceLayout;
};

struct wayland_remote_vk_image
{
    VkImage native_vk_image;
    VkDeviceMemory native_vk_image_memory;
    VkFormat format;
    uint32_t width, height;
    BOOL busy;
    struct wayland_native_buffer native_buffer;
    HANDLE remote_buffer_released_event;
};

struct wayland_remote_vk_swapchain
{
    struct vk_funcs vk_funcs;
    struct wayland_remote_surface_proxy *remote_surface_proxy;
    uint32_t count_images;
    struct wayland_remote_vk_image *images;
    enum wayland_remote_buffer_commit buffer_commit;
    HANDLE remote_throttle_event;
};

struct drm_vk_format
{
    VkFormat vk_format;
    VkFormat vk_format_srgb;
    uint32_t drm_format;
    uint32_t drm_format_alpha;
};

/* List of Vulkan formats that we know, and the corresponding DRM formats */
const static struct drm_vk_format format_table[] =
{
    {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, DRM_FORMAT_XBGR8888, DRM_FORMAT_ABGR8888},
    {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888},
};

/* Convert timeout in ms to the timeout format used by ntdll which is:
 * 100ns units, negative for monotonic time. */
static inline LARGE_INTEGER *get_nt_timeout(LARGE_INTEGER *time, int timeout_ms)
{
    if (timeout_ms == -1)
        return NULL;

    time->QuadPart = (ULONGLONG)timeout_ms * -10000;

    return time;
}

static UINT get_tick_count_since(UINT start)
{
    UINT now = NtGetTickCount();
    /* Handle tick count wrap around to zero. */
    if (now < start)
        return 0xffffffff - start + now + 1;
    else
        return now - start;
}

static uint32_t vulkan_format_to_drm_format(VkFormat format, BOOL ignore_alpha)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(format_table); i++)
    {
        const struct drm_vk_format *dvf = &format_table[i];
        if (dvf->vk_format == format || dvf->vk_format_srgb == format)
            return ignore_alpha ? dvf->drm_format : dvf->drm_format_alpha;
    }

    return DRM_FORMAT_INVALID;
}

static int wayland_native_buffer_init_vk(VkInstance instance, VkPhysicalDevice physical_device,
                                         VkDevice device, struct vk_funcs *vk_funcs,
                                         BOOL ignore_alpha, struct wayland_remote_vk_image *image)
{
    struct wayland_native_buffer *buffer = &image->native_buffer;
    VkMemoryGetFdInfoKHR memory_get_fd_info = {0};
    VkSubresourceLayout layout;
    VkImageSubresource image_subresource = {0};
    VkResult res;

    buffer->modifier = DRM_FORMAT_MOD_LINEAR;
    buffer->plane_count = 1;

    memory_get_fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    memory_get_fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    memory_get_fd_info.memory = image->native_vk_image_memory;

    res = vk_funcs->p_vkGetMemoryFdKHR(device, &memory_get_fd_info, &buffer->fds[0]);
    if (res != VK_SUCCESS)
    {
        buffer->fds[0] = -1;
        ERR("pfn_vkGetMemoryFdKHR failed, res=%d\n", res);
        goto err;
    }

    image_subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vk_funcs->p_vkGetImageSubresourceLayout(device, image->native_vk_image,
                                            &image_subresource, &layout);
    buffer->offsets[0] = layout.offset;
    buffer->strides[0] = layout.rowPitch;

    buffer->format = vulkan_format_to_drm_format(image->format, ignore_alpha);
    if (buffer->format == DRM_FORMAT_INVALID)
    {
        ERR("Failed to get corresponding DRM format for Vulkan format %d\n", image->format);
        goto err;
    }
    buffer->width = image->width;
    buffer->height = image->height;

    return 0;

err:
    wayland_native_buffer_deinit(buffer);
    ERR("Failed to init wayland_native_buffer for Vulkan image\n");
    return -1;
}

static int get_image_create_flags(VkSwapchainCreateInfoKHR *chain_create_info)
{
    uint32_t flags = 0;

    if (chain_create_info->flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR)
        flags |= VK_IMAGE_CREATE_PROTECTED_BIT;

    if (chain_create_info->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
        flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

    if (chain_create_info->flags & VK_SWAPCHAIN_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT_KHR)
        flags |= VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT;

    return flags;
}

static VkImage create_vulkan_image(VkDevice device, struct vk_funcs *vk_funcs,
                                   VkSwapchainCreateInfoKHR *chain_create_info)
{
    VkExternalMemoryImageCreateInfo external_memory_create_info = {0};
    VkImageCreateInfo image_create_info = {0};
    VkResult res;
    VkImage image;

    external_memory_create_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external_memory_create_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_create_info.pNext = &external_memory_create_info;
    image_create_info.imageType = VK_IMAGE_TYPE_2D;
    image_create_info.format = chain_create_info->imageFormat;
    image_create_info.extent.width = chain_create_info->imageExtent.width;
    image_create_info.extent.height = chain_create_info->imageExtent.height;
    image_create_info.extent.depth = 1;
    image_create_info.arrayLayers = chain_create_info->imageArrayLayers;
    image_create_info.sharingMode = chain_create_info->imageSharingMode;
    image_create_info.usage = chain_create_info->imageUsage;
    image_create_info.mipLevels = 1;
    image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    /* We'll create dma-buf buffers for these images, and so we'll need to know
     * the layout of them on memory. When VK_EXT_image_drm_format_modifier is
     * not supported, we can't use TILING_DRM_FORMAT_MODIFIER_EXT, so that
     * leaves us with TILING_LINEAR or TILING_OPTIMAL available. If we choose
     * TILING_OPTIMAL, we are not able to query the modifier chosen by the
     * driver and the number of planes (because we don't have the extension to
     * do so). So it'd be impossible to create dma-buf buffers. This leaves us
     * with TILING_LINEAR, and that makes drivers decisions predictable and we
     * can assume that they'll pick DRM_FORMAT_MOD_LINEAR and there'll be a
     * single plane. This might fail for drivers that do not support modifiers
     * at all, but we can't do better than that. */
    image_create_info.tiling = VK_IMAGE_TILING_LINEAR;
    image_create_info.flags = get_image_create_flags(chain_create_info);

    res = vk_funcs->p_vkCreateImage(device, &image_create_info, NULL, &image);
    if (res != VK_SUCCESS)
    {
        ERR("vkCreateImage failed, res=%d\n", res);
        goto err;
    }

    return image;

err:
    ERR("Failed to create Vulkan image\n");
    return VK_NULL_HANDLE;
}

static int get_memory_property_flags(VkSwapchainCreateInfoKHR *chain_create_info)
{
    uint32_t flags = 0;

    if (chain_create_info->flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR)
        flags |= VK_MEMORY_PROPERTY_PROTECTED_BIT;

    return flags;
}

static VkDeviceMemory create_vulkan_image_memory(VkInstance instance, VkPhysicalDevice physical_device,
                                                 VkDevice device, struct vk_funcs *vk_funcs,
                                                 VkSwapchainCreateInfoKHR *chain_create_info,
                                                 VkImage image)
{
    int32_t mem_type_index = -1;
    uint32_t flags;
    unsigned int i;
    VkMemoryRequirements mem_reqs;
    VkPhysicalDeviceMemoryProperties mem_props;
    VkExportMemoryAllocateInfo export_alloc_info = {0};
    VkMemoryAllocateInfo alloc_info = {0};
    VkResult res;
    VkDeviceMemory image_mem;

    export_alloc_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_alloc_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.pNext = &export_alloc_info;

    vk_funcs->p_vkGetImageMemoryRequirements(device, image, &mem_reqs);
    vk_funcs->p_vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);

    flags = get_memory_property_flags(chain_create_info);
    for (i = 0; i < mem_props.memoryTypeCount; i++)
        if ((mem_reqs.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & flags) == flags)
                mem_type_index = i;

    if (mem_type_index == -1)
    {
        ERR("Failed to find memoryTypeIndex\n");
        goto err;
    }

    alloc_info.memoryTypeIndex = mem_type_index;
    alloc_info.allocationSize = mem_reqs.size;

    res = vk_funcs->p_vkAllocateMemory(device, &alloc_info, NULL, &image_mem);
    if (res != VK_SUCCESS)
    {
        ERR("pfn_vkAllocateMemory failed, res=%d\n", res);
        goto err;
    }

    return image_mem;

err:
    ERR("Failed to create Vulkan image memory\n");
    return VK_NULL_HANDLE;
}

static void wayland_remote_vk_image_deinit(VkDevice device, struct vk_funcs *vk_funcs,
                                           struct wayland_remote_vk_image *image)
{
    vk_funcs->p_vkDestroyImage(device, image->native_vk_image, NULL);
    vk_funcs->p_vkFreeMemory(device, image->native_vk_image_memory, NULL);

    if (image->remote_buffer_released_event)
        NtClose(image->remote_buffer_released_event);

    wayland_native_buffer_deinit(&image->native_buffer);
}

static int wayland_remote_vk_image_init(VkInstance instance, VkPhysicalDevice physical_device,
                                        VkDevice device, struct vk_funcs *vk_funcs,
                                        VkSwapchainCreateInfoKHR *create_info,
                                        struct wayland_remote_vk_image *image)
{
    VkResult res;
    BOOL ignore_alpha;
    unsigned int i;

    image->native_vk_image = VK_NULL_HANDLE;
    image->native_vk_image_memory = VK_NULL_HANDLE;
    image->format = create_info->imageFormat;
    image->width = create_info->imageExtent.width;
    image->height = create_info->imageExtent.height;
    image->busy = FALSE;
    image->remote_buffer_released_event = 0;
    for (i = 0; i < ARRAY_SIZE(image->native_buffer.fds); i++)
        image->native_buffer.fds[i] = -1;

    image->native_vk_image = create_vulkan_image(device, vk_funcs, create_info);
    if (image->native_vk_image == VK_NULL_HANDLE)
        goto err;

    image->native_vk_image_memory =
        create_vulkan_image_memory(instance, physical_device, device,
                                   vk_funcs, create_info, image->native_vk_image);
    if (image->native_vk_image_memory == VK_NULL_HANDLE)
        goto err;

    res = vk_funcs->p_vkBindImageMemory(device, image->native_vk_image,
                                        image->native_vk_image_memory, 0);
    if (res != VK_SUCCESS)
    {
        ERR("pfn_vkBindImageMemory failed, res=%d\n", res);
        goto err;
    }

    ignore_alpha = create_info->compositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (wayland_native_buffer_init_vk(instance, physical_device, device, vk_funcs,
                                      ignore_alpha, image) < 0)
        goto err;

    return 0;

err:
    ERR("Failed to create remote swapchain image\n");
    wayland_remote_vk_image_deinit(device, vk_funcs, image);
    return -1;
}

static void wayland_remote_vk_image_release(struct wayland_remote_vk_image *image)
{
    if (image->remote_buffer_released_event)
    {
        NtClose(image->remote_buffer_released_event);
        image->remote_buffer_released_event = 0;
    }

    image->busy = FALSE;
}

void wayland_remote_vk_swapchain_destroy(struct wayland_remote_vk_swapchain *swapchain,
                                         VkDevice device)
{
    if (swapchain->remote_surface_proxy)
        wayland_remote_surface_proxy_destroy(swapchain->remote_surface_proxy);

    if (swapchain->images)
    {
        unsigned int i;
        for (i = 0; i < swapchain->count_images; i++)
        {
            wayland_remote_vk_image_deinit(device, &swapchain->vk_funcs,
                                           &swapchain->images[i]);
        }
        free(swapchain->images);
    }

    if (swapchain->remote_throttle_event)
        NtClose(swapchain->remote_throttle_event);

    free(swapchain);
}

struct wayland_remote_vk_swapchain *wayland_remote_vk_swapchain_create(HWND hwnd, VkInstance instance,
                                                                       VkPhysicalDevice physical_device,
                                                                       VkDevice device,
                                                                       const struct vulkan_funcs *vulkan_funcs,
                                                                       VkSwapchainCreateInfoKHR *create_info)
{
    static const uint32_t min_number_images = 4;
    struct wayland_remote_vk_swapchain *swapchain;
    unsigned int i;
    int res = 0;

    swapchain = calloc(1, sizeof(*swapchain));
    if (!swapchain)
    {
        ERR("Failed to allocate memory\n");
        goto err;
    }

#define LOAD_DEVICE_FUNCPTR(f) \
    if (!(swapchain->vk_funcs.p_##f = vulkan_funcs->p_vkGetDeviceProcAddr(device, #f))) \
        goto err

#define LOAD_INSTANCE_FUNCPTR(f) \
    if (!(swapchain->vk_funcs.p_##f = vulkan_funcs->p_vkGetInstanceProcAddr(instance, #f))) \
        goto err

    LOAD_DEVICE_FUNCPTR(vkCreateImage);
    LOAD_DEVICE_FUNCPTR(vkDestroyImage);
    LOAD_DEVICE_FUNCPTR(vkAllocateMemory);
    LOAD_DEVICE_FUNCPTR(vkFreeMemory);
    LOAD_DEVICE_FUNCPTR(vkBindImageMemory);
    LOAD_DEVICE_FUNCPTR(vkGetImageMemoryRequirements);
    LOAD_INSTANCE_FUNCPTR(vkGetPhysicalDeviceMemoryProperties);
    LOAD_DEVICE_FUNCPTR(vkImportSemaphoreFdKHR);
    LOAD_DEVICE_FUNCPTR(vkImportFenceFdKHR);
    LOAD_DEVICE_FUNCPTR(vkGetMemoryFdKHR);
    LOAD_DEVICE_FUNCPTR(vkGetImageSubresourceLayout);

#undef LOAD_DEVICE_FUNCPTR
#undef LOAD_INSTANCE_FUNCPTR

    swapchain->remote_surface_proxy =
        wayland_remote_surface_proxy_create(hwnd, WAYLAND_REMOTE_SURFACE_TYPE_GLVK);
    if (!swapchain->remote_surface_proxy)
    {
        ERR("Failed to create remote surface proxy for remote swapchain\n");
        goto err;
    }

    swapchain->count_images = max(create_info->minImageCount, min_number_images);
    swapchain->images = calloc(swapchain->count_images, sizeof(*swapchain->images));
    if (!swapchain->images)
    {
        ERR("Failed to allocate memory\n");
        goto err;
    }

    for (i = 0; i < swapchain->count_images; i++)
        res |= wayland_remote_vk_image_init(instance, physical_device, device,
                                            &swapchain->vk_funcs, create_info,
                                            &swapchain->images[i]);
    if (res < 0)
        goto err;

    swapchain->buffer_commit =
        (create_info->presentMode == VK_PRESENT_MODE_FIFO_KHR) ?
            WAYLAND_REMOTE_BUFFER_COMMIT_THROTTLED :
            WAYLAND_REMOTE_BUFFER_COMMIT_NORMAL;

    swapchain->remote_throttle_event = 0;

    return swapchain;

err:
    ERR("Failed to create remote swapchain\n");
    if (swapchain)
        wayland_remote_vk_swapchain_destroy(swapchain, device);
    return NULL;
}

VkResult wayland_remote_vk_swapchain_get_images(struct wayland_remote_vk_swapchain *swapchain,
                                                uint32_t *count, VkImage *images)
{
    unsigned int i;
    VkResult res = VK_SUCCESS;

    if (!images)
    {
        *count = swapchain->count_images;
        return VK_SUCCESS;
    }

    if (*count < swapchain->count_images)
        res = VK_INCOMPLETE;

    /* The client want us to fill images, but for some reason the size of the
     * array is larger than the number of formats that we support. So we correct
     * that size. */
    if (*count > swapchain->count_images)
        *count = swapchain->count_images;

    for (i = 0; i < (*count); i++)
        images[i] = swapchain->images[i].native_vk_image;

    return res;
}

static DWORD wait_remote_release_buffer_events(struct wayland_remote_vk_swapchain *swapchain,
                                               int timeout_ms)
{
    int count = 0;
    HANDLE *handles;
    struct wayland_remote_vk_image *image;
    struct wayland_remote_vk_image **images;
    unsigned int i;
    LARGE_INTEGER timeout;
    UINT ret = WAIT_OBJECT_0;

    handles = calloc(swapchain->count_images, sizeof(*handles));
    images = calloc(swapchain->count_images, sizeof(*images));
    if (!handles || !images)
    {
        ERR("Failed to allocate memory\n");
        ret = WAIT_FAILED;
        goto out;
    }

    if (!wayland_remote_surface_proxy_dispatch_events(swapchain->remote_surface_proxy))
    {
        ret = WAIT_FAILED;
        goto out;
    }

    for (i = 0; i < swapchain->count_images; i++)
    {
        image = &swapchain->images[i];
        if (!image->remote_buffer_released_event)
            continue;
        images[count] = image;
        handles[count] = image->remote_buffer_released_event;
        count++;
    }
    TRACE("count handles=%d\n", count);
    for (i = 0; i < count; i++)
        TRACE("handle%d=%p\n", i, handles[i]);

    /* Nothing to wait for, so just return */
    if (count == 0)
        goto out;

    ret = NtWaitForMultipleObjects(count, handles, TRUE, FALSE,
                                   get_nt_timeout(&timeout, timeout_ms));
    if (ret == WAIT_FAILED)
    {
        ERR("Failed on NtWaitForMultipleObjects() call, ret=%d\n", ret);
        goto out;
    }
    TRACE("count=%d => ret=%d\n", count, ret);

    i = ret - WAIT_OBJECT_0;
    if (i < count)
        wayland_remote_vk_image_release(images[i]);

out:
    if (ret == WAIT_FAILED)
        ERR("Failed to wait for remote release buffer event\n");
    free(handles);
    free(images);
    return ret;
}

VkResult wayland_remote_vk_swapchain_acquire_next_image(struct wayland_remote_vk_swapchain *swapchain,
                                                        VkDevice device, uint64_t timeout_ns,
                                                        VkSemaphore semaphore, VkFence fence,
                                                        uint32_t *image_index)
{
    struct vk_funcs *vk_funcs = &swapchain->vk_funcs;
    unsigned int i;
    BOOL free_image_found = FALSE;
    VkImportSemaphoreFdInfoKHR import_semaphore_fd_info = {0};
    VkImportFenceFdInfoKHR import_fence_fd_info = {0};
    VkResult res;
    static const UINT wait_timeout = 100;
    UINT wait_start = NtGetTickCount();

    /* As we are not the Vulkan driver, we don't have much information about the
     * semaphore. But the spec of VkImportSemaphoreFdInfoKHR states the
     * following:
     *
     * If handleType is VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, the
     * special value -1 for fd is treated like a valid sync file descriptor
     * referring to an object that has already signaled. The import operation
     * will succeed and the VkSemaphore will have a temporarily imported payload
     * as if a valid file descriptor had been provided.
     *
     * This special behavior allows us to signal the semaphore by setting
     * import_semaphore_fd_info.fd to -1. Same thing applies to VkFence, so we
     * set import_fence_fd_info.fd to -1 */

    import_semaphore_fd_info.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    import_semaphore_fd_info.fd = -1;
    import_semaphore_fd_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    import_semaphore_fd_info.semaphore = semaphore;
    import_semaphore_fd_info.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;

    import_fence_fd_info.sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR;
    import_fence_fd_info.fd = -1;
    import_fence_fd_info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    import_fence_fd_info.fence = fence;
    import_fence_fd_info.flags = VK_FENCE_IMPORT_TEMPORARY_BIT;

    /* Wait until we have a free image. If we don't get a free buffer within
     * wait_timeout, drop the first buffer to ensure we can continue and avoid
     * potential cross-process deadlocks (e.g., the render process waiting for
     * the window process to dispatch buffer release messages, while the window
     * process is waiting for the render process to finish rendering). */
    while (!free_image_found)
    {
        for (i = 0; i < swapchain->count_images; i++)
            if (!swapchain->images[i].busy)
            {
                free_image_found = TRUE;
                break;
            }

        if (!free_image_found)
        {
            /* If timeout is 0, the spec says that we should return VK_NOT_READY
             * when no images are available. */
            if (timeout_ns == 0)
                return VK_NOT_READY;

            if (wait_remote_release_buffer_events(swapchain, 10) == WAIT_FAILED)
                goto err;

            /* Release image so that we can continue */
            if (get_tick_count_since(wait_start) > wait_timeout)
            {
                i = 0;
                free_image_found = TRUE;
                wayland_remote_vk_image_release(&swapchain->images[i]);
            }
        }

        /* If applications defined a timeout, we must respect it */
        if (!free_image_found && timeout_ns > 0 &&
            get_tick_count_since(wait_start) > (timeout_ns / 1000000))
            return VK_TIMEOUT;
    }

    if (semaphore != VK_NULL_HANDLE)
    {
        res = vk_funcs->p_vkImportSemaphoreFdKHR(device, &import_semaphore_fd_info);
        if (res != VK_SUCCESS)
        {
            ERR("pfn_vkImportSemaphoreFdKHR failed, res=%d\n", res);
            goto err;
        }
    }
    if (fence != VK_NULL_HANDLE)
    {
        res = vk_funcs->p_vkImportFenceFdKHR(device, &import_fence_fd_info);
        if (res != VK_SUCCESS)
        {
            ERR("pfn_vkImportFenceFdKHR failed, res=%d\n", res);
            goto err;
        }
    }

    *image_index = i;
    swapchain->images[*image_index].busy = TRUE;

    return VK_SUCCESS;

err:
    ERR("Failed to acquire image from remote Vulkan swapchain");
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static DWORD wayland_remote_vk_swapchain_wait_throttle(struct wayland_remote_vk_swapchain *swapchain,
                                                       int timeout_ms)
{
    UINT ret;
    LARGE_INTEGER timeout;

    TRACE("remote_throttle_event=%p timeout_ms=%d\n",
          swapchain->remote_throttle_event, timeout_ms);

    if (!wayland_remote_surface_proxy_dispatch_events(swapchain->remote_surface_proxy))
    {
        ERR("Failed to dispatch remote events\n");
        return WAIT_FAILED;
    }

    ret = NtWaitForSingleObject(swapchain->remote_throttle_event, FALSE,
                                get_nt_timeout(&timeout, timeout_ms));
    if (ret == WAIT_OBJECT_0)
    {
        NtClose(swapchain->remote_throttle_event);
        swapchain->remote_throttle_event = 0;
    }

    TRACE("=> ret=%d\n", ret);
    return ret;
}

static void wayland_remote_vk_swapchain_throttle(struct wayland_remote_vk_swapchain *swapchain)
{
    static const UINT timeout = 100;
    UINT start, elapsed;

    start = NtGetTickCount();
    elapsed = 0;

    TRACE("throttle_event=%p\n", swapchain->remote_throttle_event);

    /* The compositor may at any time decide to not display the surface on
     * screen and thus not send any frame events. Until we have a better way to
     * deal with this, wait for a maximum of timeout for the frame event to
     * arrive, in order to avoid blocking the GL thread indefinitely. */
    while (elapsed < timeout && swapchain->remote_throttle_event &&
           wayland_remote_vk_swapchain_wait_throttle(swapchain, 10) != WAIT_FAILED)
    {
        elapsed = get_tick_count_since(start);
    }

    TRACE("throttle_event=%p => elapsed=%d\n",
          swapchain->remote_throttle_event, elapsed);

    if (swapchain->remote_throttle_event)
    {
        NtClose(swapchain->remote_throttle_event);
        swapchain->remote_throttle_event = 0;
    }
}

int wayland_remote_vk_swapchain_present(struct wayland_remote_vk_swapchain *swapchain,
                                        uint32_t image_index)
{
    struct wayland_remote_vk_image *image;

    image = &swapchain->images[image_index];
    image->busy = TRUE;

    if (swapchain->remote_throttle_event)
        wayland_remote_vk_swapchain_throttle(swapchain);

    if (!wayland_remote_surface_proxy_commit(swapchain->remote_surface_proxy,
                                             &image->native_buffer,
                                             WAYLAND_REMOTE_BUFFER_TYPE_DMABUF,
                                             swapchain->buffer_commit,
                                             &image->remote_buffer_released_event,
                                             &swapchain->remote_throttle_event))
    {
        wayland_remote_vk_image_release(image);
        goto err;
    }

    return 0;

err:
    ERR("Failed to present remote Vulkan swapchain\n");
    return -1;
}

VkResult wayland_remote_vk_filter_supported_formats(uint32_t *count_filtered_formats,
                                                    void *filtered_formats,
                                                    uint32_t count_formats_to_filter,
                                                    void *formats_to_filter,
                                                    size_t format_size,
                                                    size_t vk_surface_format_offset)
{
    VkFormat format;
    VkResult res = VK_SUCCESS;
    uint32_t count_intersect;
    unsigned int i;

    for (count_intersect = 0, i = 0; i < count_formats_to_filter; i++)
    {
        format = *(VkFormat *)((char *)formats_to_filter +
                               (i * format_size) + vk_surface_format_offset);
        if (vulkan_format_to_drm_format(format, FALSE) != DRM_FORMAT_INVALID)
        {
            if (filtered_formats)
            {
                if (count_intersect < *count_filtered_formats)
                {
                    /* There's room for a format on the array, so add it */
                    memcpy((char *)filtered_formats + (count_intersect * format_size),
                           (char *)formats_to_filter + (i * format_size),
                           format_size);
                }
                else
                {
                    /* Array size is not enough to accommodate all the formats
                     * we support, so we must return VK_INCOMPLETE */
                    res = VK_INCOMPLETE;
                    break;
                }
            }
            count_intersect++;
        }
    }

    /* We just set count_filtered_formats when the client want us to do that
     * (i.e., filtered_formats == NULL) and when the client want us to fill
     * filtered_formats but for some reason the size of the array is larger than
     * the number of formats that we support. */
    if (!filtered_formats || *count_filtered_formats > count_intersect)
        *count_filtered_formats = count_intersect;

    return res;
}
