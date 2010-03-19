/*
 *  gstvaapidisplay.c - VA display abstraction
 *
 *  gstreamer-vaapi (C) 2010 Splitted-Desktop Systems
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include "config.h"
#include "gstvaapiutils.h"
#include "gstvaapidisplay.h"
#include <va/va_backend.h>

#define DEBUG 1
#include "gstvaapidebug.h"

GST_DEBUG_CATEGORY(gst_debug_vaapi);

G_DEFINE_TYPE(GstVaapiDisplay, gst_vaapi_display, G_TYPE_OBJECT);

#define GST_VAAPI_DISPLAY_GET_PRIVATE(obj)                      \
    (G_TYPE_INSTANCE_GET_PRIVATE((obj),                         \
                                 GST_VAAPI_TYPE_DISPLAY,	\
                                 GstVaapiDisplayPrivate))

struct _GstVaapiDisplayPrivate {
    GStaticMutex        mutex;
    VADisplay           display;
    gboolean            create_display;
    GArray             *profiles;
    GArray             *image_formats;
    GArray             *subpicture_formats;
};

enum {
    PROP_0,

    PROP_DISPLAY
};

static void
append_formats(GArray *array, const VAImageFormat *va_formats, guint n)
{
    GstVaapiImageFormat format;
    gboolean has_YV12 = FALSE;
    gboolean has_I420 = FALSE;
    guint i;

    for (i = 0; i < n; i++) {
        const VAImageFormat * const va_format = &va_formats[i];

        format = gst_vaapi_image_format(va_format);
        if (!format) {
            GST_DEBUG("unsupported format %" GST_FOURCC_FORMAT,
                      GST_FOURCC_ARGS(va_format->fourcc));
            continue;
        }

        switch (format) {
        case GST_VAAPI_IMAGE_YV12:
            has_YV12 = TRUE;
            break;
        case GST_VAAPI_IMAGE_I420:
            has_I420 = TRUE;
            break;
        default:
            break;
        }
        g_array_append_val(array, format);
    }

    /* Append I420 (resp. YV12) format if YV12 (resp. I420) is not
       supported by the underlying driver */
    if (has_YV12 && !has_I420) {
        format = GST_VAAPI_IMAGE_I420;
        g_array_append_val(array, format);
    }
    else if (has_I420 && !has_YV12) {
        format = GST_VAAPI_IMAGE_YV12;
        g_array_append_val(array, format);
    }
}

/* Sort image formats. Prefer YUV formats first */
static gint
compare_yuv_formats(gconstpointer a, gconstpointer b)
{
    const GstVaapiImageFormat fmt1 = *(GstVaapiImageFormat *)a;
    const GstVaapiImageFormat fmt2 = *(GstVaapiImageFormat *)b;

    const gboolean is_fmt1_yuv = gst_vaapi_image_format_is_yuv(fmt1);
    const gboolean is_fmt2_yuv = gst_vaapi_image_format_is_yuv(fmt2);

    if (is_fmt1_yuv != is_fmt2_yuv)
        return is_fmt1_yuv ? -1 : 1;

    return ((gint)gst_vaapi_image_format_get_score(fmt1) -
            (gint)gst_vaapi_image_format_get_score(fmt2));
}

/* Sort subpicture formats. Prefer RGB formats first */
static gint
compare_rgb_formats(gconstpointer a, gconstpointer b)
{
    const GstVaapiImageFormat fmt1 = *(GstVaapiImageFormat *)a;
    const GstVaapiImageFormat fmt2 = *(GstVaapiImageFormat *)b;

    const gboolean is_fmt1_rgb = gst_vaapi_image_format_is_rgb(fmt1);
    const gboolean is_fmt2_rgb = gst_vaapi_image_format_is_rgb(fmt2);

    if (is_fmt1_rgb != is_fmt2_rgb)
        return is_fmt1_rgb ? -1 : 1;

    return ((gint)gst_vaapi_image_format_get_score(fmt1) -
            (gint)gst_vaapi_image_format_get_score(fmt2));
}

/* Check if profiles array contains profile */
static inline gboolean
find_profile(GArray *profiles, VAProfile profile)
{
    guint i;

    for (i = 0; i < profiles->len; i++)
        if (g_array_index(profiles, VAProfile, i) == profile)
            return TRUE;
    return FALSE;
}

/* Check if formats array contains format */
static inline gboolean
find_format(GArray *formats, GstVaapiImageFormat format)
{
    guint i;

    for (i = 0; i < formats->len; i++)
        if (g_array_index(formats, GstVaapiImageFormat, i) == format)
            return TRUE;
    return FALSE;
}

/* Convert formats array to GstCaps */
static GstCaps *
get_caps(GArray *formats)
{
    GstVaapiImageFormat format;
    GstCaps *out_caps, *caps;
    guint i;

    out_caps = gst_caps_new_empty();
    if (!out_caps)
        return NULL;

    for (i = 0; i < formats->len; i++) {
        format = g_array_index(formats, GstVaapiImageFormat, i);
        caps   = gst_vaapi_image_format_get_caps(format);
        if (caps)
            gst_caps_append(out_caps, caps);
    }
    return out_caps;
}

