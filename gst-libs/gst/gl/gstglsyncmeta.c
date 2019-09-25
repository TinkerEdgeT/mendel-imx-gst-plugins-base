/*
 * GStreamer
 * Copyright (C) 2014 Matthew Waters <matthew@centricular.com>
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

/**
 * SECTION:gstglsyncmeta
 * @title: GstGLSyncMeta
 * @short_description: synchronization primitives
 * @see_also: #GstGLBaseMemory, #GstGLContext
 *
 * #GstGLSyncMeta provides the ability to synchronize the OpenGL command stream
 * with the CPU or with other OpenGL contexts.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstglsyncmeta.h"

#include "gstglcontext.h"
#include "gstglfuncs.h"

#if GST_GL_HAVE_IONDMA
#include "gst/gl/gstglmemorydma.h"
#include "gst/gl/egl/gstegl.h"
#include "gst/gl/egl/gstglcontext_egl.h"
#include "gst/gl/egl/gstgldisplay_egl.h"
#endif

GST_DEBUG_CATEGORY_STATIC (gst_gl_sync_meta_debug);
#define GST_CAT_DEFAULT gst_gl_sync_meta_debug

#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#endif
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT        0x00000001
#endif
#ifndef GL_TIMEOUT_EXPIRED
#define GL_TIMEOUT_EXPIRED 0x911B
#endif
#ifndef GL_TIMEOUT_IGNORED
#define GL_TIMEOUT_IGNORED G_GUINT64_CONSTANT(0xFFFFFFFFFFFFFFFF)
#endif

#if GST_GL_HAVE_IONDMA
static gboolean
_is_dma_memory (GstBuffer * buf)
{
  if (gst_buffer_n_memory(buf)) {
    GstGLMemory *glmem;
    glmem = gst_buffer_peek_memory (buf, 0);
    return gst_is_gl_memory_dma (glmem);
  }

  return FALSE;
}

static EGLDisplay
_get_egl_display (GstGLContext * context)
{
  EGLDisplay egl_display;
  GstGLDisplayEGL *display_egl;
  if (!context->display) {
    return EGL_NO_DISPLAY;
  }
  display_egl = gst_gl_display_egl_from_gl_display (context->display);
  egl_display =
      (EGLDisplay) gst_gl_display_get_handle (GST_GL_DISPLAY (display_egl));
  gst_object_unref (display_egl);
  return egl_display;
}

static EGLSyncKHR
_eglCreateSyncKHR (GstGLContext * context)
{
  EGLSyncKHR sync;
  EGLDisplay dpy = _get_egl_display (context);
  if (dpy == EGL_NO_DISPLAY) {
    return EGL_NO_SYNC_KHR;
  }

  EGLSyncKHR (*gst_eglCreateSyncKHR) (EGLDisplay dpy, EGLenum type,
        const EGLAttrib * attrib_list);
  gst_eglCreateSyncKHR = gst_gl_context_get_proc_address (context,
        "eglCreateSyncKHR");
  sync = gst_eglCreateSyncKHR (dpy, EGL_SYNC_FENCE_KHR, NULL);
  GST_LOG ("setting egl sync object %p", sync);
  return sync;
}

static EGLint
_eglClientWaitSyncKHR (GstGLContext * context, EGLSyncKHR sync)
{
  EGLDisplay dpy = _get_egl_display (context);
  if (dpy == EGL_NO_DISPLAY) {
    return;
  }

  EGLint (*gst_eglClientWaitSyncKHR) (EGLDisplay dpy, EGLSyncKHR sync,
        EGLint flags, EGLTimeKHR timeout);
  GST_LOG ("waiting on egl sync object %p", sync);
  gst_eglClientWaitSyncKHR = gst_gl_context_get_proc_address (context,
        "eglClientWaitSyncKHR");
  return gst_eglClientWaitSyncKHR (dpy, sync,
          EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 1000000000 /* 1s */ );
}

static EGLBoolean
_eglDestroySyncKHR (GstGLContext * context, EGLSyncKHR sync)
{
  EGLDisplay dpy = _get_egl_display (context);
  if (dpy == EGL_NO_DISPLAY) {
    return EGL_FALSE;
  }

  EGLBoolean (*gst_eglDestroySyncKHR) (EGLDisplay dpy, EGLSyncKHR sync);
  gst_eglDestroySyncKHR = gst_gl_context_get_proc_address (context,
        "eglDestroySyncKHR");
  GST_LOG ("deleting egl sync object %p", sync);
  return gst_eglDestroySyncKHR (dpy, sync);
}

