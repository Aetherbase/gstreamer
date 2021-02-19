/* GStreamer
 * Copyright (C) 2021 Sebastian Dröge <sebastian@centricular.com>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ajaanc/includes/ancillarydata_cea708.h>
#include <ajaanc/includes/ancillarylist.h>
#include <ajantv2/includes/ntv2rp188.h>

#include "gstajacommon.h"
#include "gstajasrc.h"

GST_DEBUG_CATEGORY_STATIC(gst_aja_src_debug);
#define GST_CAT_DEFAULT gst_aja_src_debug

#define DEFAULT_DEVICE_IDENTIFIER ("0")
#define DEFAULT_CHANNEL (::NTV2_CHANNEL1)
// TODO: GST_AJA_VIDEO_FORMAT_AUTO
#define DEFAULT_VIDEO_FORMAT (GST_AJA_VIDEO_FORMAT_1080i_5000)
#define DEFAULT_AUDIO_SYSTEM (GST_AJA_AUDIO_SYSTEM_AUTO)
#define DEFAULT_INPUT_SOURCE (GST_AJA_INPUT_SOURCE_AUTO)
#define DEFAULT_AUDIO_SOURCE (GST_AJA_AUDIO_SOURCE_EMBEDDED)
#define DEFAULT_TIMECODE_INDEX (GST_AJA_TIMECODE_INDEX_VITC)
#define DEFAULT_REFERENCE_SOURCE (GST_AJA_REFERENCE_SOURCE_FREERUN)
#define DEFAULT_QUEUE_SIZE (16)
#define DEFAULT_CAPTURE_CPU_CORE (G_MAXUINT)

enum {
  PROP_0,
  PROP_DEVICE_IDENTIFIER,
  PROP_CHANNEL,
  PROP_VIDEO_FORMAT,
  PROP_AUDIO_SYSTEM,
  PROP_INPUT_SOURCE,
  PROP_AUDIO_SOURCE,
  PROP_TIMECODE_INDEX,
  PROP_REFERENCE_SOURCE,
  PROP_QUEUE_SIZE,
  PROP_CAPTURE_CPU_CORE,
};

typedef enum {
  QUEUE_ITEM_TYPE_FRAME,
} QueueItemType;

typedef struct {
  QueueItemType type;

  // For FRAME
  GstClockTime capture_time;
  GstBuffer *video_buffer;
  GstBuffer *audio_buffer;
  GstBuffer *anc_buffer, *anc_buffer2;
  NTV2_RP188 tc;
} QueueItem;

static void gst_aja_src_set_property(GObject *object, guint property_id,
                                     const GValue *value, GParamSpec *pspec);
static void gst_aja_src_get_property(GObject *object, guint property_id,
                                     GValue *value, GParamSpec *pspec);
static void gst_aja_src_finalize(GObject *object);

static GstCaps *gst_aja_src_get_caps(GstBaseSrc *bsrc, GstCaps *filter);
static gboolean gst_aja_src_query(GstBaseSrc *bsrc, GstQuery *query);
static gboolean gst_aja_src_unlock(GstBaseSrc *bsrc);
static gboolean gst_aja_src_unlock_stop(GstBaseSrc *bsrc);

static GstFlowReturn gst_aja_src_create(GstPushSrc *psrc, GstBuffer **buffer);

static gboolean gst_aja_src_open(GstAjaSrc *src);
static gboolean gst_aja_src_close(GstAjaSrc *src);
static gboolean gst_aja_src_start(GstAjaSrc *src);
static gboolean gst_aja_src_stop(GstAjaSrc *src);

static GstStateChangeReturn gst_aja_src_change_state(GstElement *element,
                                                     GstStateChange transition);

static void capture_thread_func(AJAThread *thread, void *data);

#define parent_class gst_aja_src_parent_class
G_DEFINE_TYPE(GstAjaSrc, gst_aja_src, GST_TYPE_PUSH_SRC);

static void gst_aja_src_class_init(GstAjaSrcClass *klass) {
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  GstBaseSrcClass *basesrc_class = GST_BASE_SRC_CLASS(klass);
  GstPushSrcClass *pushsrc_class = GST_PUSH_SRC_CLASS(klass);
  GstCaps *templ_caps;

  gobject_class->set_property = gst_aja_src_set_property;
  gobject_class->get_property = gst_aja_src_get_property;
  gobject_class->finalize = gst_aja_src_finalize;

  g_object_class_install_property(
      gobject_class, PROP_DEVICE_IDENTIFIER,
      g_param_spec_string(
          "device-identifier", "Device identifier",
          "Input device instance to use", DEFAULT_DEVICE_IDENTIFIER,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                        G_PARAM_CONSTRUCT)));

  g_object_class_install_property(
      gobject_class, PROP_CHANNEL,
      g_param_spec_uint(
          "channel", "Channel", "Channel to use", 0, NTV2_MAX_NUM_CHANNELS - 1,
          DEFAULT_CHANNEL,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                        G_PARAM_CONSTRUCT)));

  g_object_class_install_property(
      gobject_class, PROP_VIDEO_FORMAT,
      g_param_spec_enum(
          "video-format", "Video Format", "Video format to use",
          GST_TYPE_AJA_VIDEO_FORMAT, DEFAULT_VIDEO_FORMAT,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                        G_PARAM_CONSTRUCT)));

  g_object_class_install_property(
      gobject_class, PROP_QUEUE_SIZE,
      g_param_spec_uint(
          "queue-size", "Queue Size",
          "Size of internal queue in number of video frames. "
          "Half of this is allocated as device buffers and equal to the "
          "latency.",
          1, G_MAXINT, DEFAULT_QUEUE_SIZE,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property(
      gobject_class, PROP_AUDIO_SYSTEM,
      g_param_spec_enum(
          "audio-system", "Audio System", "Audio system to use",
          GST_TYPE_AJA_AUDIO_SYSTEM, DEFAULT_AUDIO_SYSTEM,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                        G_PARAM_CONSTRUCT)));

  g_object_class_install_property(
      gobject_class, PROP_INPUT_SOURCE,
      g_param_spec_enum(
          "input-source", "Input Source", "Input source to use",
          GST_TYPE_AJA_INPUT_SOURCE, DEFAULT_INPUT_SOURCE,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                        G_PARAM_CONSTRUCT)));

  g_object_class_install_property(
      gobject_class, PROP_AUDIO_SOURCE,
      g_param_spec_enum(
          "audio-source", "Audio Source", "Audio source to use",
          GST_TYPE_AJA_AUDIO_SOURCE, DEFAULT_AUDIO_SOURCE,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                        G_PARAM_CONSTRUCT)));

  g_object_class_install_property(
      gobject_class, PROP_TIMECODE_INDEX,
      g_param_spec_enum(
          "timecode-index", "Timecode Index", "Timecode index to use",
          GST_TYPE_AJA_TIMECODE_INDEX, DEFAULT_TIMECODE_INDEX,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                        G_PARAM_CONSTRUCT)));

  g_object_class_install_property(
      gobject_class, PROP_REFERENCE_SOURCE,
      g_param_spec_enum(
          "reference-source", "Reference Source", "Reference source to use",
          GST_TYPE_AJA_REFERENCE_SOURCE, DEFAULT_REFERENCE_SOURCE,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                        G_PARAM_CONSTRUCT)));

  g_object_class_install_property(
      gobject_class, PROP_CAPTURE_CPU_CORE,
      g_param_spec_uint(
          "capture-cpu-core", "Capture CPU Core",
          "Sets the affinity of the capture thread to this CPU core "
          "(-1=disabled)",
          0, G_MAXUINT, DEFAULT_CAPTURE_CPU_CORE,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                        G_PARAM_CONSTRUCT)));

  element_class->change_state = GST_DEBUG_FUNCPTR(gst_aja_src_change_state);

  basesrc_class->get_caps = GST_DEBUG_FUNCPTR(gst_aja_src_get_caps);
  basesrc_class->negotiate = NULL;
  basesrc_class->query = GST_DEBUG_FUNCPTR(gst_aja_src_query);
  basesrc_class->unlock = GST_DEBUG_FUNCPTR(gst_aja_src_unlock);
  basesrc_class->unlock_stop = GST_DEBUG_FUNCPTR(gst_aja_src_unlock_stop);

  pushsrc_class->create = GST_DEBUG_FUNCPTR(gst_aja_src_create);

  templ_caps = gst_ntv2_supported_caps(DEVICE_ID_INVALID);
  gst_element_class_add_pad_template(
      element_class,
      gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, templ_caps));
  gst_caps_unref(templ_caps);

  gst_element_class_set_static_metadata(
      element_class, "AJA audio/video src", "Audio/Video/Src",
      "Captures audio/video frames with AJA devices",
      "Sebastian Dröge <sebastian@centricular.com>");

  GST_DEBUG_CATEGORY_INIT(gst_aja_src_debug, "ajasrc", 0, "AJA src");
}

static void gst_aja_src_init(GstAjaSrc *self) {
  g_mutex_init(&self->queue_lock);
  g_cond_init(&self->queue_cond);

  self->device_identifier = g_strdup(DEFAULT_DEVICE_IDENTIFIER);
  self->channel = DEFAULT_CHANNEL;
  self->queue_size = DEFAULT_QUEUE_SIZE;
  self->video_format_setting = DEFAULT_VIDEO_FORMAT;
  self->audio_system_setting = DEFAULT_AUDIO_SYSTEM;
  self->input_source = DEFAULT_INPUT_SOURCE;
  self->audio_source = DEFAULT_AUDIO_SOURCE;
  self->timecode_index = DEFAULT_TIMECODE_INDEX;
  self->reference_source = DEFAULT_REFERENCE_SOURCE;
  self->capture_cpu_core = DEFAULT_CAPTURE_CPU_CORE;

  self->queue =
      gst_queue_array_new_for_struct(sizeof(QueueItem), self->queue_size);
  gst_base_src_set_live(GST_BASE_SRC_CAST(self), TRUE);
  gst_base_src_set_format(GST_BASE_SRC_CAST(self), GST_FORMAT_TIME);
}

void gst_aja_src_set_property(GObject *object, guint property_id,
                              const GValue *value, GParamSpec *pspec) {
  GstAjaSrc *self = GST_AJA_SRC(object);

  switch (property_id) {
    case PROP_DEVICE_IDENTIFIER:
      g_free(self->device_identifier);
      self->device_identifier = g_value_dup_string(value);
      break;
    case PROP_CHANNEL:
      self->channel = (NTV2Channel)g_value_get_uint(value);
      break;
    case PROP_QUEUE_SIZE:
      self->queue_size = g_value_get_uint(value);
      break;
    case PROP_VIDEO_FORMAT:
      self->video_format_setting = (GstAjaVideoFormat)g_value_get_enum(value);
      break;
    case PROP_AUDIO_SYSTEM:
      self->audio_system_setting = (GstAjaAudioSystem)g_value_get_enum(value);
      break;
    case PROP_INPUT_SOURCE:
      self->input_source = (GstAjaInputSource)g_value_get_enum(value);
      break;
    case PROP_AUDIO_SOURCE:
      self->audio_source = (GstAjaAudioSource)g_value_get_enum(value);
      break;
    case PROP_TIMECODE_INDEX:
      self->timecode_index = (GstAjaTimecodeIndex)g_value_get_enum(value);
      break;
    case PROP_REFERENCE_SOURCE:
      self->reference_source = (GstAjaReferenceSource)g_value_get_enum(value);
      break;
    case PROP_CAPTURE_CPU_CORE:
      self->capture_cpu_core = g_value_get_uint(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
      break;
  }
}

void gst_aja_src_get_property(GObject *object, guint property_id, GValue *value,
                              GParamSpec *pspec) {
  GstAjaSrc *self = GST_AJA_SRC(object);

  switch (property_id) {
    case PROP_DEVICE_IDENTIFIER:
      g_value_set_string(value, self->device_identifier);
      break;
    case PROP_CHANNEL:
      g_value_set_uint(value, self->channel);
      break;
    case PROP_QUEUE_SIZE:
      g_value_set_uint(value, self->queue_size);
      break;
    case PROP_VIDEO_FORMAT:
      g_value_set_enum(value, self->video_format_setting);
      break;
    case PROP_AUDIO_SYSTEM:
      g_value_set_enum(value, self->audio_system_setting);
      break;
    case PROP_INPUT_SOURCE:
      g_value_set_enum(value, self->input_source);
      break;
    case PROP_AUDIO_SOURCE:
      g_value_set_enum(value, self->audio_source);
      break;
    case PROP_TIMECODE_INDEX:
      g_value_set_enum(value, self->timecode_index);
      break;
    case PROP_REFERENCE_SOURCE:
      g_value_set_enum(value, self->reference_source);
      break;
    case PROP_CAPTURE_CPU_CORE:
      g_value_set_uint(value, self->capture_cpu_core);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
      break;
  }
}

void gst_aja_src_finalize(GObject *object) {
  GstAjaSrc *self = GST_AJA_SRC(object);

  g_assert(self->device == NULL);
  g_assert(gst_queue_array_get_length(self->queue) == 0);
  g_clear_pointer(&self->queue, gst_queue_array_free);

  g_mutex_clear(&self->queue_lock);
  g_cond_clear(&self->queue_cond);

  G_OBJECT_CLASS(parent_class)->finalize(object);
}

static gboolean gst_aja_src_open(GstAjaSrc *self) {
  GST_DEBUG_OBJECT(self, "Opening device");

  g_assert(self->device == NULL);

  self->device = gst_aja_device_obtain(self->device_identifier);
  if (!self->device) {
    GST_ERROR_OBJECT(self, "Failed to open device");
    return FALSE;
  }

  if (!self->device->device->IsDeviceReady(false)) {
    g_clear_pointer(&self->device, gst_aja_device_unref);
    return FALSE;
  }

  self->device->device->SetEveryFrameServices(::NTV2_OEM_TASKS);
  self->device_id = self->device->device->GetDeviceID();

  std::string serial_number;
  if (!self->device->device->GetSerialNumberString(serial_number))
    serial_number = "none";

  GST_DEBUG_OBJECT(self,
                   "Opened device with ID %d at index %d (%s, version %s, "
                   "serial number %s, can do VANC %d)",
                   self->device_id, self->device->device->GetIndexNumber(),
                   self->device->device->GetDisplayName().c_str(),
                   self->device->device->GetDeviceVersionString().c_str(),
                   serial_number.c_str(),
                   ::NTV2DeviceCanDoCustomAnc(self->device_id));

  GST_DEBUG_OBJECT(self,
                   "Using SDK version %d.%d.%d.%d (%s) and driver version %s",
                   AJA_NTV2_SDK_VERSION_MAJOR, AJA_NTV2_SDK_VERSION_MINOR,
                   AJA_NTV2_SDK_VERSION_POINT, AJA_NTV2_SDK_BUILD_NUMBER,
                   AJA_NTV2_SDK_BUILD_DATETIME,
                   self->device->device->GetDriverVersionString().c_str());

  self->device->device->SetMultiFormatMode(true);

  self->allocator = gst_aja_allocator_new(self->device);

  GST_DEBUG_OBJECT(self, "Opened device");

  return TRUE;
}

static gboolean gst_aja_src_close(GstAjaSrc *self) {
  gst_clear_object(&self->allocator);
  g_clear_pointer(&self->device, gst_aja_device_unref);
  self->device_id = DEVICE_ID_INVALID;

  GST_DEBUG_OBJECT(self, "Closed device");

  return TRUE;
}

static gboolean gst_aja_src_start(GstAjaSrc *self) {
  GST_DEBUG_OBJECT(self, "Starting");

  {
    // Make sure to globally lock here as the routing settings and others are
    // global shared state
    ShmMutexLocker locker;

    switch (self->video_format_setting) {
      // TODO: GST_AJA_VIDEO_FORMAT_AUTO
      case GST_AJA_VIDEO_FORMAT_1080i_5000:
        self->video_format = ::NTV2_FORMAT_1080i_5000;
        break;
      case GST_AJA_VIDEO_FORMAT_1080i_5994:
        self->video_format = ::NTV2_FORMAT_1080i_5994;
        break;
      case GST_AJA_VIDEO_FORMAT_1080i_6000:
        self->video_format = ::NTV2_FORMAT_1080i_6000;
        break;
      case GST_AJA_VIDEO_FORMAT_720p_5994:
        self->video_format = ::NTV2_FORMAT_720p_5994;
        break;
      case GST_AJA_VIDEO_FORMAT_720p_6000:
        self->video_format = ::NTV2_FORMAT_720p_6000;
        break;
      case GST_AJA_VIDEO_FORMAT_1080p_2997:
        self->video_format = ::NTV2_FORMAT_1080p_2997;
        break;
      case GST_AJA_VIDEO_FORMAT_1080p_3000:
        self->video_format = ::NTV2_FORMAT_1080p_3000;
        break;
      case GST_AJA_VIDEO_FORMAT_1080p_2500:
        self->video_format = ::NTV2_FORMAT_1080p_2500;
        break;
      case GST_AJA_VIDEO_FORMAT_1080p_2398:
        self->video_format = ::NTV2_FORMAT_1080p_2398;
        break;
      case GST_AJA_VIDEO_FORMAT_1080p_2400:
        self->video_format = ::NTV2_FORMAT_1080p_2400;
        break;
      case GST_AJA_VIDEO_FORMAT_720p_5000:
        self->video_format = ::NTV2_FORMAT_720p_5000;
        break;
      case GST_AJA_VIDEO_FORMAT_720p_2398:
        self->video_format = ::NTV2_FORMAT_720p_2398;
        break;
      case GST_AJA_VIDEO_FORMAT_720p_2500:
        self->video_format = ::NTV2_FORMAT_720p_2500;
        break;
      case GST_AJA_VIDEO_FORMAT_1080p_5000_A:
        self->video_format = ::NTV2_FORMAT_1080p_5000_A;
        break;
      case GST_AJA_VIDEO_FORMAT_1080p_5994_A:
        self->video_format = ::NTV2_FORMAT_1080p_5994_A;
        break;
      case GST_AJA_VIDEO_FORMAT_1080p_6000_A:
        self->video_format = ::NTV2_FORMAT_1080p_6000_A;
        break;
      case GST_AJA_VIDEO_FORMAT_625_5000:
        self->video_format = ::NTV2_FORMAT_625_5000;
        break;
      case GST_AJA_VIDEO_FORMAT_525_5994:
        self->video_format = ::NTV2_FORMAT_525_5994;
        break;
      case GST_AJA_VIDEO_FORMAT_525_2398:
        self->video_format = ::NTV2_FORMAT_525_2398;
        break;
      case GST_AJA_VIDEO_FORMAT_525_2400:
        self->video_format = ::NTV2_FORMAT_525_2400;
        break;
      default:
        g_assert_not_reached();
        break;
    }

    if (!::NTV2DeviceCanDoVideoFormat(self->device_id, self->video_format)) {
      GST_ERROR_OBJECT(self, "Device does not support mode %d",
                       (int)self->video_format);
      return FALSE;
    }

    gst_clear_caps(&self->configured_caps);
    self->configured_caps = gst_ntv2_video_format_to_caps(self->video_format);
    gst_video_info_from_caps(&self->configured_info, self->configured_caps);

    self->device->device->SetMode(self->channel, NTV2_MODE_CAPTURE, false);

    GST_DEBUG_OBJECT(self, "Configuring video format %d on channel %d",
                     (int)self->video_format, (int)self->channel);
    self->device->device->SetVideoFormat(self->video_format, false, false,
                                         self->channel);

    if (!::NTV2DeviceCanDoFrameBufferFormat(self->device_id,
                                            ::NTV2_FBF_10BIT_YCBCR)) {
      GST_ERROR_OBJECT(self, "Device does not support frame buffer format %d",
                       (int)::NTV2_FBF_10BIT_YCBCR);
      return FALSE;
    }
    self->device->device->SetFrameBufferFormat(self->channel,
                                               ::NTV2_FBF_10BIT_YCBCR);

    self->device->device->DMABufferAutoLock(false, true, 0);

    if (::NTV2DeviceHasBiDirectionalSDI(self->device_id))
      self->device->device->SetSDITransmitEnable(self->channel, false);

    // Always use the framebuffer associated with the channel
    NTV2InputCrosspointID framebuffer_id =
        ::GetFrameBufferInputXptFromChannel(self->channel, false);

    NTV2VANCMode vanc_mode;
    NTV2InputSource input_source;
    NTV2OutputCrosspointID input_source_id;
    switch (self->input_source) {
      case GST_AJA_INPUT_SOURCE_AUTO:
        input_source = ::NTV2ChannelToInputSource(self->channel);
        input_source_id =
            ::GetSDIInputOutputXptFromChannel(self->channel, false);
        vanc_mode = ::NTV2DeviceCanDoCustomAnc(self->device_id)
                        ? ::NTV2_VANCMODE_OFF
                        : ::NTV2_VANCMODE_TALL;
        break;
      case GST_AJA_INPUT_SOURCE_ANALOG1:
        input_source = ::NTV2_INPUTSOURCE_ANALOG1;
        input_source_id = ::NTV2_XptAnalogIn;
        vanc_mode = ::NTV2_VANCMODE_TALL;
        break;
      case GST_AJA_INPUT_SOURCE_HDMI1:
        input_source = ::NTV2_INPUTSOURCE_HDMI1;
        input_source_id = ::NTV2_XptHDMIIn1;
        vanc_mode = ::NTV2_VANCMODE_OFF;
        break;
      case GST_AJA_INPUT_SOURCE_HDMI2:
        input_source = ::NTV2_INPUTSOURCE_HDMI2;
        input_source_id = ::NTV2_XptHDMIIn2;
        vanc_mode = ::NTV2_VANCMODE_OFF;
        break;
      case GST_AJA_INPUT_SOURCE_HDMI3:
        input_source = ::NTV2_INPUTSOURCE_HDMI3;
        input_source_id = ::NTV2_XptHDMIIn3;
        vanc_mode = ::NTV2_VANCMODE_OFF;
        break;
      case GST_AJA_INPUT_SOURCE_HDMI4:
        input_source = ::NTV2_INPUTSOURCE_HDMI4;
        input_source_id = ::NTV2_XptHDMIIn4;
        vanc_mode = ::NTV2_VANCMODE_OFF;
        break;
      case GST_AJA_INPUT_SOURCE_SDI1:
        input_source = ::NTV2_INPUTSOURCE_SDI1;
        input_source_id = ::NTV2_XptSDIIn1;
        vanc_mode = ::NTV2_VANCMODE_TALL;
        break;
      case GST_AJA_INPUT_SOURCE_SDI2:
        input_source = ::NTV2_INPUTSOURCE_SDI2;
        input_source_id = ::NTV2_XptSDIIn2;
        vanc_mode = ::NTV2_VANCMODE_TALL;
        break;
      case GST_AJA_INPUT_SOURCE_SDI3:
        input_source = ::NTV2_INPUTSOURCE_SDI3;
        input_source_id = ::NTV2_XptSDIIn3;
        vanc_mode = ::NTV2_VANCMODE_TALL;
        break;
      case GST_AJA_INPUT_SOURCE_SDI4:
        input_source = ::NTV2_INPUTSOURCE_SDI4;
        input_source_id = ::NTV2_XptSDIIn4;
        vanc_mode = ::NTV2_VANCMODE_TALL;
        break;
      case GST_AJA_INPUT_SOURCE_SDI5:
        input_source = ::NTV2_INPUTSOURCE_SDI5;
        input_source_id = ::NTV2_XptSDIIn5;
        vanc_mode = ::NTV2_VANCMODE_TALL;
        break;
      case GST_AJA_INPUT_SOURCE_SDI6:
        input_source = ::NTV2_INPUTSOURCE_SDI6;
        input_source_id = ::NTV2_XptSDIIn6;
        vanc_mode = ::NTV2_VANCMODE_TALL;
        break;
      case GST_AJA_INPUT_SOURCE_SDI7:
        input_source = ::NTV2_INPUTSOURCE_SDI7;
        input_source_id = ::NTV2_XptSDIIn7;
        vanc_mode = ::NTV2_VANCMODE_TALL;
        break;
      case GST_AJA_INPUT_SOURCE_SDI8:
        input_source = ::NTV2_INPUTSOURCE_SDI8;
        input_source_id = ::NTV2_XptSDIIn8;
        vanc_mode = ::NTV2_VANCMODE_TALL;
        break;
      default:
        g_assert_not_reached();
        break;
    }

    self->configured_input_source = input_source;

    self->vanc_mode = vanc_mode;
    self->device->device->SetEnableVANCData(NTV2_IS_VANCMODE_TALL(vanc_mode),
                                            NTV2_IS_VANCMODE_TALLER(vanc_mode),
                                            self->channel);

    CNTV2SignalRouter router;

    self->device->device->GetRouting(router);

    // Need to remove old routes for the output and framebuffer we're going to
    // use
    NTV2ActualConnections connections = router.GetConnections();

    for (NTV2ActualConnectionsConstIter iter = connections.begin();
         iter != connections.end(); iter++) {
      if (iter->first == framebuffer_id || iter->second == input_source_id)
        router.RemoveConnection(iter->first, iter->second);
    }

    GST_DEBUG_OBJECT(self, "Creating connection %d - %d", framebuffer_id,
                     input_source_id);
    router.AddConnection(framebuffer_id, input_source_id);

    {
      std::stringstream os;
      CNTV2SignalRouter oldRouter;
      self->device->device->GetRouting(oldRouter);
      oldRouter.Print(os);
      GST_DEBUG_OBJECT(self, "Previous routing:\n%s", os.str().c_str());
    }
    self->device->device->ApplySignalRoute(router, true);
    {
      std::stringstream os;
      CNTV2SignalRouter currentRouter;
      self->device->device->GetRouting(currentRouter);
      currentRouter.Print(os);
      GST_DEBUG_OBJECT(self, "New routing:\n%s", os.str().c_str());
    }

    switch (self->audio_system_setting) {
      case GST_AJA_AUDIO_SYSTEM_1:
        self->audio_system = ::NTV2_AUDIOSYSTEM_1;
        break;
      case GST_AJA_AUDIO_SYSTEM_2:
        self->audio_system = ::NTV2_AUDIOSYSTEM_2;
        break;
      case GST_AJA_AUDIO_SYSTEM_3:
        self->audio_system = ::NTV2_AUDIOSYSTEM_3;
        break;
      case GST_AJA_AUDIO_SYSTEM_4:
        self->audio_system = ::NTV2_AUDIOSYSTEM_4;
        break;
      case GST_AJA_AUDIO_SYSTEM_5:
        self->audio_system = ::NTV2_AUDIOSYSTEM_5;
        break;
      case GST_AJA_AUDIO_SYSTEM_6:
        self->audio_system = ::NTV2_AUDIOSYSTEM_6;
        break;
      case GST_AJA_AUDIO_SYSTEM_7:
        self->audio_system = ::NTV2_AUDIOSYSTEM_7;
        break;
      case GST_AJA_AUDIO_SYSTEM_8:
        self->audio_system = ::NTV2_AUDIOSYSTEM_8;
        break;
      case GST_AJA_AUDIO_SYSTEM_AUTO:
        self->audio_system = ::NTV2_AUDIOSYSTEM_1;
        if (::NTV2DeviceGetNumAudioSystems(self->device_id) > 1)
          self->audio_system = ::NTV2ChannelToAudioSystem(self->channel);
        if (!::NTV2DeviceCanDoFrameStore1Display(self->device_id))
          self->audio_system = ::NTV2_AUDIOSYSTEM_1;
        break;
      default:
        g_assert_not_reached();
        break;
    }

    GST_DEBUG_OBJECT(self, "Using audio system %d", self->audio_system);

    NTV2AudioSource audio_source;
    switch (self->audio_source) {
      case GST_AJA_AUDIO_SOURCE_EMBEDDED:
        audio_source = ::NTV2_AUDIO_EMBEDDED;
        break;
      case GST_AJA_AUDIO_SOURCE_AES:
        audio_source = ::NTV2_AUDIO_AES;
        break;
      case GST_AJA_AUDIO_SOURCE_ANALOG:
        audio_source = ::NTV2_AUDIO_ANALOG;
        break;
      case GST_AJA_AUDIO_SOURCE_HDMI:
        audio_source = ::NTV2_AUDIO_HDMI;
        break;
      case GST_AJA_AUDIO_SOURCE_MIC:
        audio_source = ::NTV2_AUDIO_MIC;
        break;
      default:
        g_assert_not_reached();
        break;
    }

    self->device->device->SetAudioSystemInputSource(
        self->audio_system, audio_source,
        ::NTV2InputSourceToEmbeddedAudioInput(input_source));
    self->configured_audio_channels =
        ::NTV2DeviceGetMaxAudioChannels(self->device_id);
    self->device->device->SetNumberAudioChannels(
        self->configured_audio_channels, self->audio_system);
    self->device->device->SetAudioRate(::NTV2_AUDIO_48K, self->audio_system);
    self->device->device->SetAudioBufferSize(::NTV2_AUDIO_BUFFER_BIG,
                                             self->audio_system);
    self->device->device->SetAudioLoopBack(::NTV2_AUDIO_LOOPBACK_OFF,
                                           self->audio_system);
    self->device->device->SetEmbeddedAudioClock(
        ::NTV2_EMBEDDED_AUDIO_CLOCK_VIDEO_INPUT, self->audio_system);

    gst_caps_set_simple(self->configured_caps, "audio-channels", G_TYPE_INT,
                        self->configured_audio_channels, NULL);

    NTV2ReferenceSource reference_source;
    switch (self->reference_source) {
      case GST_AJA_REFERENCE_SOURCE_AUTO:
        reference_source = ::NTV2InputSourceToReferenceSource(input_source);
        break;
      case GST_AJA_REFERENCE_SOURCE_EXTERNAL:
        reference_source = ::NTV2_REFERENCE_EXTERNAL;
        break;
      case GST_AJA_REFERENCE_SOURCE_FREERUN:
        reference_source = ::NTV2_REFERENCE_FREERUN;
        break;
      case GST_AJA_REFERENCE_SOURCE_INPUT_1:
        reference_source = ::NTV2_REFERENCE_INPUT1;
        break;
      case GST_AJA_REFERENCE_SOURCE_INPUT_2:
        reference_source = ::NTV2_REFERENCE_INPUT2;
        break;
      case GST_AJA_REFERENCE_SOURCE_INPUT_3:
        reference_source = ::NTV2_REFERENCE_INPUT3;
        break;
      case GST_AJA_REFERENCE_SOURCE_INPUT_4:
        reference_source = ::NTV2_REFERENCE_INPUT4;
        break;
      case GST_AJA_REFERENCE_SOURCE_INPUT_5:
        reference_source = ::NTV2_REFERENCE_INPUT5;
        break;
      case GST_AJA_REFERENCE_SOURCE_INPUT_6:
        reference_source = ::NTV2_REFERENCE_INPUT6;
        break;
      case GST_AJA_REFERENCE_SOURCE_INPUT_7:
        reference_source = ::NTV2_REFERENCE_INPUT7;
        break;
      case GST_AJA_REFERENCE_SOURCE_INPUT_8:
        reference_source = ::NTV2_REFERENCE_INPUT8;
        break;
      default:
        g_assert_not_reached();
        break;
    }
    GST_DEBUG_OBJECT(self, "Configuring reference source %d",
                     (int)reference_source);

    self->device->device->SetReference(reference_source);

    switch (self->timecode_index) {
      case GST_AJA_TIMECODE_INDEX_VITC:
        self->tc_index = ::NTV2InputSourceToTimecodeIndex(input_source, false);
        break;
      case GST_AJA_TIMECODE_INDEX_ATC_LTC:
        self->tc_index = ::NTV2InputSourceToTimecodeIndex(input_source, true);
        break;
      case GST_AJA_TIMECODE_INDEX_LTC1:
        self->tc_index = ::NTV2_TCINDEX_LTC1;
        break;
      case GST_AJA_TIMECODE_INDEX_LTC2:
        self->tc_index = ::NTV2_TCINDEX_LTC2;
        break;
      default:
        g_assert_not_reached();
        break;
    }
  }

  guint video_buffer_size = ::GetVideoActiveSize(
      self->video_format, ::NTV2_FBF_10BIT_YCBCR, self->vanc_mode);

  self->buffer_pool = gst_buffer_pool_new();
  GstStructure *config = gst_buffer_pool_get_config(self->buffer_pool);
  gst_buffer_pool_config_set_params(config, NULL, video_buffer_size,
                                    2 * self->queue_size, 0);
  gst_buffer_pool_config_set_allocator(config, self->allocator, NULL);
  gst_buffer_pool_set_config(self->buffer_pool, config);
  gst_buffer_pool_set_active(self->buffer_pool, TRUE);

  guint audio_buffer_size = 401 * 1024;

  self->audio_buffer_pool = gst_buffer_pool_new();
  config = gst_buffer_pool_get_config(self->audio_buffer_pool);
  gst_buffer_pool_config_set_params(config, NULL, audio_buffer_size,
                                    2 * self->queue_size, 0);
  gst_buffer_pool_config_set_allocator(config, self->allocator, NULL);
  gst_buffer_pool_set_config(self->audio_buffer_pool, config);
  gst_buffer_pool_set_active(self->audio_buffer_pool, TRUE);

  guint anc_buffer_size = 8 * 1024;

  if (self->vanc_mode == ::NTV2_VANCMODE_OFF) {
    self->anc_buffer_pool = gst_buffer_pool_new();
    config = gst_buffer_pool_get_config(self->anc_buffer_pool);
    gst_buffer_pool_config_set_params(
        config, NULL, anc_buffer_size,
        (self->configured_info.interlace_mode ==
                 GST_VIDEO_INTERLACE_MODE_PROGRESSIVE
             ? 1
             : 2) *
            self->queue_size,
        0);
    gst_buffer_pool_config_set_allocator(config, self->allocator, NULL);
    gst_buffer_pool_set_config(self->anc_buffer_pool, config);
    gst_buffer_pool_set_active(self->anc_buffer_pool, TRUE);
  }

  self->capture_thread = new AJAThread();
  self->capture_thread->Attach(capture_thread_func, self);
  self->capture_thread->SetPriority(AJA_ThreadPriority_High);
  self->capture_thread->Start();
  g_mutex_lock(&self->queue_lock);
  self->shutdown = FALSE;
  self->playing = FALSE;
  self->flushing = FALSE;
  g_cond_signal(&self->queue_cond);
  g_mutex_unlock(&self->queue_lock);

  gst_element_post_message(GST_ELEMENT_CAST(self),
                           gst_message_new_latency(GST_OBJECT_CAST(self)));

  return TRUE;
}

static gboolean gst_aja_src_stop(GstAjaSrc *self) {
  QueueItem *item;

  GST_DEBUG_OBJECT(self, "Stopping");

  g_mutex_lock(&self->queue_lock);
  self->shutdown = TRUE;
  self->flushing = TRUE;
  self->playing = FALSE;
  g_cond_signal(&self->queue_cond);
  g_mutex_unlock(&self->queue_lock);

  if (self->capture_thread) {
    self->capture_thread->Stop();
    delete self->capture_thread;
    self->capture_thread = NULL;
  }

  GST_OBJECT_LOCK(self);
  gst_clear_caps(&self->configured_caps);
  self->configured_audio_channels = 0;
  GST_OBJECT_UNLOCK(self);

  while ((item = (QueueItem *)gst_queue_array_pop_head_struct(self->queue))) {
    if (item->type == QUEUE_ITEM_TYPE_FRAME) {
      gst_clear_buffer(&item->video_buffer);
      gst_clear_buffer(&item->audio_buffer);
      gst_clear_buffer(&item->anc_buffer);
      gst_clear_buffer(&item->anc_buffer2);
    }
  }

  if (self->buffer_pool) {
    gst_buffer_pool_set_active(self->buffer_pool, FALSE);
    gst_clear_object(&self->buffer_pool);
  }

  if (self->audio_buffer_pool) {
    gst_buffer_pool_set_active(self->audio_buffer_pool, FALSE);
    gst_clear_object(&self->audio_buffer_pool);
  }

  if (self->anc_buffer_pool) {
    gst_buffer_pool_set_active(self->anc_buffer_pool, FALSE);
    gst_clear_object(&self->anc_buffer_pool);
  }

  GST_DEBUG_OBJECT(self, "Stopped");

  return TRUE;
}

static GstStateChangeReturn gst_aja_src_change_state(
    GstElement *element, GstStateChange transition) {
  GstAjaSrc *self = GST_AJA_SRC(element);
  GstStateChangeReturn ret;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!gst_aja_src_open(self)) return GST_STATE_CHANGE_FAILURE;
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      if (!gst_aja_src_start(self)) return GST_STATE_CHANGE_FAILURE;
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE) return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      g_mutex_lock(&self->queue_lock);
      self->playing = FALSE;
      g_cond_signal(&self->queue_cond);
      g_mutex_unlock(&self->queue_lock);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      g_mutex_lock(&self->queue_lock);
      self->playing = TRUE;
      g_cond_signal(&self->queue_cond);
      g_mutex_unlock(&self->queue_lock);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      if (!gst_aja_src_stop(self)) return GST_STATE_CHANGE_FAILURE;
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      if (!gst_aja_src_close(self)) return GST_STATE_CHANGE_FAILURE;
      break;
    default:
      break;
  }

  return ret;
}

static GstCaps *gst_aja_src_get_caps(GstBaseSrc *bsrc, GstCaps *filter) {
  GstAjaSrc *self = GST_AJA_SRC(bsrc);
  GstCaps *caps;

  if (self->device) {
    caps = gst_ntv2_supported_caps(self->device_id);
  } else {
    caps = gst_pad_get_pad_template_caps(GST_BASE_SRC_PAD(self));
  }

  if (filter) {
    GstCaps *tmp =
        gst_caps_intersect_full(filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref(caps);
    caps = tmp;
  }

  return caps;
}

static gboolean gst_aja_src_query(GstBaseSrc *bsrc, GstQuery *query) {
  GstAjaSrc *self = GST_AJA_SRC(bsrc);
  gboolean ret = TRUE;

  switch (GST_QUERY_TYPE(query)) {
    case GST_QUERY_LATENCY: {
      if (self->configured_caps) {
        GstClockTime min, max;

        min = gst_util_uint64_scale_ceil(GST_SECOND,
                                         3 * self->configured_info.fps_d,
                                         self->configured_info.fps_n);
        max = self->queue_size * min;

        gst_query_set_latency(query, TRUE, min, max);
        ret = TRUE;
      } else {
        ret = FALSE;
      }

      return ret;
    }

    default:
      return GST_BASE_SRC_CLASS(parent_class)->query(bsrc, query);
      break;
  }
}

static gboolean gst_aja_src_unlock(GstBaseSrc *bsrc) {
  GstAjaSrc *self = GST_AJA_SRC(bsrc);

  g_mutex_lock(&self->queue_lock);
  self->flushing = TRUE;
  g_cond_signal(&self->queue_cond);
  g_mutex_unlock(&self->queue_lock);

  return TRUE;
}

static gboolean gst_aja_src_unlock_stop(GstBaseSrc *bsrc) {
  GstAjaSrc *self = GST_AJA_SRC(bsrc);

  g_mutex_lock(&self->queue_lock);
  self->flushing = FALSE;
  g_mutex_unlock(&self->queue_lock);

  return TRUE;
}

static GstFlowReturn gst_aja_src_create(GstPushSrc *psrc, GstBuffer **buffer) {
  GstAjaSrc *self = GST_AJA_SRC(psrc);
  GstFlowReturn flow_ret = GST_FLOW_OK;
  QueueItem item;

  g_mutex_lock(&self->queue_lock);
  while (gst_queue_array_is_empty(self->queue) && !self->flushing) {
    g_cond_wait(&self->queue_cond, &self->queue_lock);
  }

  if (self->flushing) {
    g_mutex_unlock(&self->queue_lock);
    GST_DEBUG_OBJECT(self, "Flushing");
    return GST_FLOW_FLUSHING;
  }

  item = *(QueueItem *)gst_queue_array_pop_head_struct(self->queue);
  g_mutex_unlock(&self->queue_lock);

  *buffer = item.video_buffer;
  gst_buffer_add_aja_audio_meta(*buffer, item.audio_buffer);
  gst_buffer_unref(item.audio_buffer);

  if (item.tc.IsValid()) {
    TimecodeFormat tc_format = ::kTCFormatUnknown;
    GstVideoTimeCodeFlags flags = GST_VIDEO_TIME_CODE_FLAGS_NONE;

    if (self->configured_info.fps_n == 24 && self->configured_info.fps_d == 1) {
      tc_format = kTCFormat24fps;
    } else if (self->configured_info.fps_n == 25 &&
               self->configured_info.fps_d == 1) {
      tc_format = kTCFormat25fps;
    } else if (self->configured_info.fps_n == 30 &&
               self->configured_info.fps_d == 1) {
      tc_format = kTCFormat30fps;
    } else if (self->configured_info.fps_n == 30000 &&
               self->configured_info.fps_d == 1001) {
      tc_format = kTCFormat30fpsDF;
      flags =
          (GstVideoTimeCodeFlags)(flags | GST_VIDEO_TIME_CODE_FLAGS_DROP_FRAME);
    } else if (self->configured_info.fps_n == 48 &&
               self->configured_info.fps_d == 1) {
      tc_format = kTCFormat48fps;
    } else if (self->configured_info.fps_n == 50 &&
               self->configured_info.fps_d == 1) {
      tc_format = kTCFormat50fps;
    } else if (self->configured_info.fps_n == 60 &&
               self->configured_info.fps_d == 1) {
      tc_format = kTCFormat60fps;
    } else if (self->configured_info.fps_n == 60000 &&
               self->configured_info.fps_d == 1001) {
      tc_format = kTCFormat60fpsDF;
      flags =
          (GstVideoTimeCodeFlags)(flags | GST_VIDEO_TIME_CODE_FLAGS_DROP_FRAME);
    }

    if (self->configured_info.interlace_mode !=
        GST_VIDEO_INTERLACE_MODE_PROGRESSIVE)
      flags =
          (GstVideoTimeCodeFlags)(flags | GST_VIDEO_TIME_CODE_FLAGS_INTERLACED);

    CRP188 rp188(item.tc, tc_format);
    guint hours, minutes, seconds, frames;
    rp188.GetRP188Hrs(hours);
    rp188.GetRP188Mins(minutes);
    rp188.GetRP188Secs(seconds);
    rp188.GetRP188Frms(frames);

    GstVideoTimeCode tc;
    gst_video_time_code_init(&tc, self->configured_info.fps_n,
                             self->configured_info.fps_d, NULL, flags, hours,
                             minutes, seconds, frames, 0);
    gst_buffer_add_video_time_code_meta(*buffer, &tc);
  }

  AJAAncillaryList anc_packets;

  if (item.anc_buffer) {
    GstMapInfo map = GST_MAP_INFO_INIT;
    GstMapInfo map2 = GST_MAP_INFO_INIT;

    gst_buffer_map(item.anc_buffer, &map, GST_MAP_READ);
    if (item.anc_buffer2) gst_buffer_map(item.anc_buffer2, &map2, GST_MAP_READ);

    NTV2_POINTER ptr1(map.data, map.size);
    NTV2_POINTER ptr2(map2.data, map2.size);

    AJAAncillaryList::SetFromDeviceAncBuffers(ptr1, ptr2, anc_packets);

    if (item.anc_buffer2) gst_buffer_unmap(item.anc_buffer2, &map2);
    gst_buffer_unmap(item.anc_buffer, &map);
  } else if (self->vanc_mode != ::NTV2_VANCMODE_OFF) {
    GstMapInfo map;

    NTV2FormatDescriptor format_desc(self->video_format, ::NTV2_FBF_10BIT_YCBCR,
                                     self->vanc_mode);

    gst_buffer_map(item.video_buffer, &map, GST_MAP_READ);
    NTV2_POINTER ptr(map.data, map.size);
    AJAAncillaryList::SetFromVANCData(ptr, format_desc, anc_packets);
    gst_buffer_unmap(item.video_buffer, &map);

    guint offset =
        format_desc.RasterLineToByteOffset(format_desc.GetFirstActiveLine());
    guint size = format_desc.GetVisibleRasterBytes();

    gst_buffer_resize(item.video_buffer, offset, size);
  }

  gst_clear_buffer(&item.anc_buffer);
  gst_clear_buffer(&item.anc_buffer2);

  if (anc_packets.CountAncillaryDataWithType(AJAAncillaryDataType_Cea708)) {
    AJAAncillaryData packet =
        anc_packets.GetAncillaryDataWithType(AJAAncillaryDataType_Cea708);

    if (packet.GetPayloadData() && packet.GetPayloadByteCount() &&
        AJA_SUCCESS(packet.ParsePayloadData())) {
      gst_buffer_add_video_caption_meta(
          *buffer, GST_VIDEO_CAPTION_TYPE_CEA708_CDP, packet.GetPayloadData(),
          packet.GetPayloadByteCount());
    }
  }

  // TODO: Add AFD/Bar meta

  if (!gst_pad_has_current_caps(GST_BASE_SRC_PAD(self))) {
    gst_base_src_set_caps(GST_BASE_SRC_CAST(self), self->configured_caps);
  }

  return flow_ret;
}

static void capture_thread_func(AJAThread *thread, void *data) {
  GstAjaSrc *self = GST_AJA_SRC(data);
  GstClock *clock = NULL;
  AUTOCIRCULATE_TRANSFER transfer;
  guint64 frames_dropped_last = G_MAXUINT64;
  gboolean have_signal = TRUE;

  if (self->capture_cpu_core != G_MAXUINT) {
    cpu_set_t mask;
    pthread_t current_thread = pthread_self();

    CPU_ZERO(&mask);
    CPU_SET(self->capture_cpu_core, &mask);

    if (pthread_setaffinity_np(current_thread, sizeof(mask), &mask) != 0) {
      GST_ERROR_OBJECT(self,
                       "Failed to set affinity for current thread to core %u",
                       self->capture_cpu_core);
    }
  }

  g_mutex_lock(&self->queue_lock);
restart:
  GST_DEBUG_OBJECT(self, "Waiting for playing or shutdown");
  while (!self->playing && !self->shutdown)
    g_cond_wait(&self->queue_cond, &self->queue_lock);
  if (self->shutdown) {
    GST_DEBUG_OBJECT(self, "Shutting down");
    g_mutex_unlock(&self->queue_lock);
    return;
  }

  GST_DEBUG_OBJECT(self, "Starting capture");
  g_mutex_unlock(&self->queue_lock);

  // TODO: Wait for stable input signal

  if (!self->device->device->EnableChannel(self->channel)) {
    GST_ELEMENT_ERROR(self, STREAM, FAILED, (NULL),
                      ("Failed to enable channel"));
    goto out;
  }

  {
    // Make sure to globally lock here as the routing settings and others are
    // global shared state
    ShmMutexLocker locker;

    self->device->device->AutoCirculateStop(self->channel);

    self->device->device->EnableInputInterrupt(self->channel);
    self->device->device->SubscribeInputVerticalEvent(self->channel);
    if (!self->device->device->AutoCirculateInitForInput(
            self->channel, self->queue_size / 2, self->audio_system,
            AUTOCIRCULATE_WITH_RP188 |
                (self->vanc_mode == ::NTV2_VANCMODE_OFF ? AUTOCIRCULATE_WITH_ANC
                                                        : 0),
            1)) {
      GST_ELEMENT_ERROR(self, STREAM, FAILED, (NULL),
                        ("Failed to initialize autocirculate"));
      goto out;
    }
    self->device->device->AutoCirculateStart(self->channel);
  }

  gst_clear_object(&clock);
  clock = gst_element_get_clock(GST_ELEMENT_CAST(self));

  frames_dropped_last = G_MAXUINT64;
  have_signal = TRUE;

  g_mutex_lock(&self->queue_lock);
  while (self->playing && !self->shutdown) {
    // Check for valid signal first
    NTV2VideoFormat current_video_format =
        self->device->device->GetInputVideoFormat(
            self->configured_input_source);
    if (current_video_format == ::NTV2_FORMAT_UNKNOWN) {
      GST_DEBUG_OBJECT(self, "No signal, waiting");
      g_mutex_unlock(&self->queue_lock);
      self->device->device->WaitForInputVerticalInterrupt(self->channel);
      frames_dropped_last = G_MAXUINT64;
      if (have_signal) {
        GST_ELEMENT_WARNING(GST_ELEMENT(self), RESOURCE, READ, ("Signal lost"),
                            ("No input source was detected"));
        have_signal = FALSE;
      }
      g_mutex_lock(&self->queue_lock);
      continue;
    } else if (current_video_format != self->video_format) {
      // TODO: Handle GST_AJA_VIDEO_FORMAT_AUTO here
      GST_DEBUG_OBJECT(self,
                       "Different input format %u than configured %u, waiting",
                       current_video_format, self->video_format);
      g_mutex_unlock(&self->queue_lock);
      self->device->device->WaitForInputVerticalInterrupt(self->channel);
      frames_dropped_last = G_MAXUINT64;
      if (have_signal) {
        GST_ELEMENT_WARNING(GST_ELEMENT(self), RESOURCE, READ, ("Signal lost"),
                            ("Different input source was detected"));
        have_signal = FALSE;
      }
      g_mutex_lock(&self->queue_lock);
      continue;
    }

    if (!have_signal) {
      GST_ELEMENT_INFO(GST_ELEMENT(self), RESOURCE, READ, ("Signal recovered"),
                       ("Input source detected"));
      have_signal = TRUE;
    }

    AUTOCIRCULATE_STATUS status;

    self->device->device->AutoCirculateGetStatus(self->channel, status);

    GST_TRACE_OBJECT(self,
                     "Start frame %d "
                     "end frame %d "
                     "active frame %d "
                     "start time %" G_GUINT64_FORMAT
                     " "
                     "current time %" G_GUINT64_FORMAT
                     " "
                     "frames processed %u "
                     "frames dropped %u "
                     "buffer level %u",
                     status.acStartFrame, status.acEndFrame,
                     status.acActiveFrame, status.acRDTSCStartTime,
                     status.acRDTSCCurrentTime, status.acFramesProcessed,
                     status.acFramesDropped, status.acBufferLevel);

    if (frames_dropped_last == G_MAXUINT64) {
      frames_dropped_last = status.acFramesDropped;
    } else if (frames_dropped_last < status.acFramesDropped) {
      GST_WARNING_OBJECT(self, "Dropped %" G_GUINT64_FORMAT " frames",
                         status.acFramesDropped - frames_dropped_last);

      GstClockTime timestamp =
          gst_util_uint64_scale(status.acFramesProcessed + frames_dropped_last,
                                self->configured_info.fps_n,
                                self->configured_info.fps_d * GST_SECOND);
      GstClockTime timestamp_end = gst_util_uint64_scale(
          status.acFramesProcessed + status.acFramesDropped,
          self->configured_info.fps_n,
          self->configured_info.fps_d * GST_SECOND);
      GstMessage *msg = gst_message_new_qos(
          GST_OBJECT_CAST(self), TRUE, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE,
          timestamp, timestamp_end - timestamp);
      gst_element_post_message(GST_ELEMENT_CAST(self), msg);

      frames_dropped_last = status.acFramesDropped;
    }

    if (status.IsRunning() && status.acBufferLevel > 1) {
      GstBuffer *video_buffer = NULL;
      GstBuffer *audio_buffer = NULL;
      GstBuffer *anc_buffer = NULL, *anc_buffer2 = NULL;
      GstMapInfo video_map = GST_MAP_INFO_INIT;
      GstMapInfo audio_map = GST_MAP_INFO_INIT;
      GstMapInfo anc_map = GST_MAP_INFO_INIT;
      GstMapInfo anc_map2 = GST_MAP_INFO_INIT;
      AUTOCIRCULATE_TRANSFER transfer;

      if (gst_buffer_pool_acquire_buffer(self->buffer_pool, &video_buffer,
                                         NULL) != GST_FLOW_OK) {
        GST_ELEMENT_ERROR(self, STREAM, FAILED, (NULL),
                          ("Failed to acquire video buffer"));
        break;
      }

      if (gst_buffer_pool_acquire_buffer(self->audio_buffer_pool, &audio_buffer,
                                         NULL) != GST_FLOW_OK) {
        gst_buffer_unref(video_buffer);
        GST_ELEMENT_ERROR(self, STREAM, FAILED, (NULL),
                          ("Failed to acquire audio buffer"));
        break;
      }

      if (self->vanc_mode == ::NTV2_VANCMODE_OFF) {
        if (gst_buffer_pool_acquire_buffer(self->anc_buffer_pool, &anc_buffer,
                                           NULL) != GST_FLOW_OK) {
          gst_buffer_unref(audio_buffer);
          gst_buffer_unref(video_buffer);
          GST_ELEMENT_ERROR(self, STREAM, FAILED, (NULL),
                            ("Failed to acquire anc buffer"));
          break;
        }

        if (self->configured_info.interlace_mode !=
            GST_VIDEO_INTERLACE_MODE_PROGRESSIVE) {
          if (gst_buffer_pool_acquire_buffer(
                  self->anc_buffer_pool, &anc_buffer2, NULL) != GST_FLOW_OK) {
            gst_buffer_unref(anc_buffer);
            gst_buffer_unref(audio_buffer);
            gst_buffer_unref(video_buffer);
            GST_ELEMENT_ERROR(self, STREAM, FAILED, (NULL),
                              ("Failed to acquire anc buffer"));
            break;
          }
        }
      }

      gst_buffer_map(video_buffer, &video_map, GST_MAP_READWRITE);
      gst_buffer_map(audio_buffer, &audio_map, GST_MAP_READWRITE);
      if (anc_buffer) gst_buffer_map(anc_buffer, &anc_map, GST_MAP_READWRITE);
      if (anc_buffer2)
        gst_buffer_map(anc_buffer2, &anc_map2, GST_MAP_READWRITE);

      transfer.acFrameBufferFormat = ::NTV2_FBF_10BIT_YCBCR;

      transfer.SetVideoBuffer((ULWord *)video_map.data, video_map.size);
      transfer.SetAudioBuffer((ULWord *)audio_map.data, audio_map.size);
      transfer.SetAncBuffers((ULWord *)anc_map.data, anc_map.size,
                             (ULWord *)anc_map2.data, anc_map2.size);

      g_mutex_unlock(&self->queue_lock);

      bool transfered = true;
      if (!self->device->device->AutoCirculateTransfer(self->channel,
                                                       transfer)) {
        GST_WARNING_OBJECT(self, "Failed to transfer frame");
        transfered = false;
      }

      if (anc_buffer2) gst_buffer_unmap(anc_buffer2, &anc_map2);
      if (anc_buffer) gst_buffer_unmap(anc_buffer, &anc_map);
      gst_buffer_unmap(audio_buffer, &audio_map);
      gst_buffer_unmap(video_buffer, &video_map);

      g_mutex_lock(&self->queue_lock);

      if (!transfered) {
        gst_clear_buffer(&anc_buffer2);
        gst_clear_buffer(&anc_buffer);
        gst_clear_buffer(&audio_buffer);
        gst_clear_buffer(&video_buffer);
        continue;
      }

      gst_buffer_set_size(audio_buffer, transfer.GetCapturedAudioByteCount());
      if (anc_buffer)
        gst_buffer_set_size(anc_buffer,
                            transfer.GetCapturedAncByteCount(false));
      if (anc_buffer2)
        gst_buffer_set_size(anc_buffer2,
                            transfer.GetCapturedAncByteCount(true));

      NTV2_RP188 time_code;
      transfer.acTransferStatus.acFrameStamp.GetInputTimeCode(time_code,
                                                              self->tc_index);

      gint64 frame_time = transfer.acTransferStatus.acFrameStamp.acFrameTime;
      gint64 now_sys = g_get_real_time();
      GstClockTime now_gst = gst_clock_get_time(clock);
      if (now_sys * 10 > frame_time) {
        GstClockTime diff = now_sys * 1000 - frame_time * 100;
        if (now_gst > diff)
          now_gst -= diff;
        else
          now_gst = 0;
      }

      GstClockTime base_time =
          gst_element_get_base_time(GST_ELEMENT_CAST(self));
      if (now_gst > base_time)
        now_gst -= base_time;
      else
        now_gst = 0;

      GST_BUFFER_PTS(video_buffer) = now_gst;
      GST_BUFFER_PTS(audio_buffer) = now_gst;

      // TODO: Drift detection and compensation

      QueueItem item = {.type = QUEUE_ITEM_TYPE_FRAME,
                        .capture_time = now_gst,
                        .video_buffer = video_buffer,
                        .audio_buffer = audio_buffer,
                        .anc_buffer = anc_buffer,
                        .anc_buffer2 = anc_buffer2,
                        .tc = time_code};

      while (gst_queue_array_get_length(self->queue) >= self->queue_size) {
        QueueItem *tmp =
            (QueueItem *)gst_queue_array_pop_head_struct(self->queue);

        if (tmp->type == QUEUE_ITEM_TYPE_FRAME) {
          GST_WARNING_OBJECT(self, "Element queue overrun, dropping old frame");

          GstMessage *msg = gst_message_new_qos(
              GST_OBJECT_CAST(self), TRUE, GST_CLOCK_TIME_NONE,
              GST_CLOCK_TIME_NONE, tmp->capture_time,
              gst_util_uint64_scale(GST_SECOND, self->configured_info.fps_d,
                                    self->configured_info.fps_n));
          gst_element_post_message(GST_ELEMENT_CAST(self), msg);

          gst_clear_buffer(&tmp->video_buffer);
          gst_clear_buffer(&tmp->audio_buffer);
          gst_clear_buffer(&tmp->anc_buffer);
          gst_clear_buffer(&tmp->anc_buffer2);
        }
      }

      GST_TRACE_OBJECT(self, "Queuing frame %" GST_TIME_FORMAT,
                       GST_TIME_ARGS(now_gst));
      gst_queue_array_push_tail_struct(self->queue, &item);
      GST_TRACE_OBJECT(self, "%u frames queued",
                       gst_queue_array_get_length(self->queue));
      g_cond_signal(&self->queue_cond);

    } else {
      g_mutex_unlock(&self->queue_lock);
      self->device->device->WaitForInputVerticalInterrupt(self->channel);
      g_mutex_lock(&self->queue_lock);
    }
  }

out : {
  // Make sure to globally lock here as the routing settings and others are
  // global shared state
  ShmMutexLocker locker;

  self->device->device->AutoCirculateStop(self->channel);
  self->device->device->UnsubscribeInputVerticalEvent(self->channel);
  self->device->device->DisableInputInterrupt(self->channel);
}

  if (!self->playing && !self->shutdown) goto restart;
  g_mutex_unlock(&self->queue_lock);

  gst_clear_object(&clock);

  GST_DEBUG_OBJECT(self, "Stopped");
}
