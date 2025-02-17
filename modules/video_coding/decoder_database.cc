/*
 *  Copyright (c) 2018 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/video_coding/decoder_database.h"
#if defined(USE_BUILTIN_SW_CODECS)
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/codecs/i420/include/i420.h"  // nogncheck
#include "modules/video_coding/codecs/vp8/include/vp8.h"    // nogncheck
#include "modules/video_coding/codecs/vp9/include/vp9.h"    // nogncheck
#endif
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"

namespace webrtc {

#if defined(USE_BUILTIN_SW_CODECS)
// Create an internal Decoder given a codec type
static std::unique_ptr<VCMGenericDecoder> CreateDecoder(VideoCodecType type) {
  switch (type) {
    // add by cgb
#ifndef WEBRTC_LINUX
    // case kVideoCodecVP8:
    //   return std::unique_ptr<VCMGenericDecoder>(
    //       new VCMGenericDecoder(VP8Decoder::Create()));
    // case kVideoCodecVP9:
    //   return std::unique_ptr<VCMGenericDecoder>(
    //       new VCMGenericDecoder(VP9Decoder::Create()));
    // case kVideoCodecI420:
    //   return std::unique_ptr<VCMGenericDecoder>(
    //       new VCMGenericDecoder(new I420Decoder()));
    case kVideoCodecH264:
      if (H264Decoder::IsSupported()) {
        return std::unique_ptr<VCMGenericDecoder>(
            new VCMGenericDecoder(H264Decoder::Create()));
      }
      break;
#endif // WEBRTC_LINUX
    default:
      break;
  }
  RTC_LOG(LS_WARNING) << "No internal decoder of this type exists.";
  return std::unique_ptr<VCMGenericDecoder>();
}
#endif

VCMDecoderMapItem::VCMDecoderMapItem(VideoCodec* settings,
                                     int number_of_cores,
                                     bool require_key_frame)
    : settings(settings),
      number_of_cores(number_of_cores),
      require_key_frame(require_key_frame) {
  RTC_DCHECK_GE(number_of_cores, 0);
}

VCMExtDecoderMapItem::VCMExtDecoderMapItem(
    VideoDecoder* external_decoder_instance,
    uint8_t payload_type)
    : payload_type(payload_type),
      external_decoder_instance(external_decoder_instance) {}

VCMDecoderDataBase::VCMDecoderDataBase()
    : receive_codec_(), dec_map_(), dec_external_map_() {}

VCMDecoderDataBase::~VCMDecoderDataBase() {
  ptr_decoder_.reset();
  for (auto& kv : dec_map_)
    delete kv.second;
  for (auto& kv : dec_external_map_)
    delete kv.second;
}

bool VCMDecoderDataBase::DeregisterExternalDecoder(uint8_t payload_type) {
  ExternalDecoderMap::iterator it = dec_external_map_.find(payload_type);
  if (it == dec_external_map_.end()) {
    // Not found
    return false;
  }
  // We can't use payload_type to check if the decoder is currently in use,
  // because payload type may be out of date (e.g. before we decode the first
  // frame after RegisterReceiveCodec)
  if (ptr_decoder_ &&
      ptr_decoder_->IsSameDecoder((*it).second->external_decoder_instance)) {
    // Release it if it was registered and in use.
    ptr_decoder_.reset();
  }
  DeregisterReceiveCodec(payload_type);
  delete it->second;
  dec_external_map_.erase(it);
  return true;
}

// Add the external encoder object to the list of external decoders.
// Won't be registered as a receive codec until RegisterReceiveCodec is called.
void VCMDecoderDataBase::RegisterExternalDecoder(VideoDecoder* external_decoder,
                                                 uint8_t payload_type) {
  // Check if payload value already exists, if so  - erase old and insert new.
  VCMExtDecoderMapItem* ext_decoder =
      new VCMExtDecoderMapItem(external_decoder, payload_type);
  DeregisterExternalDecoder(payload_type);
  dec_external_map_[payload_type] = ext_decoder;
}

bool VCMDecoderDataBase::DecoderRegistered() const {
  return !dec_map_.empty();
}

bool VCMDecoderDataBase::RegisterReceiveCodec(const VideoCodec* receive_codec,
                                              int number_of_cores,
                                              bool require_key_frame) {
  if (number_of_cores < 0) {
    return false;
  }
  // Check if payload value already exists, if so  - erase old and insert new.
  DeregisterReceiveCodec(receive_codec->plType);
  if (receive_codec->codecType == kVideoCodecUnknown) {
    return false;
  }
  VideoCodec* new_receive_codec = new VideoCodec(*receive_codec);
  dec_map_[receive_codec->plType] = new VCMDecoderMapItem(
      new_receive_codec, number_of_cores, require_key_frame);
  return true;
}

bool VCMDecoderDataBase::DeregisterReceiveCodec(uint8_t payload_type) {
  DecoderMap::iterator it = dec_map_.find(payload_type);
  if (it == dec_map_.end()) {
    return false;
  }
  delete it->second;
  dec_map_.erase(it);
  if (receive_codec_.plType == payload_type) {
    // This codec is currently in use.
    memset(&receive_codec_, 0, sizeof(VideoCodec));
  }
  return true;
}

VCMGenericDecoder* VCMDecoderDataBase::GetDecoder(
    const VCMEncodedFrame& frame,
    VCMDecodedFrameCallback* decoded_frame_callback) {
  RTC_DCHECK(decoded_frame_callback->UserReceiveCallback());
  uint8_t payload_type = frame.PayloadType();
  if (payload_type == receive_codec_.plType || payload_type == 0) {
    return ptr_decoder_.get();
  }
  // Check for exisitng decoder, if exists - delete.
  if (ptr_decoder_) {
    ptr_decoder_.reset();
    memset(&receive_codec_, 0, sizeof(VideoCodec));
  }
  ptr_decoder_ = CreateAndInitDecoder(frame, &receive_codec_);
  if (!ptr_decoder_) {
    return nullptr;
  }
  VCMReceiveCallback* callback = decoded_frame_callback->UserReceiveCallback();
  callback->OnIncomingPayloadType(receive_codec_.plType);
  if (ptr_decoder_->RegisterDecodeCompleteCallback(decoded_frame_callback) <
      0) {
    ptr_decoder_.reset();
    memset(&receive_codec_, 0, sizeof(VideoCodec));
    return nullptr;
  }
  return ptr_decoder_.get();
}

VCMGenericDecoder* VCMDecoderDataBase::GetCurrentDecoder() {
  return ptr_decoder_.get();
}

bool VCMDecoderDataBase::PrefersLateDecoding() const {
  return ptr_decoder_ ? ptr_decoder_->PrefersLateDecoding() : true;
}

std::unique_ptr<VCMGenericDecoder> VCMDecoderDataBase::CreateAndInitDecoder(
    const VCMEncodedFrame& frame,
    VideoCodec* new_codec) const {
  uint8_t payload_type = frame.PayloadType();
  RTC_LOG(LS_INFO) << "Initializing decoder with payload type '"
                   << static_cast<int>(payload_type) << "'.";
  RTC_DCHECK(new_codec);
  const VCMDecoderMapItem* decoder_item = FindDecoderItem(payload_type);
  if (!decoder_item) {
    RTC_LOG(LS_ERROR) << "Can't find a decoder associated with payload type: "
                      << static_cast<int>(payload_type);
    return nullptr;
  }
  std::unique_ptr<VCMGenericDecoder> ptr_decoder;
  const VCMExtDecoderMapItem* external_dec_item =
      FindExternalDecoderItem(payload_type);
  if (external_dec_item) {
    // External codec.
    ptr_decoder.reset(new VCMGenericDecoder(
        external_dec_item->external_decoder_instance, true));
  } else {
#if !defined(USE_BUILTIN_SW_CODECS)
    RTC_LOG(LS_ERROR) << "No decoder of this type exists.";
#else
    // Create decoder.
    ptr_decoder = CreateDecoder(decoder_item->settings->codecType);
#endif
  }
  if (!ptr_decoder)
    return nullptr;

  // Copy over input resolutions to prevent codec reinitialization due to
  // the first frame being of a different resolution than the database values.
  // This is best effort, since there's no guarantee that width/height have been
  // parsed yet (and may be zero).
  if (frame.EncodedImage()._encodedWidth > 0 &&
      frame.EncodedImage()._encodedHeight > 0) {
    decoder_item->settings->width = frame.EncodedImage()._encodedWidth;
    decoder_item->settings->height = frame.EncodedImage()._encodedHeight;
  }
  if (ptr_decoder->InitDecode(decoder_item->settings.get(),
                              decoder_item->number_of_cores) < 0) {
    return nullptr;
  }
  memcpy(new_codec, decoder_item->settings.get(), sizeof(VideoCodec));
  return ptr_decoder;
}

const VCMDecoderMapItem* VCMDecoderDataBase::FindDecoderItem(
    uint8_t payload_type) const {
  DecoderMap::const_iterator it = dec_map_.find(payload_type);
  if (it != dec_map_.end()) {
    return (*it).second;
  }
  return nullptr;
}

const VCMExtDecoderMapItem* VCMDecoderDataBase::FindExternalDecoderItem(
    uint8_t payload_type) const {
  ExternalDecoderMap::const_iterator it = dec_external_map_.find(payload_type);
  if (it != dec_external_map_.end()) {
    return (*it).second;
  }
  return nullptr;
}

}  // namespace webrtc