static void
_wait_cpu_egl (GstGLSyncMeta * sync_meta, GstGLContext * context)
{
  EGLint egl_res;

  if(sync_meta->egl_data == EGL_NO_SYNC_KHR) {
    return;
  }

  do {
    egl_res = _eglClientWaitSyncKHR (context,
        (EGLSyncKHR) sync_meta->egl_data);
  } while (egl_res == EGL_TIMEOUT_EXPIRED_KHR);
}
#endif

static void
_default_set_sync_gl (GstGLSyncMeta * sync_meta, GstGLContext * context)
{
  const GstGLFuncs *gl = context->gl_vtable;

  if (gl->FenceSync) {
    if (sync_meta->data) {
      GST_LOG ("deleting sync object %p", sync_meta->data);
      gl->DeleteSync ((GLsync) sync_meta->data);
    }
    sync_meta->data =
        (gpointer) gl->FenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    GST_LOG ("setting sync object %p", sync_meta->data);
  }

  if (gst_gl_context_is_shared (context))
    gl->Flush ();

#if GST_GL_HAVE_IONDMA
  if (sync_meta->is_egl) {
    if (sync_meta->egl_data) {
      _eglDestroySyncKHR (context, (EGLSyncKHR) sync_meta->egl_data);
    }
    sync_meta->egl_data = _eglCreateSyncKHR (context);
  }
#endif
}

static void
_default_wait_gl (GstGLSyncMeta * sync_meta, GstGLContext * context)
{
  const GstGLFuncs *gl = context->gl_vtable;

  if (sync_meta->data && gl->WaitSync) {
    GST_LOG ("waiting on sync object %p", sync_meta->data);
    gl->WaitSync ((GLsync) sync_meta->data, 0, GL_TIMEOUT_IGNORED);
  }
}

static void
_default_wait_cpu_gl (GstGLSyncMeta * sync_meta, GstGLContext * context)
{
  const GstGLFuncs *gl = context->gl_vtable;
  GLenum res;

#if GST_GL_HAVE_IONDMA
  if (sync_meta->egl_data) {
    EGLint egl_res;

    do {
      egl_res = _eglClientWaitSyncKHR (context,
          (EGLSyncKHR) sync_meta->egl_data);
    } while (egl_res == EGL_TIMEOUT_EXPIRED_KHR);
    return;
  }
#endif

  if (sync_meta->data && gl->ClientWaitSync) {
    do {
      GST_LOG ("waiting on sync object %p", sync_meta->data);
      res =
          gl->ClientWaitSync ((GLsync) sync_meta->data,
          GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000 /* 1s */ );
    } while (res == GL_TIMEOUT_EXPIRED);
  } else {
    gl->Finish ();
  }
}

static void
_default_copy (GstGLSyncMeta * src, GstBuffer * sbuffer, GstGLSyncMeta * dest,
    GstBuffer * dbuffer)
{
  GST_LOG ("copy sync object %p from meta %p to %p", src->data, src, dest);

#if GST_GL_HAVE_IONDMA
  dest->is_egl = src->is_egl;
#endif

  /* Setting a sync point here relies on GstBuffer copying
   * metas after data */
  gst_gl_sync_meta_set_sync_point (src, src->context);
}

static void
_default_free_gl (GstGLSyncMeta * sync_meta, GstGLContext * context)
{
  const GstGLFuncs *gl = context->gl_vtable;

  if (sync_meta->data) {
    GST_LOG ("deleting sync object %p", sync_meta->data);
    gl->DeleteSync ((GLsync) sync_meta->data);
    sync_meta->data = NULL;
  }

#if GST_GL_HAVE_IONDMA
  if (sync_meta->egl_data) {
    _eglDestroySyncKHR (context, (EGLSyncKHR) sync_meta->egl_data);
    sync_meta->egl_data = NULL;
  }
#endif
}

/**
 * gst_buffer_add_gl_sync_meta_full:
 * @context: a #GstGLContext
 * @buffer: a #GstBuffer
 * @data: sync data to hold
 *
 * Returns: (transfer none): the #GstGLSyncMeta added to #GstBuffer
 *
 * Since: 1.8
 */
