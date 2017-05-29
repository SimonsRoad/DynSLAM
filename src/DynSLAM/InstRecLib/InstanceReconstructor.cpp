

#include "InstanceReconstructor.h"
#include "InstanceView.h"

#include <vector>

namespace instreclib {
namespace reconstruction {

using namespace std;
using namespace instreclib::segmentation;
using namespace instreclib::utils;
using namespace ITMLib::Objects;

// TODO(andrei): Implement this in CUDA. It should be easy.
template <typename DEPTH_T>
void ProcessSilhouette_CPU(Vector4u *sourceRGB, DEPTH_T *sourceDepth, Vector4u *destRGB,
                           DEPTH_T *destDepth, Vector2i sourceDims,
                           const InstanceDetection &detection) {
  // Blanks out the detection's silhouette in the 'source' frames, and writes
  // its pixels into
  // the output frames.
  // Initially, the dest frames will be the same size as the source ones, but
  // this is wasteful
  // in terms of memory: we should use bbox+1-sized buffers in the future, since
  // most
  // silhouettes are relatively small wrt the size of the whole frame.
  //
  // Moreover, we should be able to pass in several output buffer addresses and
  // a list of
  // detections to the CUDA kernel, and do all the ``splitting up'' work in one
  // kernel call. We
  // may need to add support for the adaptive-size output buffers, since
  // otherwise writing to
  // e.g., 5-6 output buffers may end up using up way too much GPU memory.

  int frame_width = sourceDims[0];
  int frame_height = sourceDims[1];
  const BoundingBox &bbox = detection.GetBoundingBox();

  int box_width = bbox.GetWidth();
  int box_height = bbox.GetHeight();

  memset(destRGB, 0, frame_width * frame_height * sizeof(*sourceRGB));
  memset(destDepth, 0, frame_width * frame_height * sizeof(DEPTH_T));

  for (int row = 0; row < box_height; ++row) {
    for (int col = 0; col < box_width; ++col) {
      int frame_row = row + bbox.r.y0;
      int frame_col = col + bbox.r.x0;
      // TODO(andrei): Are the CPU-specific InfiniTAM functions doing this in a
      // nicer way?

      if (frame_row < 0 || frame_row >= frame_height ||
          frame_col < 0 || frame_col >= frame_width) {
        continue;
      }

      int frame_idx = frame_row * frame_width + frame_col;
      u_char mask_val = detection.mask->GetMaskData()->at<u_char>(row, col);
      if (mask_val == 1) {
        destRGB[frame_idx].r = sourceRGB[frame_idx].r;
        destRGB[frame_idx].g = sourceRGB[frame_idx].g;
        destRGB[frame_idx].b = sourceRGB[frame_idx].b;
        destRGB[frame_idx].a = sourceRGB[frame_idx].a;
        sourceRGB[frame_idx].r = 0;
        sourceRGB[frame_idx].g = 0;
        sourceRGB[frame_idx].b = 0;
        sourceRGB[frame_idx].a = 0;

        destDepth[frame_idx] = sourceDepth[frame_idx];
        sourceDepth[frame_idx] = 0.0f;
      }
    }
  }
}

void InstanceReconstructor::ProcessFrame(
    ITMLib::Objects::ITMView *main_view,
    const segmentation::InstanceSegmentationResult &segmentation_result) {
  // TODO(andrei): Perform this slicing 100% on the GPU.
  main_view->rgb->UpdateHostFromDevice();
  main_view->depth->UpdateHostFromDevice();

  ORUtils::Vector4<unsigned char> *rgb_data_h =
      main_view->rgb->GetData(MemoryDeviceType::MEMORYDEVICE_CPU);
  float *depth_data_h = main_view->depth->GetData(MemoryDeviceType::MEMORYDEVICE_CPU);

  vector<InstanceView> new_instance_views;
  for (const InstanceDetection &instance_detection : segmentation_result.instance_detections) {
    // At this stage of the project, we only care about cars. In the future,
    // this scheme could
    // be extended to also support other classes, as well as any unknown, but
    // moving, objects.
    if (instance_detection.class_id == kPascalVoc2012.label_to_id.at("car")) {
      Vector2i frame_size = main_view->rgb->noDims;
      // bool use_gpu = main_view->rgb->isAllocated_CUDA; // May need to modify
      // 'MemoryBlock' to
      // check this, since the field is private.
      bool use_gpu = true;
      auto view = make_shared<ITMView>(main_view->calib, frame_size, frame_size, use_gpu);
      auto rgb_segment_h = view->rgb->GetData(MemoryDeviceType::MEMORYDEVICE_CPU);
      auto depth_segment_h = view->depth->GetData(MemoryDeviceType::MEMORYDEVICE_CPU);

      ProcessSilhouette_CPU(rgb_data_h, depth_data_h, rgb_segment_h, depth_segment_h,
                            main_view->rgb->noDims, instance_detection);

      view->rgb->UpdateDeviceFromHost();
      view->depth->UpdateDeviceFromHost();

      new_instance_views.emplace_back(instance_detection, view);
    }
  }

  // Associate this frame's detection(s) with those from previous frames.
  this->instance_tracker_->ProcessInstanceViews(frame_idx_, new_instance_views);

  this->ProcessReconstructions();

  main_view->rgb->UpdateDeviceFromHost();
  main_view->depth->UpdateDeviceFromHost();

  // ``Graphically'' display the object tracks for debugging.
  for (const Track &track : this->instance_tracker_->GetTracks()) {
    cout << "Track: " << track.GetAsciiArt() << endl;
  }

  frame_idx_++;
}

ITMUChar4Image *InstanceReconstructor::GetInstancePreviewRGB(size_t track_idx) {
  const auto &tracks = instance_tracker_->GetTracks();
  if (tracks.empty()) {
    return nullptr;
  }

  size_t idx = track_idx;
  if (idx >= tracks.size()) {
    idx = tracks.size() - 1;
  }

  return tracks[idx].GetLastFrame().instance_view.GetView()->rgb;
}

ITMFloatImage *InstanceReconstructor::GetInstancePreviewDepth(size_t track_idx) {
  const auto &tracks = instance_tracker_->GetTracks();
  if (tracks.empty()) {
    return nullptr;
  }

  size_t idx = track_idx;
  if (idx >= tracks.size()) {
    idx = tracks.size() - 1;
  }

  return tracks[idx].GetLastFrame().instance_view.GetView()->depth;
}
void InstanceReconstructor::ProcessReconstructions() {
  for (Track &track : instance_tracker_->GetTracks()) {
    // TODO(andrei): proper heuristic to determine which tracks are worth
    // reconstructing, e.g.,
    // based on slice surface, length, gaps, etc.

    // TODO wait until a track is ``good enough,'' then retroactively rebuild it from all
    // available frames.

    // Since this is very memory-hungry, we restrict creation to the very
    // first things we see.
//    if (track.GetId() < 3 || track.GetId() == 5) {
    if (track.GetId() < 0) {
      if (id_to_reconstruction_.find(track.GetId()) == id_to_reconstruction_.cend()) {
        cout << endl << endl;
        cout << "Starting to reconstruct instance with ID: " << track.GetId() << endl;
        ITMLibSettings *settings = new ITMLibSettings(*driver->GetSettings());

        // Set a much smaller voxel block number for the reconstruction, since individual
        // objects occupy a limited amount of space in the scene.
        settings->sdfLocalBlockNum = 1500;
        // We don't want to create an (expensive) meshing engine for every instance.
        settings->createMeshingEngine = false;

        id_to_reconstruction_.emplace(make_pair(
            track.GetId(),
            new InfiniTamDriver(settings,
                                driver->GetView()->calib,
                                driver->GetView()->rgb->noDims,
                                driver->GetView()->rgb->noDims)));
      } else {
        // TODO(andrei): Use some heuristic to avoid cases which are obviously
        // crappy.
        cout << "Continuing to reconstruct instance with ID: " << track.GetId() << endl;
      }

      // This doesn't seem necessary, since we nab the instance view after the
      // "global"
      // UpdateView which processes the depth.
      //          id_to_reconstruction_[track.GetId()]->UpdateView(rgb,
      //          depth);
      // This replaces the "UpdateView" call.
      InfiniTamDriver *instance_driver = id_to_reconstruction_[track.GetId()];
      instance_driver->SetView(track.GetLastFrame().instance_view.GetView());
      // TODO(andrei): This seems like the place to shove in e.g., scene flow
      // data.

      cout << endl << endl << "Start instance integration for #" << track.GetId() << endl;
      instance_driver->Track();
      instance_driver->Integrate();
      instance_driver->PrepareNextStep();

      cout << endl << endl << "Finished instance integration." << endl;
    } else {
      cout << "Won't create voxel volume for instance #" << track.GetId() << " in the current"
           << " experimental mode." << endl;
    }
  }
}

}  // namespace reconstruction
}  // namespace instreclib