static void
gst_vaapi_display_destroy(GstVaapiDisplay *display)
{
    GstVaapiDisplayPrivate * const priv = display->priv;

    if (priv->profiles) {
        g_array_free(priv->profiles, TRUE);
        priv->profiles = NULL;
    }

    if (priv->image_formats) {
        g_array_free(priv->image_formats, TRUE);
        priv->image_formats = NULL;
    }

    if (priv->subpicture_formats) {
        g_array_free(priv->subpicture_formats, TRUE);
        priv->subpicture_formats = NULL;
    }

    if (priv->display) {
        vaTerminate(priv->display);
        priv->display = NULL;
    }

    if (priv->create_display) {
        GstVaapiDisplayClass *klass = GST_VAAPI_DISPLAY_GET_CLASS(display);
        if (klass->close_display)
            klass->close_display(display);
    }
}

static gboolean
gst_vaapi_display_create(GstVaapiDisplay *display)
{
    GstVaapiDisplayPrivate * const priv = display->priv;
    gboolean            has_errors      = TRUE;
    VAProfile          *profiles        = NULL;
    VAImageFormat      *formats         = NULL;
    unsigned int       *flags           = NULL;
    gint                i, n, major_version, minor_version;
    VAStatus            status;

    if (!priv->display && priv->create_display) {
        GstVaapiDisplayClass *klass = GST_VAAPI_DISPLAY_GET_CLASS(display);
        if (klass->open_display && !klass->open_display(display))
            return FALSE;
        if (klass->get_display)
            priv->display = klass->get_display(display);
    }
    if (!priv->display)
        return FALSE;

    status = vaInitialize(priv->display, &major_version, &minor_version);
    if (!vaapi_check_status(status, "vaInitialize()"))
        goto end;
    GST_DEBUG("VA-API version %d.%d", major_version, minor_version);

    /* VA profiles */
    profiles = g_new(VAProfile, vaMaxNumProfiles(priv->display));
    if (!profiles)
        goto end;
    status = vaQueryConfigProfiles(priv->display, profiles, &n);
    if (!vaapi_check_status(status, "vaQueryConfigProfiles()"))
        goto end;

    GST_DEBUG("%d profiles", n);
    for (i = 0; i < n; i++)
        GST_DEBUG("  %s", string_of_VAProfile(profiles[i]));

    priv->profiles = g_array_new(FALSE, FALSE, sizeof(VAProfile));
    if (!priv->profiles)
        goto end;
    g_array_append_vals(priv->profiles, profiles, n);

    /* VA image formats */
    formats = g_new(VAImageFormat, vaMaxNumImageFormats(priv->display));
    if (!formats)
        goto end;
    status = vaQueryImageFormats(priv->display, formats, &n);
    if (!vaapi_check_status(status, "vaQueryImageFormats()"))
        goto end;

    GST_DEBUG("%d image formats", n);
    for (i = 0; i < n; i++)
        GST_DEBUG("  %s", string_of_FOURCC(formats[i].fourcc));

    priv->image_formats =
        g_array_new(FALSE, FALSE, sizeof(GstVaapiImageFormat));
    if (!priv->image_formats)
        goto end;
    append_formats(priv->image_formats, formats, n);
    g_array_sort(priv->image_formats, compare_yuv_formats);

    /* VA subpicture formats */
    n = vaMaxNumSubpictureFormats(priv->display);
    formats = g_renew(VAImageFormat, formats, n);
    flags   = g_new(guint, n);
    if (!formats || !flags)
        goto end;
    status = vaQuerySubpictureFormats(priv->display, formats, flags, &n);
    if (!vaapi_check_status(status, "vaQuerySubpictureFormats()"))
        goto end;

    GST_DEBUG("%d subpicture formats", n);
    for (i = 0; i < n; i++)
        GST_DEBUG("  %s", string_of_FOURCC(formats[i].fourcc));

    priv->subpicture_formats =
        g_array_new(FALSE, FALSE, sizeof(GstVaapiImageFormat));
    if (!priv->subpicture_formats)
        goto end;
    append_formats(priv->subpicture_formats, formats, n);
    g_array_sort(priv->subpicture_formats, compare_rgb_formats);

    has_errors = FALSE;
end:
    g_free(profiles);
    g_free(formats);
    g_free(flags);
    return !has_errors;
}

static void
gst_vaapi_display_lock_default(GstVaapiDisplay *display)
{
    g_static_mutex_lock(&display->priv->mutex);
}

static void
gst_vaapi_display_unlock_default(GstVaapiDisplay *display)
{
    g_static_mutex_unlock(&display->priv->mutex);
}

static void
gst_vaapi_display_finalize(GObject *object)
{
    GstVaapiDisplay * const display = GST_VAAPI_DISPLAY(object);

    gst_vaapi_display_destroy(display);

    G_OBJECT_CLASS(gst_vaapi_display_parent_class)->finalize(object);
}

