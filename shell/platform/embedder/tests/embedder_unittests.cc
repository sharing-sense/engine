// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include <string>

#include "embedder.h"
#include "flutter/fml/file.h"
#include "flutter/fml/make_copyable.h"
#include "flutter/fml/mapping.h"
#include "flutter/fml/message_loop.h"
#include "flutter/fml/synchronization/count_down_latch.h"
#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/fml/thread.h"
#include "flutter/runtime/dart_vm.h"
#include "flutter/shell/platform/embedder/tests/embedder_assertions.h"
#include "flutter/shell/platform/embedder/tests/embedder_config_builder.h"
#include "flutter/shell/platform/embedder/tests/embedder_test.h"
#include "flutter/testing/testing.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/tonic/converter/dart_converter.h"

namespace flutter {
namespace testing {

using EmbedderTest = testing::EmbedderTest;

TEST(EmbedderTestNoFixture, MustNotRunWithInvalidArgs) {
  EmbedderTestContext context;
  EmbedderConfigBuilder builder(
      context, EmbedderConfigBuilder::InitializationPreference::kNoInitialize);
  auto engine = builder.LaunchEngine();
  ASSERT_FALSE(engine.is_valid());
}

TEST_F(EmbedderTest, CanLaunchAndShutdownWithValidProjectArgs) {
  auto& context = GetEmbedderContext();
  fml::AutoResetWaitableEvent latch;
  context.AddIsolateCreateCallback([&latch]() { latch.Signal(); });
  EmbedderConfigBuilder builder(context);
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Wait for the root isolate to launch.
  latch.Wait();
  engine.reset();
}

TEST_F(EmbedderTest, CanLaunchAndShutdownMultipleTimes) {
  EmbedderConfigBuilder builder(GetEmbedderContext());
  for (size_t i = 0; i < 3; ++i) {
    auto engine = builder.LaunchEngine();
    ASSERT_TRUE(engine.is_valid());
    FML_LOG(INFO) << "Engine launch count: " << i + 1;
  }
}

TEST_F(EmbedderTest, CanInvokeCustomEntrypoint) {
  auto& context = GetEmbedderContext();
  static fml::AutoResetWaitableEvent latch;
  Dart_NativeFunction entrypoint = [](Dart_NativeArguments args) {
    latch.Signal();
  };
  context.AddNativeCallback("SayHiFromCustomEntrypoint", entrypoint);
  EmbedderConfigBuilder builder(context);
  builder.SetDartEntrypoint("customEntrypoint");
  auto engine = builder.LaunchEngine();
  latch.Wait();
  ASSERT_TRUE(engine.is_valid());
}

TEST_F(EmbedderTest, CanInvokeCustomEntrypointMacro) {
  auto& context = GetEmbedderContext();

  fml::AutoResetWaitableEvent latch1;
  fml::AutoResetWaitableEvent latch2;
  fml::AutoResetWaitableEvent latch3;

  // Can be defined separately.
  auto entry1 = [&latch1](Dart_NativeArguments args) {
    FML_LOG(INFO) << "In Callback 1";
    latch1.Signal();
  };
  auto native_entry1 = CREATE_NATIVE_ENTRY(entry1);
  context.AddNativeCallback("SayHiFromCustomEntrypoint1", native_entry1);

  // Can be wrapped in in the args.
  auto entry2 = [&latch2](Dart_NativeArguments args) {
    FML_LOG(INFO) << "In Callback 2";
    latch2.Signal();
  };
  context.AddNativeCallback("SayHiFromCustomEntrypoint2",
                            CREATE_NATIVE_ENTRY(entry2));

  // Everything can be inline.
  context.AddNativeCallback(
      "SayHiFromCustomEntrypoint3",
      CREATE_NATIVE_ENTRY([&latch3](Dart_NativeArguments args) {
        FML_LOG(INFO) << "In Callback 3";
        latch3.Signal();
      }));

  EmbedderConfigBuilder builder(context);
  builder.SetDartEntrypoint("customEntrypoint1");
  auto engine = builder.LaunchEngine();
  latch1.Wait();
  latch2.Wait();
  latch3.Wait();
  ASSERT_TRUE(engine.is_valid());
}

class EmbedderTestTaskRunner {
 public:
  EmbedderTestTaskRunner(std::function<void(FlutterTask)> on_forward_task)
      : on_forward_task_(on_forward_task) {}

