/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/importers/proto/graphics_frame_event_parser.h"

#include <inttypes.h>

#include "perfetto/ext/base/utils.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"

namespace perfetto {
namespace trace_processor {

constexpr char kQueueLostMessage[] =
    "Missing queue event. The slice is now a bit extended than it might "
    "actually have been";
GraphicsFrameEventParser::GraphicsFrameEventParser(
    TraceProcessorContext* context)
    : context_(context),
      graphics_event_scope_id_(
          context->storage->InternString("graphics_frame_event")),
      unknown_event_name_id_(context->storage->InternString("unknown_event")),
      no_layer_name_name_id_(context->storage->InternString("no_layer_name")),
      layer_name_key_id_(context->storage->InternString("layer_name")),
      event_type_name_ids_{
          {context->storage->InternString(
               "unspecified_event") /* UNSPECIFIED */,
           context->storage->InternString("Dequeue") /* DEQUEUE */,
           context->storage->InternString("Queue") /* QUEUE */,
           context->storage->InternString("Post") /* POST */,
           context->storage->InternString(
               "AcquireFenceSignaled") /* ACQUIRE_FENCE */,
           context->storage->InternString("Latch") /* LATCH */,
           context->storage->InternString(
               "HWCCompositionQueued") /* HWC_COMPOSITION_QUEUED */,
           context->storage->InternString(
               "FallbackComposition") /* FALLBACK_COMPOSITION */,
           context->storage->InternString(
               "PresentFenceSignaled") /* PRESENT_FENCE */,
           context->storage->InternString(
               "ReleaseFenceSignaled") /* RELEASE_FENCE */,
           context->storage->InternString("Modify") /* MODIFY */,
           context->storage->InternString("Detach") /* DETACH */,
           context->storage->InternString("Attach") /* ATTACH */,
           context->storage->InternString("Cancel") /* CANCEL */}},
      queue_lost_message_id_(
          context->storage->InternString(kQueueLostMessage)) {}

bool GraphicsFrameEventParser::CreateBufferEvent(
    int64_t timestamp,
    GraphicsFrameEventDecoder& event) {
  if (!event.has_buffer_id()) {
    context_->storage->IncrementStats(
        stats::graphics_frame_event_parser_errors);
    PERFETTO_ELOG("GraphicsFrameEvent with missing buffer id field.");
    return false;
  }

  StringId event_name_id = unknown_event_name_id_;
  if (event.has_type()) {
    const auto type = static_cast<size_t>(event.type());
    if (type < event_type_name_ids_.size()) {
      event_name_id = event_type_name_ids_[type];
      graphics_frame_stats_map_[event.buffer_id()][type] = timestamp;
    } else {
      context_->storage->IncrementStats(
          stats::graphics_frame_event_parser_errors);
      PERFETTO_ELOG("GraphicsFrameEvent with unknown type %zu.", type);
    }
  } else {
    context_->storage->IncrementStats(
        stats::graphics_frame_event_parser_errors);
    PERFETTO_ELOG("GraphicsFrameEvent with missing type field.");
  }

  const uint32_t buffer_id = event.buffer_id();
  StringId layer_name_id;

  if (event.has_layer_name()) {
    auto layer_name_str = event.layer_name();
    const base::StringView layer_name(layer_name_str);
    layer_name_id = context_->storage->InternString(layer_name);
  } else {
    layer_name_id = no_layer_name_name_id_;
  }
  char buffer[4096];
  base::StringWriter track_name(buffer, sizeof(buffer));
  track_name.AppendLiteral("Buffer: ");
  track_name.AppendUnsignedInt(buffer_id);

  const StringId track_name_id =
      context_->storage->InternString(track_name.GetStringView());
  const int64_t duration =
      event.has_duration_ns() ? static_cast<int64_t>(event.duration_ns()) : 0;
  uint32_t frame_number = event.has_frame_number() ? event.frame_number() : 0;

  tables::GpuTrackTable::Row track(track_name_id);
  track.scope = graphics_event_scope_id_;
  TrackId track_id = context_->track_tracker->InternGpuTrack(track);

  {
    tables::GraphicsFrameSliceTable::Row row;
    row.ts = timestamp;
    row.track_id = track_id;
    row.name = event_name_id;
    row.dur = duration;
    row.frame_number = frame_number;
    row.layer_name = layer_name_id;
    if (event.type() == GraphicsFrameEvent::PRESENT_FENCE) {
      auto acquire_ts =
          graphics_frame_stats_map_[event.buffer_id()]
                                   [GraphicsFrameEvent::ACQUIRE_FENCE];
      auto queue_ts = graphics_frame_stats_map_[event.buffer_id()]
                                               [GraphicsFrameEvent::QUEUE];
      auto latch_ts = graphics_frame_stats_map_[event.buffer_id()]
                                               [GraphicsFrameEvent::LATCH];

      row.queue_to_acquire_time =
          std::max(acquire_ts - queue_ts, static_cast<int64_t>(0));
      row.acquire_to_latch_time = latch_ts - acquire_ts;
      row.latch_to_present_time = timestamp - latch_ts;
    }
    auto slice_id = context_->slice_tracker->ScopedFrameEvent(row);
    if (event.type() == GraphicsFrameEvent::DEQUEUE) {
      dequeue_slice_ids_[buffer_id] = slice_id;
    } else if (event.type() == GraphicsFrameEvent::QUEUE) {
      auto it = dequeue_slice_ids_.find(buffer_id);
      if (it != dequeue_slice_ids_.end()) {
        auto dequeue_slice_id = it->second;
        auto* graphics_frame_slice_table =
            context_->storage->mutable_graphics_frame_slice_table();
        uint32_t row_idx =
            *graphics_frame_slice_table->id().IndexOf(dequeue_slice_id);
        graphics_frame_slice_table->mutable_frame_number()->Set(row_idx,
                                                                frame_number);
      }
    }
  }
  return true;
}

// Here we convert the buffer events into Phases(slices)
// APP: Dequeue to Queue
// Wait for GPU: Queue to Acquire
// SurfaceFlinger (SF): Latch to Present
// Display: Present to next Present (of the same layer)
void GraphicsFrameEventParser::CreatePhaseEvent(
    int64_t timestamp,
    GraphicsFrameEventDecoder& event) {
  const uint32_t buffer_id = event.buffer_id();
  uint32_t frame_number = event.has_frame_number() ? event.frame_number() : 0;
  StringId layer_name_id;
  if (event.has_layer_name()) {
    auto layer_name_str = event.layer_name();
    const base::StringView layer_name(layer_name_str);
    layer_name_id = context_->storage->InternString(layer_name);
  } else {
    layer_name_id = no_layer_name_name_id_;
  }
  char track_buffer[4096];
  char slice_buffer[4096];
  // We'll be using the name StringWriter and name_id for writing track names
  // and slice names.
  base::StringWriter track_name(track_buffer, sizeof(track_buffer));
  base::StringWriter slice_name(slice_buffer, sizeof(slice_buffer));
  StringId track_name_id;
  TrackId track_id;
  bool start_slice = true;

  // Close the previous phase before starting the new phase
  switch (event.type()) {
    case GraphicsFrameEvent::DEQUEUE: {
      track_name.reset();
      track_name.AppendLiteral("APP_");
      track_name.AppendUnsignedInt(buffer_id);
      track_name_id =
          context_->storage->InternString(track_name.GetStringView());
      tables::GpuTrackTable::Row app_track(track_name_id);
      app_track.scope = graphics_event_scope_id_;
      track_id = context_->track_tracker->InternGpuTrack(app_track);
      dequeue_map_[buffer_id] = track_id;
      last_dequeued_[buffer_id] = timestamp;
      break;
    }

    case GraphicsFrameEvent::QUEUE: {
      auto dequeueTime = dequeue_map_.find(buffer_id);
      if (dequeueTime != dequeue_map_.end()) {
        const auto opt_slice_id = context_->slice_tracker->EndFrameEvent(
            timestamp, dequeueTime->second);
        slice_name.reset();
        slice_name.AppendUnsignedInt(frame_number);
        if (opt_slice_id) {
          auto* graphics_frame_slice_table =
              context_->storage->mutable_graphics_frame_slice_table();
          // Set the name of the slice to be the frame number since dequeue did
          // not have a frame number at that time.
          uint32_t row_idx =
              *graphics_frame_slice_table->id().IndexOf(*opt_slice_id);
          StringId frame_name_id =
              context_->storage->InternString(slice_name.GetStringView());
          graphics_frame_slice_table->mutable_name()->Set(row_idx,
                                                          frame_name_id);
          graphics_frame_slice_table->mutable_frame_number()->Set(row_idx,
                                                                  frame_number);
          dequeue_map_.erase(dequeueTime);
        }
      }
      // The AcquireFence might be signaled before receiving a QUEUE event
      // sometimes. In that case, we shouldn't start a slice.
      if (last_acquired_[buffer_id] > last_dequeued_[buffer_id] &&
          last_acquired_[buffer_id] < timestamp) {
        start_slice = false;
        break;
      }
      track_name.reset();
      track_name.AppendLiteral("GPU_");
      track_name.AppendUnsignedInt(buffer_id);
      track_name_id =
          context_->storage->InternString(track_name.GetStringView());
      tables::GpuTrackTable::Row gpu_track(track_name_id);
      gpu_track.scope = graphics_event_scope_id_;
      track_id = context_->track_tracker->InternGpuTrack(gpu_track);
      queue_map_[buffer_id] = track_id;
      break;
    }
    case GraphicsFrameEvent::ACQUIRE_FENCE: {
      auto queueTime = queue_map_.find(buffer_id);
      if (queueTime != queue_map_.end()) {
        context_->slice_tracker->EndFrameEvent(timestamp, queueTime->second);
        queue_map_.erase(queueTime);
      }
      last_acquired_[buffer_id] = timestamp;
      start_slice = false;
      break;
    }
    case GraphicsFrameEvent::LATCH: {
      // b/157578286 - Sometimes Queue event goes missing. To prevent having a
      // wrong slice info, we try to close any existing APP slice.
      auto dequeueTime = dequeue_map_.find(buffer_id);
      if (dequeueTime != dequeue_map_.end()) {
        auto args_callback = [this](ArgsTracker::BoundInserter* inserter) {
          inserter->AddArg(context_->storage->InternString("Details"),
                           Variadic::String(queue_lost_message_id_));
        };
        const auto opt_slice_id = context_->slice_tracker->EndFrameEvent(
            timestamp, dequeueTime->second, args_callback);
        slice_name.reset();
        slice_name.AppendUnsignedInt(frame_number);
        if (opt_slice_id) {
          auto* graphics_frame_slice_table =
              context_->storage->mutable_graphics_frame_slice_table();
          // Set the name of the slice to be the frame number since dequeue did
          // not have a frame number at that time.
          uint32_t row_idx =
              *graphics_frame_slice_table->id().IndexOf(*opt_slice_id);
          StringId frame_name_id =
              context_->storage->InternString(slice_name.GetStringView());
          graphics_frame_slice_table->mutable_name()->Set(row_idx,
                                                          frame_name_id);
          graphics_frame_slice_table->mutable_frame_number()->Set(row_idx,
                                                                  frame_number);
          dequeue_map_.erase(dequeueTime);
        }
      }
      track_name.reset();
      track_name.AppendLiteral("SF_");
      track_name.AppendUnsignedInt(buffer_id);
      track_name_id =
          context_->storage->InternString(track_name.GetStringView());
      tables::GpuTrackTable::Row sf_track(track_name_id);
      sf_track.scope = graphics_event_scope_id_;
      track_id = context_->track_tracker->InternGpuTrack(sf_track);
      latch_map_[buffer_id] = track_id;
      break;
    }

    case GraphicsFrameEvent::PRESENT_FENCE: {
      auto latchTime = latch_map_.find(buffer_id);
      if (latchTime != latch_map_.end()) {
        context_->slice_tracker->EndFrameEvent(timestamp, latchTime->second);
        latch_map_.erase(latchTime);
      }
      auto displayTime = display_map_.find(layer_name_id);
      if (displayTime != display_map_.end()) {
        context_->slice_tracker->EndFrameEvent(timestamp, displayTime->second);
        display_map_.erase(displayTime);
      }
      base::StringView layerName(event.layer_name());
      track_name.reset();
      track_name.AppendLiteral("Display_");
      track_name.AppendString(layerName.substr(0, 10));
      track_name_id =
          context_->storage->InternString(track_name.GetStringView());
      tables::GpuTrackTable::Row display_track(track_name_id);
      display_track.scope = graphics_event_scope_id_;
      track_id = context_->track_tracker->InternGpuTrack(display_track);
      display_map_[layer_name_id] = track_id;
      break;
    }

    default:
      start_slice = false;
  }

  // Start the new phase if needed.
  if (start_slice) {
    tables::GraphicsFrameSliceTable::Row slice;
    slice.ts = timestamp;
    slice.track_id = track_id;
    slice.layer_name = layer_name_id;
    slice_name.reset();
    // If the frame_number is known, set it as the name of the slice.
    // If not known (DEQUEUE), set the name as the timestamp.
    // Timestamp is chosen here because the stack_id is hashed based on the name
    // of the slice. To not have any conflicting stack_id with any of the
    // existing slices, we use timestamp as the temporary name.
    if (frame_number != 0) {
      slice_name.AppendUnsignedInt(frame_number);
    } else {
      slice_name.AppendInt(timestamp);
    }
    slice.name = context_->storage->InternString(slice_name.GetStringView());
    slice.frame_number = frame_number;
    context_->slice_tracker->BeginFrameEvent(slice);
  }
}

void GraphicsFrameEventParser::ParseGraphicsFrameEvent(int64_t timestamp,
                                                       ConstBytes blob) {
  protos::pbzero::GraphicsFrameEvent_Decoder frame_event(blob.data, blob.size);
  if (!frame_event.has_buffer_event()) {
    return;
  }

  ConstBytes bufferBlob = frame_event.buffer_event();
  protos::pbzero::GraphicsFrameEvent_BufferEvent_Decoder event(bufferBlob.data,
                                                               bufferBlob.size);
  if (CreateBufferEvent(timestamp, event)) {
    // Create a phase event only if the buffer event finishes successfully
    CreatePhaseEvent(timestamp, event);
  }
}

}  // namespace trace_processor
}  // namespace perfetto
