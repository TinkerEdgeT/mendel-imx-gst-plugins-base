/*
 * GStreamer
 * Copyright (C) 2012 Matthew Waters <ystree00@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gl/gl.h>
#if GST_GL_HAVE_PLATFORM_EGL && GST_GL_HAVE_DMABUF
#include <gst/gl/egl/gsteglimage.h>
#include <gst/allocators/gstdmabuf.h>
#endif

#include "gstgldownloadelement.h"

#if GST_GL_HAVE_PHYMEM
#include <gst/gl/gstglphymemory.h>
#endif

#if GST_GL_HAVE_IONDMA
#include <gst/gl/egl/gstglmemoryegl.h>
#include <gst/gl/gstglmemorydma.h>
#include <gst/allocators/gstionmemory.h>
#include <libdrm/drm_fourcc.h>
#endif

GST_DEBUG_CATEGORY_STATIC (gst_gl_download_element_debug);
#define GST_CAT_DEFAULT gst_gl_download_element_debug

#define gst_gl_download_element_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstGLDownloadElement, gst_gl_download_element,
    GST_TYPE_GL_BASE_FILTER,
    GST_DEBUG_CATEGORY_INIT (gst_gl_download_element_debug, "gldownloadelement",
        0, "download element"););

static gboolean gst_gl_download_element_get_unit_size (GstBaseTransform * trans,
    GstCaps * caps, gsize * size);
static GstCaps *gst_gl_download_element_transform_caps (GstBaseTransform * bt,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static gboolean gst_gl_download_element_set_caps (GstBaseTransform * bt,
    GstCaps * in_caps, GstCaps * out_caps);
static GstFlowReturn
gst_gl_download_element_prepare_output_buffer (GstBaseTransform * bt,
    GstBuffer * buffer, GstBuffer ** outbuf);
static GstFlowReturn gst_gl_download_element_transform (GstBaseTransform * bt,
    GstBuffer * buffer, GstBuffer * outbuf);
static gboolean gst_gl_download_element_propose_allocation (GstBaseTransform *
    bt, GstQuery * decide_query, GstQuery * query);
static gboolean gst_gl_download_element_decide_allocation (GstBaseTransform *
    trans, GstQuery * query);
static void gst_gl_download_element_finalize (GObject * object);

#if GST_GL_HAVE_IONDMA
static gboolean gst_gl_download_element_gl_start (GstGLBaseFilter * base);
static void gst_gl_download_element_gl_stop (GstGLBaseFilter * base);
static GstBuffer *gst_gl_download_element_export_teximage_dma (
    GstGLDownloadElement * dl, GstBuffer * inbuf);
#endif

#if GST_GL_HAVE_PLATFORM_EGL && GST_GL_HAVE_DMABUF
#define EXTRA_CAPS_TEMPLATE "video/x-raw(" GST_CAPS_FEATURE_MEMORY_DMABUF "); "
#else
#define EXTRA_CAPS_TEMPLATE
#endif

static GstStaticPadTemplate gst_gl_download_element_src_pad_template =
    GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (
        EXTRA_CAPS_TEMPLATE
        "video/x-raw; video/x-raw(memory:GLMemory)"));

static GstStaticPadTemplate gst_gl_download_element_sink_pad_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw(memory:GLMemory); video/x-raw"));

static void
gst_gl_download_element_class_init (GstGLDownloadElementClass * klass)
{
  GstBaseTransformClass *bt_class = GST_BASE_TRANSFORM_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
#if GST_GL_HAVE_IONDMA
  GstGLBaseFilterClass *gl_class = GST_GL_BASE_FILTER_CLASS (klass);
#endif

  bt_class->transform_caps = gst_gl_download_element_transform_caps;
  bt_class->set_caps = gst_gl_download_element_set_caps;
  bt_class->get_unit_size = gst_gl_download_element_get_unit_size;
  bt_class->prepare_output_buffer =
      gst_gl_download_element_prepare_output_buffer;
  bt_class->transform = gst_gl_download_element_transform;
  bt_class->decide_allocation = gst_gl_download_element_decide_allocation;
  bt_class->propose_allocation = gst_gl_download_element_propose_allocation;

  bt_class->passthrough_on_same_caps = TRUE;

#if GST_GL_HAVE_IONDMA
  gl_class->gl_start = gst_gl_download_element_gl_start;
  gl_class->gl_stop = gst_gl_download_element_gl_stop;
#endif

  gst_element_class_add_static_pad_template (element_class,
      &gst_gl_download_element_src_pad_template);
  gst_element_class_add_static_pad_template (element_class,
      &gst_gl_download_element_sink_pad_template);

  gst_element_class_set_metadata (element_class,
      "OpenGL downloader", "Filter/Video",
      "Downloads data from OpenGL", "Matthew Waters <matthew@centricular.com>");

  object_class->finalize = gst_gl_download_element_finalize;
}

static void
gst_gl_download_element_init (GstGLDownloadElement * download)
{
  gst_base_transform_set_prefer_passthrough (GST_BASE_TRANSFORM (download),
      TRUE);
}

static gboolean
gst_gl_download_element_set_caps (GstBaseTransform * bt, GstCaps * in_caps,
    GstCaps * out_caps)
{
  GstGLDownloadElement *dl = GST_GL_DOWNLOAD_ELEMENT (bt);
  GstVideoInfo out_info;
  GstCapsFeatures *features = NULL;

  if (!gst_video_info_from_caps (&out_info, out_caps))
    return FALSE;

  features = gst_caps_get_features (out_caps, 0);

  dl->do_pbo_transfers = FALSE;
  if (dl->dmabuf_allocator) {
    gst_object_unref (GST_OBJECT (dl->dmabuf_allocator));
    dl->dmabuf_allocator = NULL;
  }

  if (!features) {
    dl->do_pbo_transfers = TRUE;
    return TRUE;
  }

  if (gst_caps_features_contains (features, GST_CAPS_FEATURE_MEMORY_GL_MEMORY)) {
    /* do nothing with the buffer */
