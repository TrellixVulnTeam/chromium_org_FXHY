// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/in_process_renderer_thread.h"

#include "content/renderer/render_process.h"
#include "content/renderer/render_process_impl.h"
#include "content/renderer/render_thread_impl.h"

namespace content {

InProcessRendererThread::InProcessRendererThread(const std::string& channel_id)
    : Thread("Chrome_InProcRendererThread"), channel_id_(channel_id) {
}//调用父类Thread的构造函数，并且将Unix Socket的名字存储在channel_id_中

InProcessRendererThread::~InProcessRendererThread() {
  Stop();
}

void InProcessRendererThread::Init() {
  render_process_.reset(new RenderProcessImpl());//这是一个假的Render进程，只是Browser进程中的一个线程
  new RenderThreadImpl(channel_id_);//描述当前线程，在创建过程中会创建一个Client端的IPC通信通道
}

void InProcessRendererThread::CleanUp() {
  render_process_.reset();

  // It's a little lame to manually set this flag.  But the single process
  // RendererThread will receive the WM_QUIT.  We don't need to assert on
  // this thread, so just force the flag manually.
  // If we want to avoid this, we could create the InProcRendererThread
  // directly with _beginthreadex() rather than using the Thread class.
  // We used to set this flag in the Init function above. However there
  // other threads like WebThread which are created by this thread
  // which resets this flag. Please see Thread::StartWithOptions. Setting
  // this flag to true in Cleanup works around these problems.
  SetThreadWasQuitProperly(true);
}

base::Thread* CreateInProcessRendererThread(const std::string& channel_id) {
  return new InProcessRendererThread(channel_id);
}

}  // namespace content
