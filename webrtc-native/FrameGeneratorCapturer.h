#pragma once

#include "TestVideoCapturer.h"
#include "FrameGenerator.h"

namespace webrtc {

    class FrameGenerator;

    class FrameGeneratorCapturer : public TestVideoCapturer {
    public:
        class SinkWantsObserver {
        public:
            // OnSinkWantsChanged is called when FrameGeneratorCapturer::AddOrUpdateSink
            // is called.
            virtual void OnSinkWantsChanged(rtc::VideoSinkInterface<VideoFrame>* sink,
                const rtc::VideoSinkWants& wants) = 0;

        protected:
            virtual ~SinkWantsObserver() {}
        };

        // |type| has the default value OutputType::I420. |num_squares| has the
        // default value 10.
        static FrameGeneratorCapturer* Create(
            int width,
            int height,
            absl::optional<FrameGenerator::OutputType> type,
            absl::optional<int> num_squares,
            int target_fps,
            Clock* clock);

        static FrameGeneratorCapturer* CreateFromYuvFile(const std::string& file_name,
            size_t width,
            size_t height,
            int target_fps,
            Clock* clock);

        static FrameGeneratorCapturer* CreateSlideGenerator(int width,
            int height,
            int frame_repeat_count,
            int target_fps,
            Clock* clock);
        virtual ~FrameGeneratorCapturer();

        void Start();
        void Stop();
        void ChangeResolution(size_t width, size_t height);
        void ChangeFramerate(int target_framerate);

        void SetSinkWantsObserver(SinkWantsObserver* observer);

        void AddOrUpdateSink(rtc::VideoSinkInterface<VideoFrame>* sink,
            const rtc::VideoSinkWants& wants) override;
        void RemoveSink(rtc::VideoSinkInterface<VideoFrame>* sink) override;

        void ForceFrame();
        void SetFakeRotation(VideoRotation rotation);
        void SetFakeColorSpace(absl::optional<ColorSpace> color_space);

        int64_t first_frame_capture_time() const { return first_frame_capture_time_; }

        FrameGeneratorCapturer(Clock* clock,
            std::unique_ptr<FrameGenerator> frame_generator,
            int target_fps);
        bool Init();

    private:
        void InsertFrame();
        static bool Run(void* obj);
        int GetCurrentConfiguredFramerate();
        void UpdateFps(int max_fps) RTC_EXCLUSIVE_LOCKS_REQUIRED(&lock_);

        Clock* const clock_;
        bool sending_;
        SinkWantsObserver* sink_wants_observer_ RTC_GUARDED_BY(&lock_);

        rtc::CriticalSection lock_;
        std::unique_ptr<FrameGenerator> frame_generator_;

        int source_fps_ RTC_GUARDED_BY(&lock_);
        int target_capture_fps_ RTC_GUARDED_BY(&lock_);
        absl::optional<int> wanted_fps_ RTC_GUARDED_BY(&lock_);
        VideoRotation fake_rotation_ = kVideoRotation_0;
        absl::optional<ColorSpace> fake_color_space_ RTC_GUARDED_BY(&lock_);

        int64_t first_frame_capture_time_;
        // Must be the last field, so it will be deconstructed first as tasks
        // in the TaskQueue access other fields of the instance of this class.
        rtc::TaskQueue task_queue_;
    };
}  // namespace webrtc