#if GST_GL_HAVE_PLATFORM_EGL && GST_GL_HAVE_DMABUF
  } else if (gst_caps_features_contains (features,
          GST_CAPS_FEATURE_MEMORY_DMABUF)) {
    dl->dmabuf_allocator = gst_dmabuf_allocator_new ();
#endif
  } else if (gst_caps_features_contains (features,
          GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY)) {
    dl->do_pbo_transfers = TRUE;
  }

  return TRUE;
}

static GstCaps *
_set_caps_features (const GstCaps * caps, const gchar * feature_name)
{
  GstCaps *tmp = gst_caps_copy (caps);
  guint n = gst_caps_get_size (tmp);
  guint i = 0;

  for (i = 0; i < n; i++)
    gst_caps_set_features (tmp, i,
        gst_caps_features_from_string (feature_name));

  return tmp;
}

static void
_remove_field (GstCaps * caps, const gchar * field)
{
  guint n = gst_caps_get_size (caps);
  guint i = 0;

  for (i = 0; i < n; i++) {
    GstStructure *s = gst_caps_get_structure (caps, i);
    gst_structure_remove_field (s, field);
  }
}

static GstCaps *
gst_gl_download_element_transform_caps (GstBaseTransform * bt,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstCaps *result, *tmp;

  if (direction == GST_PAD_SRC) {
    tmp = _set_caps_features (caps, GST_CAPS_FEATURE_MEMORY_GL_MEMORY);
    tmp = gst_caps_merge (gst_caps_ref (caps), tmp);
  } else {
    GstCaps *newcaps;
    tmp = gst_caps_ref (caps);

#if GST_GL_HAVE_PLATFORM_EGL && GST_GL_HAVE_DMABUF
    newcaps = _set_caps_features (caps, GST_CAPS_FEATURE_MEMORY_DMABUF);
    _remove_field (newcaps, "texture-target");
    tmp = gst_caps_merge (tmp, newcaps);
#endif

    newcaps = _set_caps_features (caps, GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY);
    _remove_field (newcaps, "texture-target");
    tmp = gst_caps_merge (tmp, newcaps);
  }

  if (filter) {
    result = gst_caps_intersect_full (filter, tmp, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (tmp);
  } else {
    result = tmp;
  }

  GST_DEBUG_OBJECT (bt, "returning caps %" GST_PTR_FORMAT, result);

  return result;
}

static gboolean
gst_gl_download_element_get_unit_size (GstBaseTransform * trans, GstCaps * caps,
    gsize * size)
{
  gboolean ret = FALSE;
  GstVideoInfo info;

  ret = gst_video_info_from_caps (&info, caps);
  if (ret)
    *size = GST_VIDEO_INFO_SIZE (&info);

  return TRUE;
}

#if GST_GL_HAVE_PLATFORM_EGL && GST_GL_HAVE_DMABUF

struct DmabufInfo
{
  GstMemory *dmabuf;
  gint stride;
  gsize offset;
};

static void
_free_dmabuf_info (struct DmabufInfo *info)
{
  gst_memory_unref (info->dmabuf);
  g_free (info);
}

static GQuark
_dmabuf_info_quark (void)
{
  static GQuark quark = 0;

  if (!quark)
    quark = g_quark_from_static_string ("GstGLDownloadDmabufInfo");
  return quark;
}

static struct DmabufInfo *
_get_cached_dmabuf_info (GstGLMemory * mem)
{
  return gst_mini_object_get_qdata (GST_MINI_OBJECT (mem),
      _dmabuf_info_quark ());
}

static void
_set_cached_dmabuf_info (GstGLMemory * mem, struct DmabufInfo *info)
{
  return gst_mini_object_set_qdata (GST_MINI_OBJECT (mem),
      _dmabuf_info_quark (), info, (GDestroyNotify) _free_dmabuf_info);
}

struct DmabufTransfer
{
  GstGLDownloadElement *download;
  GstGLMemory *glmem;
  struct DmabufInfo *info;
};

static void
_create_cached_dmabuf_info (GstGLContext * context, gpointer data)
{
  struct DmabufTransfer *transfer = (struct DmabufTransfer *) data;
  GstEGLImage *image;

  image = gst_egl_image_from_texture (context, transfer->glmem, NULL);
  if (image) {
    int fd;
    gint stride;
    gsize offset;

    if (gst_egl_image_export_dmabuf (image, &fd, &stride, &offset)) {
      GstGLDownloadElement *download = transfer->download;
      struct DmabufInfo *info;
      gsize maxsize;

      gst_memory_get_sizes (GST_MEMORY_CAST (transfer->glmem), NULL, &maxsize);

      info = g_new0 (struct DmabufInfo, 1);
      info->dmabuf =
          gst_dmabuf_allocator_alloc (download->dmabuf_allocator, fd, maxsize);
      info->stride = stride;
      info->offset = offset;

      transfer->info = info;
    }

    gst_egl_image_unref (image);
  }
}

static GstBuffer *
_try_export_dmabuf (GstGLDownloadElement * download, GstBuffer * inbuf)
{
  GstGLMemory *glmem;
  GstBuffer *buffer = NULL;
  int i;
  gsize offset[GST_VIDEO_MAX_PLANES];
  gint stride[GST_VIDEO_MAX_PLANES];
  GstCaps *src_caps;
  GstVideoInfo out_info;
  gsize total_offset;

  glmem = GST_GL_MEMORY_CAST (gst_buffer_peek_memory (inbuf, 0));
  if (glmem) {
    GstGLContext *context = GST_GL_BASE_MEMORY_CAST (glmem)->context;
    if (gst_gl_context_get_gl_platform (context) != GST_GL_PLATFORM_EGL)
      return NULL;
  }

  buffer = gst_buffer_new ();
  total_offset = 0;

  for (i = 0; i < gst_buffer_n_memory (inbuf); i++) {
    struct DmabufInfo *info;

    glmem = GST_GL_MEMORY_CAST (gst_buffer_peek_memory (inbuf, i));
    info = _get_cached_dmabuf_info (glmem);
    if (!info) {
      GstGLContext *context = GST_GL_BASE_MEMORY_CAST (glmem)->context;
      struct DmabufTransfer transfer;

      transfer.download = download;
      transfer.glmem = glmem;
      transfer.info = NULL;
      gst_gl_context_thread_add (context, _create_cached_dmabuf_info,
          &transfer);
      info = transfer.info;

      if (info)
        _set_cached_dmabuf_info (glmem, info);
    }

    if (info) {
      offset[i] = total_offset + info->offset;
      stride[i] = info->stride;
      total_offset += gst_memory_get_sizes (info->dmabuf, NULL, NULL);
      gst_buffer_insert_memory (buffer, -1, gst_memory_ref (info->dmabuf));
    } else {
      gst_buffer_unref (buffer);
      buffer = NULL;
      goto export_complete;
    }
  }

  src_caps = gst_pad_get_current_caps (GST_BASE_TRANSFORM (download)->srcpad);
  gst_video_info_from_caps (&out_info, src_caps);
  gst_caps_unref (src_caps);

  if (download->add_videometa) {
    gst_buffer_add_video_meta_full (buffer, GST_VIDEO_FRAME_FLAG_NONE,
        out_info.finfo->format, out_info.width, out_info.height,
        out_info.finfo->n_planes, offset, stride);
  } else {
    int i;
    gboolean match = TRUE;
    for (i = 0; i < gst_buffer_n_memory (inbuf); i++) {
      if (offset[i] != out_info.offset[i] || stride[i] != out_info.stride[i]) {
        match = FALSE;
        break;
      }
    }

    if (!match) {
      gst_buffer_unref (buffer);
      buffer = NULL;
    }
  }

export_complete:

  return buffer;
}
#endif /* GST_GL_HAVE_PLATFORM_EGL && GST_GL_HAVE_DMABUF */

static GstFlowReturn
gst_gl_download_element_prepare_output_buffer (GstBaseTransform * bt,
    GstBuffer * inbuf, GstBuffer ** outbuf)
{
  GstGLDownloadElement *dl = GST_GL_DOWNLOAD_ELEMENT (bt);
  GstCaps *src_caps = gst_pad_get_current_caps (bt->srcpad);
  gint i, n;
  GstMemory *mem;
  GstVideoInfo info;

  mem = gst_buffer_peek_memory (inbuf, 0);
  gst_video_info_from_caps (&info, src_caps);
  gst_caps_replace (&src_caps, NULL);

#if GST_GL_HAVE_IONDMA
  *outbuf = gst_gl_download_element_export_teximage_dma (dl, inbuf);
  if (*outbuf) {
    return GST_FLOW_OK;
  }

  if (gst_is_gl_memory_dma (mem)) {
    GstGLContext *context = GST_GL_BASE_FILTER (bt)->context;

    *outbuf = gst_gl_memory_dma_buffer_to_gstbuffer (context, &info, inbuf);

    GST_DEBUG_OBJECT (dl, "gl download with dma buf.");

    return GST_FLOW_OK;
  }
#endif

#if GST_GL_HAVE_PHYMEM
  if (gst_is_gl_physical_memory (mem)) {
    GstGLContext *context = GST_GL_BASE_FILTER (bt)->context;

    *outbuf = gst_gl_phymem_buffer_to_gstbuffer (context, &info, inbuf);

    GST_DEBUG_OBJECT (dl, "gl download with direct viv.");

    return GST_FLOW_OK;
  }
#endif /* GST_GL_HAVE_PHYMEM */

  *outbuf = inbuf;

  if (dl->do_pbo_transfers) {
    n = gst_buffer_n_memory (*outbuf);
    for (i = 0; i < n; i++) {
      GstMemory *mem = gst_buffer_peek_memory (*outbuf, i);

      if (gst_is_gl_memory_pbo (mem))
        gst_gl_memory_pbo_download_transfer ((GstGLMemoryPBO *) mem);
    }
  }
#if GST_GL_HAVE_PLATFORM_EGL && GST_GL_HAVE_DMABUF
  else if (dl->dmabuf_allocator) {
    GstBuffer *buffer = _try_export_dmabuf (dl, inbuf);
    if (buffer) {
      if (GST_BASE_TRANSFORM_GET_CLASS (bt)->copy_metadata)
        if (!GST_BASE_TRANSFORM_GET_CLASS (bt)->copy_metadata (bt, inbuf,
                buffer)) {
          GST_ELEMENT_WARNING (GST_ELEMENT (bt), STREAM, NOT_IMPLEMENTED,
              ("could not copy metadata"), (NULL));
        }

      *outbuf = buffer;
    } else {
      GstCaps *src_caps;
      GstCapsFeatures *features;

      gst_object_unref (dl->dmabuf_allocator);
      dl->dmabuf_allocator = NULL;

      src_caps = gst_pad_get_current_caps (bt->srcpad);
      src_caps = gst_caps_make_writable (src_caps);
      features = gst_caps_get_features (src_caps, 0);
      gst_caps_features_remove (features, GST_CAPS_FEATURE_MEMORY_DMABUF);

      if (!gst_base_transform_update_src_caps (bt, src_caps)) {
        gst_caps_unref (src_caps);
        GST_ERROR_OBJECT (bt, "DMABuf exportation didn't work and system "
            "memory is not supported.");
        return GST_FLOW_NOT_NEGOTIATED;
      }
      gst_caps_unref (src_caps);
    }
  }
#endif

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_gl_download_element_transform (GstBaseTransform * bt,
    GstBuffer * inbuf, GstBuffer * outbuf)
{
  return GST_FLOW_OK;
}

static gboolean
gst_gl_download_element_propose_allocation (GstBaseTransform * bt,
    GstQuery * decide_query, GstQuery * query)
{
  GstGLContext *context = GST_GL_BASE_FILTER (bt)->context;
  GstGLDownloadElement *download = GST_GL_DOWNLOAD_ELEMENT (bt);
  GstAllocationParams params;
  GstAllocator *allocator = NULL;
  GstBufferPool *pool = NULL;
  guint n_pools, i;
  GstVideoInfo info;
  GstCaps *caps;
  GstStructure *config;
  gsize size;
  GstVideoFormat fmt;

#if GST_GL_HAVE_IONDMA
  if (download->glExportTexImageDMA) {
    return GST_BASE_TRANSFORM_CLASS (parent_class)->propose_allocation (bt,
        decide_query, query);
  }
#endif

  gst_query_parse_allocation (query, &caps, NULL);
  if (!gst_video_info_from_caps (&info, caps)) {
    GST_WARNING_OBJECT (bt, "invalid caps specified");
    return FALSE;
  }

  gst_allocation_params_init (&params);

  fmt = GST_VIDEO_INFO_FORMAT (&info);

  GST_DEBUG_OBJECT (bt, "video format is %s", gst_video_format_to_string (fmt));

  #if GST_GL_HAVE_IONDMA
  if (fmt == GST_VIDEO_FORMAT_RGBA || fmt == GST_VIDEO_FORMAT_RGB16 ||
      fmt == GST_VIDEO_FORMAT_RGB ||
      fmt == GST_VIDEO_FORMAT_BGRA ||
      fmt == GST_VIDEO_FORMAT_BGR ) {
    allocator = gst_gl_memory_dma_allocator_obtain ();
    GST_DEBUG_OBJECT (bt, "obtain dma memory allocator %p.", allocator);
  }
#endif

#if GST_GL_HAVE_PHYMEM
  if (!allocator && gst_is_gl_physical_memory_supported_fmt (&info)) {
    allocator = gst_phy_mem_allocator_obtain ();
    GST_DEBUG_OBJECT (bt, "obtain physical memory allocator %p.", allocator);
  }
#endif /* GST_GL_HAVE_PHYMEM */

  if (!allocator)
    allocator = gst_allocator_find (GST_GL_MEMORY_ALLOCATOR_NAME);

  if (!allocator) {
    GST_ERROR_OBJECT (bt, "Can't obtain gl memory allocator.");
    return FALSE;
  }

  gst_query_add_allocation_param (query, allocator, &params);
  gst_object_unref (allocator);

  n_pools = gst_query_get_n_allocation_pools (query);
  for (i = 0; i < n_pools; i++) {
    gst_query_parse_nth_allocation_pool (query, i, &pool, NULL, NULL, NULL);
    gst_object_unref (pool);
    pool = NULL;
  }

  //new buffer pool
  pool = gst_gl_buffer_pool_new (context);
  config = gst_buffer_pool_get_config (pool);

  /* the normal size of a frame */
  size = info.size;
  gst_buffer_pool_config_set_params (config, caps, size, 0, 0);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_GL_SYNC_META);

  if (!gst_buffer_pool_set_config (pool, config)) {
    gst_object_unref (pool);
    GST_WARNING_OBJECT (bt, "failed setting config");
    return FALSE;
  }

  GST_DEBUG_OBJECT (download, "create pool %p", pool);

  /* propose up to 3 buffers with no minimum to save memory */
  gst_query_add_allocation_pool (query, pool, size, 0, 3);

  gst_object_unref (pool);

  return TRUE;
}

static gboolean
gst_gl_download_element_decide_allocation (GstBaseTransform * trans,
    GstQuery * query)
{
  GstGLDownloadElement *download = GST_GL_DOWNLOAD_ELEMENT_CAST (trans);

  if (gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL)) {
    download->add_videometa = TRUE;
  } else {
    download->add_videometa = FALSE;
  }

  return GST_BASE_TRANSFORM_CLASS (parent_class)->decide_allocation (trans,
      query);
}