  void SetForwardingTaskRunner(fml::RefPtr<fml::TaskRunner> runner) {
    forwarding_target_ = std::move(runner);
  }

  FlutterTaskRunnerDescription GetEmbedderDescription() {
    FlutterTaskRunnerDescription desc;
    desc.struct_size = sizeof(desc);
    desc.user_data = this;
    desc.runs_task_on_current_thread_callback = [](void* user_data) -> bool {
      return reinterpret_cast<EmbedderTestTaskRunner*>(user_data)
          ->forwarding_target_->RunsTasksOnCurrentThread();
    };
    desc.post_task_callback = [](FlutterTask task, uint64_t target_time_nanos,
                                 void* user_data) -> void {
      auto runner = reinterpret_cast<EmbedderTestTaskRunner*>(user_data);

      auto target_time = fml::TimePoint::FromEpochDelta(
          fml::TimeDelta::FromNanoseconds(target_time_nanos));

      runner->forwarding_target_->PostTaskForTime(
          [task, forwarder = runner->on_forward_task_]() { forwarder(task); },
          target_time);
    };
    return desc;
  }

 private:
  fml::RefPtr<fml::TaskRunner> forwarding_target_;
  std::function<void(FlutterTask)> on_forward_task_;

  FML_DISALLOW_COPY_AND_ASSIGN(EmbedderTestTaskRunner);
};

TEST_F(EmbedderTest, CanSpecifyCustomTaskRunner) {
  auto& context = GetEmbedderContext();
  fml::AutoResetWaitableEvent latch;

  // Run the test on its own thread with a message loop so that it san safely
  // pump its event loop while we wait for all the conditions to be checked.
  fml::Thread thread;
  UniqueEngine engine;
  bool signaled = false;

  EmbedderTestTaskRunner runner([&](FlutterTask task) {
    // There may be multiple tasks posted but we only need to check assertions
    // once.
    if (signaled) {
      // Since we have the baton, return it back to the engine. We don't care
      // about the return value because the engine could be shutting down an it
      // may not actually be able to accept the same.
      FlutterEngineRunTask(engine.get(), &task);
      return;
    }

    signaled = true;
    FML_LOG(INFO) << "Checking assertions.";
    ASSERT_TRUE(engine.is_valid());
    ASSERT_EQ(FlutterEngineRunTask(engine.get(), &task), kSuccess);
    latch.Signal();
  });

  thread.GetTaskRunner()->PostTask([&]() {
    EmbedderConfigBuilder builder(context);
    const auto task_runner_description = runner.GetEmbedderDescription();
    runner.SetForwardingTaskRunner(
        fml::MessageLoop::GetCurrent().GetTaskRunner());
    builder.SetPlatformTaskRunner(&task_runner_description);
    builder.SetDartEntrypoint("invokePlatformTaskRunner");
    engine = builder.LaunchEngine();
    ASSERT_TRUE(engine.is_valid());
  });

  // Signaled when all the assertions are checked.
  latch.Wait();
  FML_LOG(INFO) << "Assertions checked. Killing engine.";
  ASSERT_TRUE(engine.is_valid());

  // Since the engine was started on its own thread, it must be killed there as
  // well.
  fml::AutoResetWaitableEvent kill_latch;
  thread.GetTaskRunner()->PostTask(
      fml::MakeCopyable([&engine, &kill_latch]() mutable {
        engine.reset();
        FML_LOG(INFO) << "Engine killed.";
        kill_latch.Signal();
      }));
  kill_latch.Wait();

  ASSERT_TRUE(signaled);
}

TEST(EmbedderTestNoFixture, CanGetCurrentTimeInNanoseconds) {
  auto point1 = fml::TimePoint::FromEpochDelta(
      fml::TimeDelta::FromNanoseconds(FlutterEngineGetCurrentTime()));
  auto point2 = fml::TimePoint::Now();

  ASSERT_LT((point2 - point1), fml::TimeDelta::FromMilliseconds(1));
}

TEST_F(EmbedderTest, CanCreateOpenGLRenderingEngine) {
  EmbedderConfigBuilder builder(GetEmbedderContext());
  builder.SetOpenGLRendererConfig();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
}

TEST_F(EmbedderTest, IsolateServiceIdSent) {
  auto& context = GetEmbedderContext();
  fml::AutoResetWaitableEvent latch;

  fml::Thread thread;
  UniqueEngine engine;
  std::string isolate_message;

  EmbedderTestTaskRunner runner(
      [&](FlutterTask task) { FlutterEngineRunTask(engine.get(), &task); });

  thread.GetTaskRunner()->PostTask([&]() {
    EmbedderConfigBuilder builder(context);
    const auto task_runner_description = runner.GetEmbedderDescription();
    runner.SetForwardingTaskRunner(
        fml::MessageLoop::GetCurrent().GetTaskRunner());
    builder.SetPlatformTaskRunner(&task_runner_description);
    builder.SetDartEntrypoint("main");
    builder.SetPlatformMessageCallback(
        [&](const FlutterPlatformMessage* message) {
          if (strcmp(message->channel, "flutter/isolate") == 0) {
            isolate_message = {reinterpret_cast<const char*>(message->message),
                               message->message_size};
            latch.Signal();
          }
        });
    engine = builder.LaunchEngine();
    ASSERT_TRUE(engine.is_valid());
  });

  // Wait for the isolate ID message and check its format.
  latch.Wait();
  ASSERT_EQ(isolate_message.find("isolates/"), 0ul);

  // Since the engine was started on its own thread, it must be killed there as
  // well.
  fml::AutoResetWaitableEvent kill_latch;
  thread.GetTaskRunner()->PostTask(
      fml::MakeCopyable([&engine, &kill_latch]() mutable {
        engine.reset();
        kill_latch.Signal();
      }));
  kill_latch.Wait();
}

//------------------------------------------------------------------------------
/// Creates a platform message response callbacks, does NOT send them, and
/// immediately collects the same.
///
TEST_F(EmbedderTest, CanCreateAndCollectCallbacks) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  builder.SetDartEntrypoint("platform_messages_response");
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY([](Dart_NativeArguments args) {}));

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  FlutterPlatformMessageResponseHandle* response_handle = nullptr;
  auto callback = [](const uint8_t* data, size_t size,
                     void* user_data) -> void {};
  auto result = FlutterPlatformMessageCreateResponseHandle(
      engine.get(), callback, nullptr, &response_handle);
  ASSERT_EQ(result, kSuccess);
  ASSERT_NE(response_handle, nullptr);

