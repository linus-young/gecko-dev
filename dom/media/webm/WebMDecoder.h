/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#if !defined(WebMDecoder_h_)
#define WebMDecoder_h_

#include "MediaDecoder.h"
#include "MediaFormatReader.h"

namespace mozilla {

class MediaContentType;

class WebMDecoder : public MediaDecoder
{
public:
  explicit WebMDecoder(MediaDecoderOwner* aOwner) : MediaDecoder(aOwner) {}
  MediaDecoder* Clone(MediaDecoderOwner* aOwner) override {
    if (!IsWebMEnabled()) {
      return nullptr;
    }
    return new WebMDecoder(aOwner);
  }
  MediaDecoderStateMachine* CreateStateMachine() override;

  // Returns true if aContentType is a WebM type that we think we can render
  // with an enabled platform decoder backend.
  // If provided, codecs are checked for support.
  static bool IsSupportedType(const MediaContentType& aContentType);

  void GetMozDebugReaderData(nsAString& aString) override;

private:
  RefPtr<MediaFormatReader> mReader;
};

} // namespace mozilla

#endif