GstGLSyncMeta *
gst_buffer_add_gl_sync_meta_full (GstGLContext * context, GstBuffer * buffer,
    gpointer data)
{
  GstGLSyncMeta *meta;

  g_return_val_if_fail (GST_IS_GL_CONTEXT (context), NULL);

  meta =
      (GstGLSyncMeta *) gst_buffer_add_meta ((buffer), GST_GL_SYNC_META_INFO,
      NULL);

  if (!meta)
    return NULL;

  meta->context = gst_object_ref (context);
  meta->data = data;
#if GST_GL_HAVE_IONDMA
  meta->is_egl = _is_dma_memory(buffer);
#endif

  return meta;
}

/**
 * gst_buffer_add_gl_sync_meta:
 * @context: a #GstGLContext
 * @buffer: a #GstBuffer
 *
 * Returns: (transfer none): the #GstGLSyncMeta added to #GstBuffer
 *
 * Since: 1.6
 */
GstGLSyncMeta *
gst_buffer_add_gl_sync_meta (GstGLContext * context, GstBuffer * buffer)
{
  GstGLSyncMeta *ret = gst_buffer_add_gl_sync_meta_full (context, buffer, NULL);
  if (!ret)
    return NULL;

  ret->set_sync_gl = _default_set_sync_gl;
  ret->wait_gl = _default_wait_gl;
  ret->wait_cpu_gl = _default_wait_cpu_gl;
  ret->copy = _default_copy;
  ret->free_gl = _default_free_gl;

  return ret;
}

static void
_set_sync_point (GstGLContext * context, GstGLSyncMeta * sync_meta)
{
  g_assert (sync_meta->set_sync_gl != NULL);

  GST_LOG ("setting sync point %p", sync_meta);
  sync_meta->set_sync_gl (sync_meta, context);
}

/**
 * gst_gl_sync_meta_set_sync_point:
 * @sync_meta: a #GstGLSyncMeta
 * @context: a #GstGLContext
 *
 * Set a sync point to possibly wait on at a later time.
 *
 * Since: 1.6
 */
void
gst_gl_sync_meta_set_sync_point (GstGLSyncMeta * sync_meta,
    GstGLContext * context)
{
  if (sync_meta->set_sync)
    sync_meta->set_sync (sync_meta, context);
  else
    gst_gl_context_thread_add (context,
        (GstGLContextThreadFunc) _set_sync_point, sync_meta);
}

static void
_wait (GstGLContext * context, GstGLSyncMeta * sync_meta)
{
  g_assert (sync_meta->wait_gl != NULL);

  GST_LOG ("waiting %p", sync_meta);
  sync_meta->wait_gl (sync_meta, context);
}

/**
 * gst_gl_sync_meta_wait:
 * @sync_meta: a #GstGLSyncMeta
 * @context: a #GstGLContext
 *
 * Insert a wait into @context's command stream ensuring all previous OpenGL
 * commands before @sync_meta have completed.
 *
 * Since: 1.6
 */
void
gst_gl_sync_meta_wait (GstGLSyncMeta * sync_meta, GstGLContext * context)
{
  if (sync_meta->wait)
    sync_meta->wait (sync_meta, context);
  else
    gst_gl_context_thread_add (context,
        (GstGLContextThreadFunc) _wait, sync_meta);
}

static void
_wait_cpu (GstGLContext * context, GstGLSyncMeta * sync_meta)
{
  g_assert (sync_meta->wait_cpu_gl != NULL);

  GST_LOG ("waiting %p", sync_meta);
  sync_meta->wait_cpu_gl (sync_meta, context);
}

/**
 * gst_gl_sync_meta_wait_cpu:
 * @sync_meta: a #GstGLSyncMeta
 * @context: a #GstGLContext
 *
 * Perform a wait so that the sync point has passed from the CPU's perspective
 * What that means, is that all GL operations changing CPU-visible data before
 * the sync point are now visible.
 *
 * Since: 1.8
 */
void
gst_gl_sync_meta_wait_cpu (GstGLSyncMeta * sync_meta, GstGLContext * context)
{
  if (sync_meta->wait_cpu)
    sync_meta->wait_cpu (sync_meta, context);
  else
#if GST_GL_HAVE_IONDMA
      if (sync_meta->is_egl && sync_meta->egl_data) {
        _wait_cpu_egl (sync_meta, context);
      } else
#endif
    gst_gl_context_thread_add (context,
        (GstGLContextThreadFunc) _wait_cpu, sync_meta);
}