  result = FlutterPlatformMessageReleaseResponseHandle(engine.get(),
                                                       response_handle);
  ASSERT_EQ(result, kSuccess);
}

//------------------------------------------------------------------------------
/// Sends platform messages to Dart code than simply echoes the contents of the
/// message back to the embedder. The embedder registers a native callback to
/// intercept that message.
///
TEST_F(EmbedderTest, PlatformMessagesCanReceiveResponse) {
  struct Captures {
    fml::AutoResetWaitableEvent latch;
    std::thread::id thread_id;
  };
  Captures captures;

  GetThreadTaskRunner()->PostTask([&]() {
    captures.thread_id = std::this_thread::get_id();
    auto& context = GetEmbedderContext();
    EmbedderConfigBuilder builder(context);
    builder.SetDartEntrypoint("platform_messages_response");

    fml::AutoResetWaitableEvent ready;
    context.AddNativeCallback(
        "SignalNativeTest",
        CREATE_NATIVE_ENTRY(
            [&ready](Dart_NativeArguments args) { ready.Signal(); }));

    auto engine = builder.LaunchEngine();
    ASSERT_TRUE(engine.is_valid());

    static std::string kMessageData = "Hello from embedder.";

    FlutterPlatformMessageResponseHandle* response_handle = nullptr;
    auto callback = [](const uint8_t* data, size_t size,
                       void* user_data) -> void {
      ASSERT_EQ(size, kMessageData.size());
      ASSERT_EQ(strncmp(reinterpret_cast<const char*>(kMessageData.data()),
                        reinterpret_cast<const char*>(data), size),
                0);
      auto captures = reinterpret_cast<Captures*>(user_data);
      ASSERT_EQ(captures->thread_id, std::this_thread::get_id());
      captures->latch.Signal();
    };
    auto result = FlutterPlatformMessageCreateResponseHandle(
        engine.get(), callback, &captures, &response_handle);
    ASSERT_EQ(result, kSuccess);

    FlutterPlatformMessage message = {};
    message.struct_size = sizeof(FlutterPlatformMessage);
    message.channel = "test_channel";
    message.message = reinterpret_cast<const uint8_t*>(kMessageData.data());
    message.message_size = kMessageData.size();
    message.response_handle = response_handle;

    ready.Wait();
    result = FlutterEngineSendPlatformMessage(engine.get(), &message);
    ASSERT_EQ(result, kSuccess);

    result = FlutterPlatformMessageReleaseResponseHandle(engine.get(),
                                                         response_handle);
    ASSERT_EQ(result, kSuccess);
  });

  captures.latch.Wait();
}