static void
gst_vaapi_display_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
)
{
    GstVaapiDisplay * const display = GST_VAAPI_DISPLAY(object);

    switch (prop_id) {
    case PROP_DISPLAY:
        display->priv->display = g_value_get_pointer(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
gst_vaapi_display_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
)
{
    GstVaapiDisplay * const display = GST_VAAPI_DISPLAY(object);

    switch (prop_id) {
    case PROP_DISPLAY:
        g_value_set_pointer(value, gst_vaapi_display_get_display(display));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
gst_vaapi_display_constructed(GObject *object)
{
    GstVaapiDisplay * const display = GST_VAAPI_DISPLAY(object);
    GObjectClass *parent_class;

    display->priv->create_display = display->priv->display == NULL;
    gst_vaapi_display_create(display);

    parent_class = G_OBJECT_CLASS(gst_vaapi_display_parent_class);
    if (parent_class->constructed)
        parent_class->constructed(object);
}

static void
gst_vaapi_display_class_init(GstVaapiDisplayClass *klass)
{
    GObjectClass * const object_class = G_OBJECT_CLASS(klass);
    GstVaapiDisplayClass * const dpy_class = GST_VAAPI_DISPLAY_CLASS(klass);

    GST_DEBUG_CATEGORY_INIT(gst_debug_vaapi, "vaapi", 0, "VA-API helper");

    g_type_class_add_private(klass, sizeof(GstVaapiDisplayPrivate));

    object_class->finalize      = gst_vaapi_display_finalize;
    object_class->set_property  = gst_vaapi_display_set_property;
    object_class->get_property  = gst_vaapi_display_get_property;
    object_class->constructed   = gst_vaapi_display_constructed;

    dpy_class->lock_display     = gst_vaapi_display_lock_default;
    dpy_class->unlock_display   = gst_vaapi_display_unlock_default;

    g_object_class_install_property
        (object_class,
         PROP_DISPLAY,
         g_param_spec_pointer("display",
                              "VA display",
                              "VA display",
                              G_PARAM_READWRITE|G_PARAM_CONSTRUCT_ONLY));
}

static void
gst_vaapi_display_init(GstVaapiDisplay *display)
{
    GstVaapiDisplayPrivate *priv = GST_VAAPI_DISPLAY_GET_PRIVATE(display);

    display->priv               = priv;
    priv->display               = NULL;
    priv->create_display        = TRUE;
    priv->profiles              = NULL;
    priv->image_formats         = NULL;
    priv->subpicture_formats    = NULL;

    g_static_mutex_init(&priv->mutex);
}

GstVaapiDisplay *
gst_vaapi_display_new_with_display(VADisplay va_display)
{
    return g_object_new(GST_VAAPI_TYPE_DISPLAY,
                        "display", va_display,
                        NULL);
}

void
gst_vaapi_display_lock(GstVaapiDisplay *display)
{
    GstVaapiDisplayClass *klass;

    g_return_if_fail(GST_VAAPI_IS_DISPLAY(display));

    klass = GST_VAAPI_DISPLAY_GET_CLASS(display);
    if (klass->lock_display)
        klass->lock_display(display);
}

void
gst_vaapi_display_unlock(GstVaapiDisplay *display)
{
    GstVaapiDisplayClass *klass;

    g_return_if_fail(GST_VAAPI_IS_DISPLAY(display));

    klass = GST_VAAPI_DISPLAY_GET_CLASS(display);
    if (klass->unlock_display)
        klass->unlock_display(display);
}

VADisplay
gst_vaapi_display_get_display(GstVaapiDisplay *display)
{
    g_return_val_if_fail(GST_VAAPI_IS_DISPLAY(display), NULL);

    return display->priv->display;
}

gboolean
gst_vaapi_display_has_profile(GstVaapiDisplay *display, VAProfile profile)
{
    g_return_val_if_fail(GST_VAAPI_IS_DISPLAY(display), FALSE);

    return find_profile(display->priv->profiles, profile);
}

GstCaps *
gst_vaapi_display_get_image_caps(GstVaapiDisplay *display)
{
    g_return_val_if_fail(GST_VAAPI_IS_DISPLAY(display), NULL);

    return get_caps(display->priv->image_formats);
}

gboolean
gst_vaapi_display_has_image_format(
    GstVaapiDisplay    *display,
    GstVaapiImageFormat format
)
{
    g_return_val_if_fail(GST_VAAPI_IS_DISPLAY(display), FALSE);
    g_return_val_if_fail(format, FALSE);

    return find_format(display->priv->image_formats, format);
}

GstCaps *
gst_vaapi_display_get_subpicture_caps(GstVaapiDisplay *display)
{
    g_return_val_if_fail(GST_VAAPI_IS_DISPLAY(display), NULL);

    return get_caps(display->priv->subpicture_formats);
}

gboolean
gst_vaapi_display_has_subpicture_format(
    GstVaapiDisplay    *display,
    GstVaapiImageFormat format
)
{
    g_return_val_if_fail(GST_VAAPI_IS_DISPLAY(display), FALSE);
    g_return_val_if_fail(format, FALSE);

    return find_format(display->priv->subpicture_formats, format);
}
