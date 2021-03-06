// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/aura/mus/capture_synchronizer.h"

#include "base/auto_reset.h"
#include "services/ui/public/interfaces/window_tree.mojom.h"
#include "ui/aura/client/capture_client.h"
#include "ui/aura/mus/capture_synchronizer_delegate.h"
#include "ui/aura/mus/window_mus.h"
#include "ui/aura/window.h"

namespace aura {

CaptureSynchronizer::CaptureSynchronizer(CaptureSynchronizerDelegate* delegate,
                                         ui::mojom::WindowTree* window_tree,
                                         client::CaptureClient* capture_client)
    : delegate_(delegate),
      window_tree_(window_tree),
      capture_client_(capture_client) {
  capture_client_->AddObserver(this);
}

CaptureSynchronizer::~CaptureSynchronizer() {
  SetCaptureWindow(nullptr);
  capture_client_->RemoveObserver(this);
}

void CaptureSynchronizer::SetCaptureFromServer(WindowMus* window) {
  if (window == capture_window_)
    return;

  DCHECK(!setting_capture_);
  // Don't immediately set |capture_client_|. It's possible the change will be
  // rejected.
  base::AutoReset<bool> capture_reset(&setting_capture_, true);
  base::AutoReset<WindowMus*> window_setting_capture_to_reset(
      &window_setting_capture_to_, window);
  capture_client_->SetCapture(window ? window->GetWindow() : nullptr);
}

void CaptureSynchronizer::SetCaptureWindow(WindowMus* window) {
  if (capture_window_)
    capture_window_->GetWindow()->RemoveObserver(this);
  capture_window_ = window;
  if (capture_window_)
    capture_window_->GetWindow()->AddObserver(this);
}

void CaptureSynchronizer::OnWindowDestroying(Window* window) {
  // The CaptureClient implementation handles resetting capture when a window
  // is destroyed, but because of observer ordering this may be called first.
  DCHECK_EQ(window, capture_window_->GetWindow());
  SetCaptureWindow(nullptr);
  // The server will release capture when a window is destroyed, so no need
  // explicitly schedule a change.
}

void CaptureSynchronizer::OnCaptureChanged(Window* lost_capture,
                                           Window* gained_capture) {
  if (!gained_capture && !capture_window_)
    return;  // Happens if the window is deleted during notification.

  WindowMus* gained_capture_mus = WindowMus::Get(gained_capture);
  if (setting_capture_ && gained_capture_mus == window_setting_capture_to_) {
    SetCaptureWindow(gained_capture_mus);
    return;
  }

  const uint32_t change_id =
      delegate_->CreateChangeIdForCapture(capture_window_);
  WindowMus* old_capture_window = capture_window_;
  SetCaptureWindow(gained_capture_mus);
  if (capture_window_)
    window_tree_->SetCapture(change_id, capture_window_->server_id());
  else
    window_tree_->ReleaseCapture(change_id, old_capture_window->server_id());
}

}  // namespace aura