//------------------------------------------------------------------------------
/// Tests that a platform message can be sent with no response handle. Instead
/// of the platform message integrity checked via a response handle, a native
/// callback with the response is invoked to assert integrity.
///
TEST_F(EmbedderTest, PlatformMessagesCanBeSentWithoutResponseHandles) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);

  builder.SetDartEntrypoint("platform_messages_no_response");

  const std::string message_data = "Hello but don't call me back.";

  fml::AutoResetWaitableEvent ready, message;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&ready](Dart_NativeArguments args) { ready.Signal(); }));
  context.AddNativeCallback(
      "SignalNativeMessage",
      CREATE_NATIVE_ENTRY(
          ([&message, &message_data](Dart_NativeArguments args) {
            auto received_message = tonic::DartConverter<std::string>::FromDart(
                Dart_GetNativeArgument(args, 0));
            ASSERT_EQ(received_message, message_data);
            message.Signal();
          })));

  auto engine = builder.LaunchEngine();

  ASSERT_TRUE(engine.is_valid());
  ready.Wait();

  FlutterPlatformMessage platform_message = {};
  platform_message.struct_size = sizeof(FlutterPlatformMessage);
  platform_message.channel = "test_channel";
  platform_message.message =
      reinterpret_cast<const uint8_t*>(message_data.data());
  platform_message.message_size = message_data.size();
  platform_message.response_handle = nullptr;  // No response needed.

  auto result =
      FlutterEngineSendPlatformMessage(engine.get(), &platform_message);
  ASSERT_EQ(result, kSuccess);
  message.Wait();
}

//------------------------------------------------------------------------------
/// Tests that a null platform message can be sent.
///
TEST_F(EmbedderTest, NullPlatformMessagesCanBeSent) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);

  builder.SetDartEntrypoint("null_platform_messages");

  fml::AutoResetWaitableEvent ready, message;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&ready](Dart_NativeArguments args) { ready.Signal(); }));
  context.AddNativeCallback(
      "SignalNativeMessage",
      CREATE_NATIVE_ENTRY(([&message](Dart_NativeArguments args) {
        auto received_message = tonic::DartConverter<std::string>::FromDart(
            Dart_GetNativeArgument(args, 0));
        ASSERT_EQ("true", received_message);
        message.Signal();
      })));

  auto engine = builder.LaunchEngine();

  ASSERT_TRUE(engine.is_valid());
  ready.Wait();

  FlutterPlatformMessage platform_message = {};
  platform_message.struct_size = sizeof(FlutterPlatformMessage);
  platform_message.channel = "test_channel";
  platform_message.message = nullptr;
  platform_message.message_size = 0;
  platform_message.response_handle = nullptr;  // No response needed.

  auto result =
      FlutterEngineSendPlatformMessage(engine.get(), &platform_message);
  ASSERT_EQ(result, kSuccess);
  message.Wait();
}