static void
gst_gl_download_element_finalize (GObject * object)
{
  GstGLDownloadElement *download = GST_GL_DOWNLOAD_ELEMENT_CAST (object);

  if (download->dmabuf_allocator) {
    gst_object_unref (GST_OBJECT (download->dmabuf_allocator));
    download->dmabuf_allocator = NULL;
  }

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

#if GST_GL_HAVE_IONDMA
static const gchar*
_video_format_to_string (GstVideoFormat format)
{
  if (format == GST_VIDEO_FORMAT_UNKNOWN)
    return "UNSET";
  return gst_video_format_to_string (format);
}

static gboolean
has_blitter (GstGLDownloadElement * dl, gboolean * blitter)
{
  gsize length;
  gchar *contents = NULL;
  gboolean ret = FALSE;

  if (!g_file_get_contents ("/sys/firmware/devicetree/base/model",
      &contents, &length, NULL)) {
    GST_WARNING_OBJECT (dl, "Failed to read devicetree model");
    goto beach;
  }

  if (g_strrstr(contents, "i.MX8MQ")) {
    ret = TRUE;
    *blitter = TRUE;
  } else if (g_strrstr(contents, "i.MX8MM")) {
    ret = TRUE;
    *blitter = FALSE;
  } else {
    GST_WARNING_OBJECT (dl, "Unsupported model '%s'", contents);
  }

beach:
  g_free (contents);
  return ret;
}

static void
setup_export_teximage_dma (GstGLDownloadElement * dl)
{
  gboolean blitter;
  GstStructure *config;
  GstAllocationParams params;
  GstVideoAlignment alignment;
  GstGLContext *export_context = NULL;
  GstCaps *export_caps = NULL;
  GstBufferPool *pool = NULL;
  GstVideoConverter *converter = NULL;
  GstVideoFormat export_format = GST_VIDEO_FORMAT_UNKNOWN, src_format;
  GstGLContext *context = GST_GL_BASE_FILTER (dl)->context;
  GstBaseTransform *transform = GST_BASE_TRANSFORM (dl);
  GstCaps *src_caps = gst_pad_get_current_caps (transform->srcpad);
    PFNGLEXPORTTEXIMAGEDMAPROC glExportTexImageDMA =
      gst_gl_context_get_proc_address (context, "glExportTexImageDMA");

  gst_video_info_init (&dl->export_info);
  gst_video_info_init (&dl->src_info);
  gst_allocation_params_init (&params);
  gst_video_alignment_reset (&alignment);
  dl->glExportTexImageDMA = NULL;

  if (!glExportTexImageDMA) {
    GST_INFO_OBJECT (dl, "glExportTexImageDMA not supported");
    goto beach;
  }

  if (!gst_video_info_from_caps (&dl->src_info, src_caps)) {
    GST_WARNING_OBJECT (dl, "invalid src_caps %" GST_PTR_FORMAT, src_caps);
    goto beach;
  }
  src_format = GST_VIDEO_INFO_FORMAT (&dl->src_info);

  if (!has_blitter (dl, &blitter)) {
    goto beach;
  }

  switch (src_format) {
    case GST_VIDEO_FORMAT_RGBA:
      export_format = src_format;
      break;
    case GST_VIDEO_FORMAT_RGB:
    case GST_VIDEO_FORMAT_BGR:
      dl->src_info.stride[0] = dl->src_info.width * 3;
      dl->src_info.size = dl->src_info.stride[0] * dl->src_info.height;
      /* Tightly pack 24bit formats and fall through. */
    case GST_VIDEO_FORMAT_BGRA:
      export_format = blitter ? src_format : GST_VIDEO_FORMAT_RGBA;
      break;
    default:
      export_format = GST_VIDEO_FORMAT_UNKNOWN;
      break;
  }

  if (export_format == GST_VIDEO_FORMAT_UNKNOWN) {
    GST_INFO_OBJECT (dl, "Format '%s' not supported",
        _video_format_to_string (src_format));
    goto beach;
  }

  gst_video_info_set_format(&dl->export_info, export_format,
      blitter ? dl->src_info.width : GST_ROUND_UP_16 (dl->src_info.width),
      blitter ? dl->src_info.height : GST_ROUND_UP_16 (dl->src_info.height));
  dl->export_info.colorimetry = dl->src_info.colorimetry;
  dl->export_info.par_n = dl->src_info.par_n;
  dl->export_info.par_d = dl->src_info.par_d;
  dl->export_info.fps_n = dl->src_info.fps_n;
  dl->export_info.fps_d = dl->src_info.fps_d;
  params.align = blitter ? 0 : 127;
  alignment.stride_align[0] = params.align;
  gst_video_info_align (&dl->export_info, &alignment);
  if (blitter) {
    /* Always tightly pack. */
    dl->export_info.stride[0] = dl->export_info.width *
        GST_VIDEO_FORMAT_INFO_PSTRIDE (dl->export_info.finfo, 0);
  }
  export_caps = gst_video_info_to_caps (&dl->export_info);
  GST_DEBUG_OBJECT (dl, "export_caps %" GST_PTR_FORMAT, export_caps);
  GST_DEBUG_OBJECT (dl, "src_caps %" GST_PTR_FORMAT, src_caps);

  pool = gst_video_buffer_pool_new ();
  config = gst_buffer_pool_get_config (pool);

  gst_buffer_pool_config_set_params (config, export_caps,
      dl->export_info.size, 0, 3);
  gst_buffer_pool_config_add_option (config,
    GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
  gst_buffer_pool_config_set_video_alignment (config, &alignment);
  gst_buffer_pool_config_set_allocator (config,
      gst_ion_allocator_obtain (), &params);

  if (!gst_buffer_pool_set_config (pool, config)) {
    GST_ERROR_OBJECT (dl, "gst_buffer_pool_set_config failed");
    goto beach;
  }
  gst_buffer_pool_set_active (pool, TRUE);

  dl->export_info.width = dl->src_info.width;
  dl->export_info.height = dl->src_info.height;

  if (export_format != src_format ||
      dl->export_info.stride[0] != dl->src_info.stride[0]) {
    converter = gst_video_converter_new (&dl->export_info, &dl->src_info,
        gst_structure_new ("GstVideoConvertConfig",
            GST_VIDEO_CONVERTER_OPT_THREADS, G_TYPE_UINT,
            g_get_num_processors(), NULL));
    if (!converter) {
      GST_ERROR_OBJECT (dl, "gst_video_converter_new failed");
      goto beach;
    }
  }

  export_context = gst_gl_context_new (gst_gl_context_get_display (context));
  if (!export_context) {
    GST_ERROR_OBJECT (dl, "gst_gl_context_new failed");
    goto beach;
  }

  if (!gst_gl_context_create (export_context, context, NULL)) {
    GST_ERROR_OBJECT (dl, "gst_gl_context_create failed");
    goto beach;
  }

  dl->glExportTexImageDMA = glExportTexImageDMA;
  dl->export_context = export_context;
  dl->export_pool = pool;
  dl->converter = converter;
  export_context = NULL;
  pool = NULL;
  converter = NULL;

beach:
  if (src_caps) {
    gst_caps_unref (src_caps);
  }
  if (export_caps) {
    gst_caps_unref (export_caps);
  }
  if (pool) {
    gst_object_unref (pool);
  }
  if (converter) {
    gst_video_converter_free (converter);
  }
  if (export_context) {
    gst_object_unref (export_context);
  }

  GST_INFO_OBJECT (dl,
      "glExportTexImageDMA %d converter %d: %s (%dx%d %d) -> %s (%dx%d %d)",
      !!dl->glExportTexImageDMA, !!dl->converter,
      _video_format_to_string (GST_VIDEO_INFO_FORMAT (&dl->export_info)),
      dl->export_info.width, dl->export_info.height, dl->export_info.stride[0],
      _video_format_to_string (GST_VIDEO_INFO_FORMAT (&dl->src_info)),
      dl->src_info.width, dl->src_info.height, dl->src_info.stride[0]);
}

struct ExportParams
{
  GstGLDownloadElement *dl;
  GLuint tex;
  GLint fd;
  GLint fourcc;
  GLsizei width;
  GLsizei height;
  GLsizei stride;
  GLboolean res;
};

static void
_export_gl (GstGLContext * context, struct ExportParams * params)
{
  GstClockTime ts;

  ts = gst_util_get_timestamp ();
  params->res = params->dl->glExportTexImageDMA(params->tex, &params->fd,
      &params->fourcc, &params->width, &params->height, &params->stride);
  ts = gst_util_get_timestamp () - ts;
  GST_DEBUG_OBJECT (params->dl, "glExportTexImageDMA %.2g ms",
      (double) ts / GST_MSECOND);
}

static GstBuffer *
gst_gl_download_element_export_teximage_dma (
    GstGLDownloadElement * dl, GstBuffer * inbuf)
{
  GstVideoFormat export_format;
  GstMemory *memory = gst_buffer_peek_memory (inbuf, 0);
  GstGLContext *context = GST_GL_BASE_MEMORY_CAST (memory)->context;
  GstBuffer *export_buf = NULL, *src_buf = NULL, *outbuf = NULL;
  GstVideoMeta *export_meta = NULL;
  GstGLColorConvert *glconverter = NULL;
  struct ExportParams params = { 0 };

  if (!dl->glExportTexImageDMA) {
    goto beach;
  }

  if (!gst_gl_context_can_share (context, dl->export_context)) {
    GST_WARNING_OBJECT (dl, "buffer and internal GL contexts can't share"
        " resources, performance will be degraded");
  } else {
    context = dl->export_context;
  }

  /* YUV dma-bufs are uploaded to GL textures as EGLImage and stamped as RGBA
   * as they can be sampled by HW as such when drawing. However we can't get
   * CPU accessible RGBA data out of them without drawing at least once.
   * This is analogous to the disable-passthrough property NXP added to the
   * glcolorconvert element.
   */
  if (gst_is_gl_memory_egl (memory)) {
    if (G_UNLIKELY (dl->glconverter == NULL)) {
      GstCaps *sink_caps = NULL;

      sink_caps = gst_pad_get_current_caps (GST_BASE_TRANSFORM (dl)->sinkpad);
      glconverter = gst_gl_color_convert_new (context);

      if (!gst_gl_color_convert_set_caps (glconverter, sink_caps, sink_caps)) {
        gst_caps_unref (sink_caps);
        GST_ERROR_OBJECT (dl, "failed to set gl converter caps");
        goto beach;
      }

      gst_caps_unref (sink_caps);
      dl->glconverter = gst_object_ref (glconverter);
      GST_DEBUG_OBJECT (dl, "created gl color converter %p", dl->glconverter);
    } else {
      glconverter = gst_object_ref (dl->glconverter);
    }
  }

  if (glconverter) {
    GstBuffer *tmp_buf;

    tmp_buf = gst_gl_color_convert_perform (glconverter, inbuf);
    if (!tmp_buf) {
      inbuf = NULL;
      GST_ERROR_OBJECT (dl, "failed to convert inbuf");
      goto beach;
    }
    gst_buffer_copy_into (tmp_buf, inbuf,
        GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS, 0, -1);
    inbuf = tmp_buf;
    memory = gst_buffer_peek_memory (inbuf, 0);
  }

  if (gst_buffer_pool_acquire_buffer (dl->export_pool, &export_buf, NULL) !=
      GST_FLOW_OK) {
    GST_ERROR_OBJECT (dl, "gst_buffer_pool_acquire_buffer failed");
    goto beach;
  }

  /* GL has no notion of BGR/BGRA so GStreamer stores its data labeled RGBA
   * and swizzles with shaders. The driver doesn't know what the texture
   * contains based on its internal book keeping, it's always RGBA as far as
   * it knows. So we always export as RGB/A to get the output of GStreamer
   * shaders in the intended format.
   */
  export_format = GST_VIDEO_INFO_FORMAT (&dl->export_info);
  switch (export_format) {
    case GST_VIDEO_FORMAT_RGBA:
    case GST_VIDEO_FORMAT_BGRA:
      params.fourcc = DRM_FORMAT_ABGR8888;
      break;
    case GST_VIDEO_FORMAT_RGB:
    case GST_VIDEO_FORMAT_BGR:
      params.fourcc = DRM_FORMAT_BGR888;
      break;
    default:
      goto beach;
  }

  params.dl = dl;
  params.tex = gst_gl_memory_get_texture_id (GST_GL_MEMORY_CAST (memory));
  params.fd = gst_dmabuf_memory_get_fd (gst_buffer_peek_memory (export_buf, 0));
  params.width = dl->export_info.width;
  params.height = dl->export_info.height;
  params.stride = dl->export_info.stride[0];
  gst_gl_context_thread_add (context, (GstGLContextThreadFunc) _export_gl,
      &params);

  if (!params.res) {
    GST_ERROR_OBJECT (dl, "glExportTexImageDMA failed");
    goto beach;
  }

  /* If we're packing RGB data more tightly than GStreamer normally does
     we must manually set our stride on the meta. */
  export_meta = gst_buffer_get_video_meta (export_buf);
  if (G_LIKELY (!export_meta)) {
    gst_buffer_add_video_meta_full (export_buf, GST_VIDEO_FRAME_FLAG_NONE,
        dl->export_info.finfo->format, dl->export_info.width,
        dl->export_info.height, dl->export_info.finfo->n_planes,
        dl->export_info.offset, dl->export_info.stride);
  } else {
    export_meta->stride[0] = dl->export_info.stride[0];
  }

  if (dl->converter) {
    GstVideoFrame in_frame, out_frame;
    GstClockTime ts;

    /* TODO: buffer pool */
    src_buf = gst_buffer_new_allocate (NULL, dl->src_info.size, NULL);

    if (!src_buf) {
      GST_ERROR_OBJECT (dl, "failed to allocate buffer of size %zu",
          dl->src_info.size);
      goto beach;
    }

    ts = gst_util_get_timestamp ();
    if (!gst_video_frame_map (&in_frame, &dl->export_info, export_buf,
        GST_MAP_READ | GST_VIDEO_FRAME_MAP_FLAG_NO_REF)) {
      GST_ERROR_OBJECT (dl, "gst_video_frame_map failed");
      goto beach;
    }
    if (!gst_video_frame_map (&out_frame, &dl->src_info, src_buf,
        GST_MAP_READ | GST_VIDEO_FRAME_MAP_FLAG_NO_REF)) {
      GST_ERROR_OBJECT (dl, "gst_video_frame_map failed");
      goto beach;
    }
    gst_video_converter_frame (dl->converter, &in_frame, &out_frame);
    gst_video_frame_unmap(&out_frame);
    gst_video_frame_unmap(&in_frame);
    ts = gst_util_get_timestamp () - ts;
    GST_DEBUG_OBJECT (dl, "convert %.2g ms",
        (double) ts / GST_MSECOND);

    outbuf = src_buf;
    src_buf = NULL;
  } else {
    outbuf = export_buf;
    export_buf = NULL;
  }

  gst_buffer_copy_into (outbuf, inbuf,
        GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS, 0, -1);

beach:
  gst_buffer_replace (&export_buf, NULL);
  gst_buffer_replace (&src_buf, NULL);
  if (glconverter) {
    gst_object_unref (glconverter);
    if (inbuf) {
      gst_buffer_unref (inbuf);
    }
  }
  return outbuf;
}

static gboolean
gst_gl_download_element_gl_start (GstGLBaseFilter * base)
{
  GstGLDownloadElement *download = GST_GL_DOWNLOAD_ELEMENT_CAST (base);

  setup_export_teximage_dma (download);

  return GST_GL_BASE_FILTER_CLASS (parent_class)->gl_start (base);
}

static void
gst_gl_download_element_gl_stop (GstGLBaseFilter * base)
{
  GstGLDownloadElement *download = GST_GL_DOWNLOAD_ELEMENT_CAST (base);

  GST_GL_BASE_FILTER_CLASS (parent_class)->gl_stop (base);

  if (download->export_pool) {
    gst_buffer_pool_set_active (download->export_pool, FALSE);
    gst_object_unref (GST_OBJECT (download->export_pool));
    download->export_pool = NULL;
  }

  if (download->converter) {
    gst_video_converter_free (download->converter);
    download->converter = NULL;
  }

  if (download->glconverter) {
    gst_object_unref (download->glconverter);
    download->glconverter = NULL;
  }

  if (download->export_context) {
    gst_object_unref (download->export_context);
    download->export_context = NULL;
  }
}
#endif