static gboolean
_gst_gl_sync_meta_transform (GstBuffer * dest, GstMeta * meta,
    GstBuffer * buffer, GQuark type, gpointer data)
{
  GstGLSyncMeta *dmeta, *smeta;

  smeta = (GstGLSyncMeta *) meta;

  if (GST_META_TRANSFORM_IS_COPY (type)) {
    GstMetaTransformCopy *copy = data;

    g_assert (smeta->copy != NULL);

    if (!copy->region) {
      /* only copy if the complete data is copied as well */
      dmeta = gst_buffer_add_gl_sync_meta_full (smeta->context, dest, NULL);
      if (!dmeta)
        return FALSE;

      dmeta->set_sync = smeta->set_sync;
      dmeta->set_sync_gl = smeta->set_sync_gl;
      dmeta->wait = smeta->wait;
      dmeta->wait_gl = smeta->wait_gl;
      dmeta->wait_cpu = smeta->wait_cpu;
      dmeta->wait_cpu_gl = smeta->wait_cpu_gl;
      dmeta->copy = smeta->copy;
      dmeta->free = smeta->free;
      dmeta->free_gl = smeta->free_gl;

      GST_LOG ("copying sync meta %p into %p", smeta, dmeta);
      smeta->copy (smeta, buffer, dmeta, dest);
    }
  } else {
    /* return FALSE, if transform type is not supported */
    return FALSE;
  }

  return TRUE;
}

static void
_free_gl_sync_meta (GstGLContext * context, GstGLSyncMeta * sync_meta)
{
  g_assert (sync_meta->free_gl != NULL);

  GST_LOG ("free sync meta %p", sync_meta);
  sync_meta->free_gl (sync_meta, context);
}

static void
_gst_gl_sync_meta_free (GstGLSyncMeta * sync_meta, GstBuffer * buffer)
{
  if (sync_meta->free)
    sync_meta->free (sync_meta, sync_meta->context);
  else
    gst_gl_context_thread_add (sync_meta->context,
        (GstGLContextThreadFunc) _free_gl_sync_meta, sync_meta);

  gst_object_unref (sync_meta->context);
}

static gboolean
_gst_gl_sync_meta_init (GstGLSyncMeta * sync_meta, gpointer params,
    GstBuffer * buffer)
{
  static volatile gsize _init;

  if (g_once_init_enter (&_init)) {
    GST_DEBUG_CATEGORY_INIT (gst_gl_sync_meta_debug, "glsyncmeta", 0,
        "glsyncmeta");
    g_once_init_leave (&_init, 1);
  }

  sync_meta->context = NULL;
  sync_meta->data = NULL;
  sync_meta->set_sync = NULL;
  sync_meta->set_sync_gl = NULL;
  sync_meta->wait = NULL;
  sync_meta->wait_gl = NULL;
  sync_meta->wait_cpu = NULL;
  sync_meta->wait_cpu_gl = NULL;
  sync_meta->copy = NULL;
  sync_meta->free = NULL;
  sync_meta->free_gl = NULL;

#if GST_GL_HAVE_IONDMA
  sync_meta->is_egl = FALSE;
  sync_meta->egl_data = NULL;
#endif

  return TRUE;
}

GType
gst_gl_sync_meta_api_get_type (void)
{
  static volatile GType type = 0;
  static const gchar *tags[] = { NULL };

  if (g_once_init_enter (&type)) {
    GType _type = gst_meta_api_type_register ("GstGLSyncMetaAPI", tags);
    g_once_init_leave (&type, _type);
  }

  return type;
}

const GstMetaInfo *
gst_gl_sync_meta_get_info (void)
{
  static const GstMetaInfo *meta_info = NULL;

  if (g_once_init_enter (&meta_info)) {
    const GstMetaInfo *meta =
        gst_meta_register (GST_GL_SYNC_META_API_TYPE, "GstGLSyncMeta",
        sizeof (GstGLSyncMeta), (GstMetaInitFunction) _gst_gl_sync_meta_init,
        (GstMetaFreeFunction) _gst_gl_sync_meta_free,
        _gst_gl_sync_meta_transform);
    g_once_init_leave (&meta_info, meta);
  }

  return meta_info;
}