//------------------------------------------------------------------------------
/// Tests that a null platform message cannot be send if the message_size
/// isn't equals to 0.
///
TEST_F(EmbedderTest, InvalidPlatformMessages) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);

  auto engine = builder.LaunchEngine();

  ASSERT_TRUE(engine.is_valid());

  FlutterPlatformMessage platform_message = {};
  platform_message.struct_size = sizeof(FlutterPlatformMessage);
  platform_message.channel = "test_channel";
  platform_message.message = nullptr;
  platform_message.message_size = 1;
  platform_message.response_handle = nullptr;  // No response needed.

  auto result =
      FlutterEngineSendPlatformMessage(engine.get(), &platform_message);
  ASSERT_EQ(result, kInvalidArguments);
}

//------------------------------------------------------------------------------
/// Asserts behavior of FlutterProjectArgs::shutdown_dart_vm_when_done (which is
/// set to true by default in these unit-tests).
///
TEST_F(EmbedderTest, VMShutsDownWhenNoEnginesInProcess) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);

  const auto launch_count = DartVM::GetVMLaunchCount();

  {
    auto engine = builder.LaunchEngine();
    ASSERT_EQ(launch_count + 1u, DartVM::GetVMLaunchCount());
  }

  {
    auto engine = builder.LaunchEngine();
    ASSERT_EQ(launch_count + 2u, DartVM::GetVMLaunchCount());
  }
}

//------------------------------------------------------------------------------
/// These snapshots may be materialized from symbols and the size field may not
/// be relevant. Since this information is redundant, engine launch should not
/// be gated on a non-zero buffer size.
///
TEST_F(EmbedderTest, VMAndIsolateSnapshotSizesAreRedundantInAOTMode) {
  if (!DartVM::IsRunningPrecompiledCode()) {
    GTEST_SKIP();
    return;
  }
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);

  // The fixture sets this up correctly. Intentionally mess up the args.
  builder.GetProjectArgs().vm_snapshot_data_size = 0;
  builder.GetProjectArgs().vm_snapshot_instructions_size = 0;
  builder.GetProjectArgs().isolate_snapshot_data_size = 0;
  builder.GetProjectArgs().isolate_snapshot_instructions_size = 0;

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
}

//------------------------------------------------------------------------------
/// If an incorrectly configured compositor is set on the engine, the engine
/// must fail to launch instead of failing to render a frame at a later point in
/// time.
///
TEST_F(EmbedderTest,
       MustPreventEngineLaunchWhenRequiredCompositorArgsAreAbsent) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  builder.SetCompositor();
  builder.GetCompositor().create_backing_store_callback = nullptr;
  builder.GetCompositor().collect_backing_store_callback = nullptr;
  builder.GetCompositor().present_layers_callback = nullptr;
  auto engine = builder.LaunchEngine();
  ASSERT_FALSE(engine.is_valid());
}

//------------------------------------------------------------------------------
/// Must be able to render to a custom compositor whose render targets are fully
/// complete OpenGL textures.
///
TEST_F(EmbedderTest, CompositorMustBeAbleToRenderToOpenGLFramebuffer) {
  auto& context = GetEmbedderContext();

  context.SetupCompositor();

  context.GetCompositor().SetRenderTargetType(
      EmbedderTestCompositor::RenderTargetType::kOpenGLFramebuffer);

  fml::CountDownLatch latch(3);
  context.GetCompositor().SetNextPresentCallback(
      [&](const FlutterLayer** layers, size_t layers_count) {
        ASSERT_EQ(layers_count, 3u);

        {
          FlutterBackingStore backing_store = *layers[0]->backing_store;
          backing_store.struct_size = sizeof(backing_store);
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0, 0);

          ASSERT_EQ(*layers[0], layer);
        }

        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 42;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;
          layer.size = FlutterSizeMake(123.0, 456.0);
          layer.offset = FlutterPointMake(1.0, 2.0);

          ASSERT_EQ(*layers[1], layer);
        }

        {
          FlutterBackingStore backing_store = *layers[2]->backing_store;
          backing_store.struct_size = sizeof(backing_store);
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[2], layer);
        }

        latch.CountDown();
      });

  EmbedderConfigBuilder builder(context);
  builder.SetOpenGLRendererConfig();
  builder.SetCompositor();
  builder.SetDartEntrypoint("can_composite_platform_views");
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&latch](Dart_NativeArguments args) { latch.CountDown(); }));

  auto engine = builder.LaunchEngine();

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_TRUE(engine.is_valid());

  latch.Wait();
}

//------------------------------------------------------------------------------
/// Must be able to render using a custom compositor whose render targets for
/// the individual layers are OpenGL textures.
///
TEST_F(EmbedderTest, CompositorMustBeAbleToRenderToOpenGLTexture) {
  auto& context = GetEmbedderContext();

  context.SetupCompositor();

  context.GetCompositor().SetRenderTargetType(
      EmbedderTestCompositor::RenderTargetType::kOpenGLTexture);

  fml::CountDownLatch latch(3);
  context.GetCompositor().SetNextPresentCallback(
      [&](const FlutterLayer** layers, size_t layers_count) {
        ASSERT_EQ(layers_count, 3u);

        {
          FlutterBackingStore backing_store = *layers[0]->backing_store;
          backing_store.struct_size = sizeof(backing_store);
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeTexture;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0, 0);

          ASSERT_EQ(*layers[0], layer);
        }

        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 42;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;
          layer.size = FlutterSizeMake(123.0, 456.0);
          layer.offset = FlutterPointMake(1.0, 2.0);

          ASSERT_EQ(*layers[1], layer);
        }

        {
          FlutterBackingStore backing_store = *layers[2]->backing_store;
          backing_store.struct_size = sizeof(backing_store);
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeTexture;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[2], layer);
        }

        latch.CountDown();
      });

  EmbedderConfigBuilder builder(context);
  builder.SetOpenGLRendererConfig();
  builder.SetCompositor();
  builder.SetDartEntrypoint("can_composite_platform_views");
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&latch](Dart_NativeArguments args) { latch.CountDown(); }));

  auto engine = builder.LaunchEngine();

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_TRUE(engine.is_valid());

  latch.Wait();
}

//------------------------------------------------------------------------------
/// Must be able to render using a custom compositor whose render target for the
/// individual layers are software buffers.
///
TEST_F(EmbedderTest, CompositorMustBeAbleToRenderToSoftwareBuffer) {
  auto& context = GetEmbedderContext();

  context.SetupCompositor();

  context.GetCompositor().SetRenderTargetType(
      EmbedderTestCompositor::RenderTargetType::kSoftwareBuffer);

  fml::CountDownLatch latch(3);
  context.GetCompositor().SetNextPresentCallback(
      [&](const FlutterLayer** layers, size_t layers_count) {
        ASSERT_EQ(layers_count, 3u);

        {
          FlutterBackingStore backing_store = *layers[0]->backing_store;
          backing_store.struct_size = sizeof(backing_store);
          backing_store.type = kFlutterBackingStoreTypeSoftware;
          backing_store.did_update = true;
          ASSERT_FLOAT_EQ(
              backing_store.software.row_bytes * backing_store.software.height,
              800 * 4 * 600.0);

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0, 0);

          ASSERT_EQ(*layers[0], layer);
        }

        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 42;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;
          layer.size = FlutterSizeMake(123.0, 456.0);
          layer.offset = FlutterPointMake(1.0, 2.0);

          ASSERT_EQ(*layers[1], layer);
        }

        {
          FlutterBackingStore backing_store = *layers[2]->backing_store;
          backing_store.struct_size = sizeof(backing_store);
          backing_store.type = kFlutterBackingStoreTypeSoftware;
          backing_store.did_update = true;
          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[2], layer);
        }

        latch.CountDown();
      });

  EmbedderConfigBuilder builder(context);
  builder.SetOpenGLRendererConfig();
  builder.SetCompositor();
  builder.SetDartEntrypoint("can_composite_platform_views");
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&latch](Dart_NativeArguments args) { latch.CountDown(); }));

  auto engine = builder.LaunchEngine();

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_TRUE(engine.is_valid());

  latch.Wait();
}

static sk_sp<SkSurface> CreateRenderSurface(const FlutterLayer& layer,
                                            GrContext* context) {
  const auto image_info =
      SkImageInfo::MakeN32Premul(layer.size.width, layer.size.height);
  auto surface =
      SkSurface::MakeRenderTarget(context,                   // context
                                  SkBudgeted::kNo,           // budgeted
                                  image_info,                // image info
                                  1,                         // sample count
                                  kTopLeft_GrSurfaceOrigin,  // surface origin
                                  nullptr,  // surface properties
                                  false     // mipmaps

      );
  FML_CHECK(surface != nullptr);
  return surface;
}

static bool RasterImagesAreSame(sk_sp<SkImage> a, sk_sp<SkImage> b) {
  FML_CHECK(!a->isTextureBacked());
  FML_CHECK(!b->isTextureBacked());

  if (!a || !b) {
    return false;
  }

  SkPixmap pixmapA;
  SkPixmap pixmapB;

  if (!a->peekPixels(&pixmapA)) {
    FML_LOG(ERROR) << "Could not peek pixels of image A.";
    return false;
  }

  if (!b->peekPixels(&pixmapB)) {
    FML_LOG(ERROR) << "Could not peek pixels of image B.";

    return false;
  }

  const auto sizeA = pixmapA.rowBytes() * pixmapA.height();
  const auto sizeB = pixmapB.rowBytes() * pixmapB.height();

  if (sizeA != sizeB) {
    FML_LOG(ERROR) << "Pixmap sizes were inconsistent.";
    return false;
  }

  return ::memcmp(pixmapA.addr(), pixmapB.addr(), sizeA) == 0;
}

bool WriteImageToDisk(const fml::UniqueFD& directory,
                      const std::string& name,
                      sk_sp<SkImage> image) {
  if (!image) {
    return false;
  }

  auto data = image->encodeToData(SkEncodedImageFormat::kPNG, 100);

  if (!data) {
    return false;
  }

  fml::NonOwnedMapping mapping(static_cast<const uint8_t*>(data->data()),
                               data->size());
  return WriteAtomically(directory, name.c_str(), mapping);
}

//------------------------------------------------------------------------------
/// Test the layer structure and pixels rendered when using a custom compositor.
///
TEST_F(EmbedderTest, CompositorMustBeAbleToRenderKnownScene) {
  auto& context = GetEmbedderContext();

  context.SetupCompositor();

  context.GetCompositor().SetRenderTargetType(
      EmbedderTestCompositor::RenderTargetType::kOpenGLTexture);

  fml::CountDownLatch latch(6);

  sk_sp<SkImage> scene_image;
  context.SetNextSceneCallback([&](sk_sp<SkImage> scene) {
    scene_image = std::move(scene);
    latch.CountDown();
  });

  context.GetCompositor().SetNextPresentCallback(
      [&](const FlutterLayer** layers, size_t layers_count) {
        ASSERT_EQ(layers_count, 5u);

        // Layer Root
        {
          FlutterBackingStore backing_store = *layers[0]->backing_store;
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeTexture;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[0], layer);
        }

        // Layer 1
        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 1;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;
          layer.size = FlutterSizeMake(50.0, 150.0);
          layer.offset = FlutterPointMake(20.0, 20.0);

          ASSERT_EQ(*layers[1], layer);
        }

        // Layer 2
        {
          FlutterBackingStore backing_store = *layers[2]->backing_store;
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeTexture;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[2], layer);
        }

        // Layer 3
        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 2;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;
          layer.size = FlutterSizeMake(50.0, 150.0);
          layer.offset = FlutterPointMake(40.0, 40.0);

          ASSERT_EQ(*layers[3], layer);
        }

        // Layer 4
        {
          FlutterBackingStore backing_store = *layers[4]->backing_store;
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeTexture;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[4], layer);
        }

        latch.CountDown();
      });

  context.GetCompositor().SetPlatformViewRendererCallback(
      [&](const FlutterLayer& layer, GrContext* context) -> sk_sp<SkImage> {
        auto surface = CreateRenderSurface(layer, context);
        auto canvas = surface->getCanvas();
        FML_CHECK(canvas != nullptr);

        switch (layer.platform_view->identifier) {
          case 1: {
            SkPaint paint;
            // See dart test for total order.
            paint.setColor(SK_ColorGREEN);
            paint.setAlpha(127);
            const auto& rect =
                SkRect::MakeWH(layer.size.width, layer.size.height);
            canvas->drawRect(rect, paint);
            latch.CountDown();
          } break;
          case 2: {
            SkPaint paint;
            // See dart test for total order.
            paint.setColor(SK_ColorMAGENTA);
            paint.setAlpha(127);
            const auto& rect =
                SkRect::MakeWH(layer.size.width, layer.size.height);
            canvas->drawRect(rect, paint);
            latch.CountDown();
          } break;
          default:
            // Asked to render an unknown platform view.
            FML_CHECK(false)
                << "Test was asked to composite an unknown platform view.";
        }

        return surface->makeImageSnapshot();
      });

  EmbedderConfigBuilder builder(context);
  builder.SetOpenGLRendererConfig();
  builder.SetCompositor();
  builder.SetDartEntrypoint("can_composite_platform_views_with_known_scene");
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&latch](Dart_NativeArguments args) { latch.CountDown(); }));

  auto engine = builder.LaunchEngine();

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_TRUE(engine.is_valid());

  latch.Wait();

  // Render the scene and assert that it matches expectation.
  ASSERT_TRUE(scene_image);
  fml::FileMapping fixture_image_mapping(OpenFixture("compositor.png"));
  ASSERT_GE(fixture_image_mapping.GetSize(), 0u);
  auto encoded_image = SkData::MakeWithoutCopy(
      fixture_image_mapping.GetMapping(), fixture_image_mapping.GetSize());
  auto fixture_image =
      SkImage::MakeFromEncoded(std::move(encoded_image))->makeRasterImage();
  ASSERT_TRUE(fixture_image);
  auto scene_image_subset = scene_image->makeSubset(
      SkIRect::MakeWH(fixture_image->width(), fixture_image->height()));
  ASSERT_TRUE(scene_image_subset);
  const auto images_are_same =
      RasterImagesAreSame(scene_image_subset, fixture_image);
  if (!images_are_same) {
    auto fixtures_fd = OpenFixturesDirectory();
    ASSERT_TRUE(
        WriteImageToDisk(fixtures_fd, "actual.png", scene_image_subset));
    ASSERT_TRUE(
        WriteImageToDisk(fixtures_fd, "expectation.png", fixture_image));
    FML_LOG(ERROR) << "Test compositor did not generated expected images. Got: "
                      "'actual.png' Expected: 'expectation.png' in Directory: "
                   << GetFixturesPath();
  }
  ASSERT_TRUE(images_are_same);
}

}  // namespace testing
}  // namespace flutter
