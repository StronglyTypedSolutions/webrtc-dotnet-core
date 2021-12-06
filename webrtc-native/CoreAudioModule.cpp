#include "pch.h"
#include "CoreAudioModule.h"
#include "CoreAudioDevice.h"

#include "modules/audio_device/audio_device_generic.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/scoped_ref_ptr.h"
#include "system_wrappers/include/metrics.h"

#define CHECKinitialized_() \
  {                         \
    if (!initialized_) {    \
      return -1;            \
    }                       \
  }

#define CHECKinitialized__BOOL() \
  {                              \
    if (!initialized_) {         \
      return false;              \
    }                            \
  }

namespace webrtc {

    rtc::scoped_refptr<AudioDeviceModule> AudioDeviceModule::Create() {
        RTC_LOG(INFO) << __FUNCTION__;

        rtc::scoped_refptr<CoreAudioModule> audioDevice(
            new rtc::RefCountedObject<CoreAudioModule>());

        // Create the audio device .
        if (audioDevice->CreateCoreAudioDevice() == -1) {
            return nullptr;
        }

        // Ensure that the generic audio buffer can communicate with the platform
        // specific parts.
        if (audioDevice->AttachAudioBuffer() == -1) {
            return nullptr;
        }

        return audioDevice;
    }

    CoreAudioModule::CoreAudioModule() {
        RTC_LOG(INFO) << __FUNCTION__;
    }

    int32_t CoreAudioModule::CreateCoreAudioDevice() {
        RTC_LOG(INFO) << __FUNCTION__;
        RTC_LOG(INFO) << "Attempting to use the Windows Core Audio device...";
        if (AudioDeviceWindowsCore::CoreAudioIsSupported()) {
            audio_device_.reset(new AudioDeviceWindowsCore());
            RTC_LOG(INFO) << "Windows Core Audio device is created";
        }

        if (!audio_device_) {
            RTC_LOG(LS_ERROR)
                << "Failed to create the Windows Core Audio device.";
            return -1;
        }
        return 0;
    }

    int32_t CoreAudioModule::AttachAudioBuffer() {
        RTC_LOG(INFO) << __FUNCTION__;
        audio_device_->AttachAudioBuffer(&audio_device_buffer_);
        return 0;
    }

    CoreAudioModule::~CoreAudioModule() {
        RTC_LOG(INFO) << __FUNCTION__;
    }

    int32_t CoreAudioModule::ActiveAudioLayer(AudioLayer* audioLayer) const {
        RTC_LOG(INFO) << __FUNCTION__;
        AudioLayer activeAudio;
        if (audio_device_->ActiveAudioLayer(activeAudio) == -1) {
            return -1;
        }
        *audioLayer = activeAudio;
        return 0;
    }

    int32_t CoreAudioModule::Init() {
        RTC_LOG(INFO) << __FUNCTION__;
        if (initialized_)
            return 0;
        RTC_CHECK(audio_device_);
        AudioDeviceGeneric::InitStatus status = audio_device_->Init();
        RTC_HISTOGRAM_ENUMERATION(
            "WebRTC.Audio.InitializationResult", static_cast<int>(status),
            static_cast<int>(AudioDeviceGeneric::InitStatus::NUM_STATUSES));
        if (status != AudioDeviceGeneric::InitStatus::OK) {
            RTC_LOG(LS_ERROR) << "Audio device initialization failed.";
            return -1;
        }
        initialized_ = true;
        return 0;
    }

    int32_t CoreAudioModule::Terminate() {
        RTC_LOG(INFO) << __FUNCTION__;
        if (!initialized_)
            return 0;
        if (audio_device_->Terminate() == -1) {
            return -1;
        }
        initialized_ = false;
        return 0;
    }

    bool CoreAudioModule::Initialized() const {
        RTC_LOG(INFO) << __FUNCTION__ << ": " << initialized_;
        return initialized_;
    }

    int32_t CoreAudioModule::InitSpeaker() {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        return audio_device_->InitSpeaker();
    }

    int32_t CoreAudioModule::InitMicrophone() {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        return audio_device_->InitMicrophone();
    }

    int32_t CoreAudioModule::SpeakerVolumeIsAvailable(bool* available) {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        bool isAvailable = false;
        if (audio_device_->SpeakerVolumeIsAvailable(isAvailable) == -1) {
            return -1;
        }
        *available = isAvailable;
        RTC_LOG(INFO) << "output: " << isAvailable;
        return 0;
    }

    int32_t CoreAudioModule::SetSpeakerVolume(uint32_t volume) {
        RTC_LOG(INFO) << __FUNCTION__ << "(" << volume << ")";
        CHECKinitialized_();
        return audio_device_->SetSpeakerVolume(volume);
    }

    int32_t CoreAudioModule::SpeakerVolume(uint32_t* volume) const {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        uint32_t level = 0;
        if (audio_device_->SpeakerVolume(level) == -1) {
            return -1;
        }
        *volume = level;
        RTC_LOG(INFO) << "output: " << *volume;
        return 0;
    }

    bool CoreAudioModule::SpeakerIsInitialized() const {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized__BOOL();
        bool isInitialized = audio_device_->SpeakerIsInitialized();
        RTC_LOG(INFO) << "output: " << isInitialized;
        return isInitialized;
    }

    bool CoreAudioModule::MicrophoneIsInitialized() const {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized__BOOL();
        bool isInitialized = audio_device_->MicrophoneIsInitialized();
        RTC_LOG(INFO) << "output: " << isInitialized;
        return isInitialized;
    }

    int32_t CoreAudioModule::MaxSpeakerVolume(uint32_t* maxVolume) const {
        CHECKinitialized_();
        uint32_t maxVol = 0;
        if (audio_device_->MaxSpeakerVolume(maxVol) == -1) {
            return -1;
        }
        *maxVolume = maxVol;
        return 0;
    }

    int32_t CoreAudioModule::MinSpeakerVolume(uint32_t* minVolume) const {
        CHECKinitialized_();
        uint32_t minVol = 0;
        if (audio_device_->MinSpeakerVolume(minVol) == -1) {
            return -1;
        }
        *minVolume = minVol;
        return 0;
    }

    int32_t CoreAudioModule::SpeakerMuteIsAvailable(bool* available) {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        bool isAvailable = false;
        if (audio_device_->SpeakerMuteIsAvailable(isAvailable) == -1) {
            return -1;
        }
        *available = isAvailable;
        RTC_LOG(INFO) << "output: " << isAvailable;
        return 0;
    }

    int32_t CoreAudioModule::SetSpeakerMute(bool enable) {
        RTC_LOG(INFO) << __FUNCTION__ << "(" << enable << ")";
        CHECKinitialized_();
        return audio_device_->SetSpeakerMute(enable);
    }

    int32_t CoreAudioModule::SpeakerMute(bool* enabled) const {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        bool muted = false;
        if (audio_device_->SpeakerMute(muted) == -1) {
            return -1;
        }
        *enabled = muted;
        RTC_LOG(INFO) << "output: " << muted;
        return 0;
    }

    int32_t CoreAudioModule::MicrophoneMuteIsAvailable(bool* available) {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        bool isAvailable = false;
        if (audio_device_->MicrophoneMuteIsAvailable(isAvailable) == -1) {
            return -1;
        }
        *available = isAvailable;
        RTC_LOG(INFO) << "output: " << isAvailable;
        return 0;
    }

    int32_t CoreAudioModule::SetMicrophoneMute(bool enable) {
        RTC_LOG(INFO) << __FUNCTION__ << "(" << enable << ")";
        CHECKinitialized_();
        return (audio_device_->SetMicrophoneMute(enable));
    }

    int32_t CoreAudioModule::MicrophoneMute(bool* enabled) const {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        bool muted = false;
        if (audio_device_->MicrophoneMute(muted) == -1) {
            return -1;
        }
        *enabled = muted;
        RTC_LOG(INFO) << "output: " << muted;
        return 0;
    }

    int32_t CoreAudioModule::MicrophoneVolumeIsAvailable(bool* available) {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        bool isAvailable = false;
        if (audio_device_->MicrophoneVolumeIsAvailable(isAvailable) == -1) {
            return -1;
        }
        *available = isAvailable;
        RTC_LOG(INFO) << "output: " << isAvailable;
        return 0;
    }

    int32_t CoreAudioModule::SetMicrophoneVolume(uint32_t volume) {
        RTC_LOG(INFO) << __FUNCTION__ << "(" << volume << ")";
        CHECKinitialized_();
        return (audio_device_->SetMicrophoneVolume(volume));
    }

    int32_t CoreAudioModule::MicrophoneVolume(uint32_t* volume) const {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        uint32_t level = 0;
        if (audio_device_->MicrophoneVolume(level) == -1) {
            return -1;
        }
        *volume = level;
        RTC_LOG(INFO) << "output: " << *volume;
        return 0;
    }

    int32_t CoreAudioModule::StereoRecordingIsAvailable(
        bool* available) const {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        bool isAvailable = false;
        if (audio_device_->StereoRecordingIsAvailable(isAvailable) == -1) {
            return -1;
        }
        *available = isAvailable;
        RTC_LOG(INFO) << "output: " << isAvailable;
        return 0;
    }

    int32_t CoreAudioModule::SetStereoRecording(bool enable) {
        RTC_LOG(INFO) << __FUNCTION__ << "(" << enable << ")";
        CHECKinitialized_();
        if (audio_device_->RecordingIsInitialized()) {
            RTC_LOG(LERROR)
                << "unable to set stereo mode after recording is initialized";
            return -1;
        }
        if (audio_device_->SetStereoRecording(enable) == -1) {
            if (enable) {
                RTC_LOG(WARNING) << "failed to enable stereo recording";
            }
            return -1;
        }
        int8_t nChannels(1);
        if (enable) {
            nChannels = 2;
        }
        audio_device_buffer_.SetRecordingChannels(nChannels);
        return 0;
    }

    int32_t CoreAudioModule::StereoRecording(bool* enabled) const {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        bool stereo = false;
        if (audio_device_->StereoRecording(stereo) == -1) {
            return -1;
        }
        *enabled = stereo;
        RTC_LOG(INFO) << "output: " << stereo;
        return 0;
    }

    int32_t CoreAudioModule::StereoPlayoutIsAvailable(bool* available) const {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        bool isAvailable = false;
        if (audio_device_->StereoPlayoutIsAvailable(isAvailable) == -1) {
            return -1;
        }
        *available = isAvailable;
        RTC_LOG(INFO) << "output: " << isAvailable;
        return 0;
    }

    int32_t CoreAudioModule::SetStereoPlayout(bool enable) {
        RTC_LOG(INFO) << __FUNCTION__ << "(" << enable << ")";
        CHECKinitialized_();
        if (audio_device_->PlayoutIsInitialized()) {
            RTC_LOG(LERROR)
                << "unable to set stereo mode while playing side is initialized";
            return -1;
        }
        if (audio_device_->SetStereoPlayout(enable)) {
            RTC_LOG(WARNING) << "stereo playout is not supported";
            return -1;
        }
        int8_t nChannels(1);
        if (enable) {
            nChannels = 2;
        }
        audio_device_buffer_.SetPlayoutChannels(nChannels);
        return 0;
    }

    int32_t CoreAudioModule::StereoPlayout(bool* enabled) const {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        bool stereo = false;
        if (audio_device_->StereoPlayout(stereo) == -1) {
            return -1;
        }
        *enabled = stereo;
        RTC_LOG(INFO) << "output: " << stereo;
        return 0;
    }

    int32_t CoreAudioModule::PlayoutIsAvailable(bool* available) {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        bool isAvailable = false;
        if (audio_device_->PlayoutIsAvailable(isAvailable) == -1) {
            return -1;
        }
        *available = isAvailable;
        RTC_LOG(INFO) << "output: " << isAvailable;
        return 0;
    }

    int32_t CoreAudioModule::RecordingIsAvailable(bool* available) {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        bool isAvailable = false;
        if (audio_device_->RecordingIsAvailable(isAvailable) == -1) {
            return -1;
        }
        *available = isAvailable;
        RTC_LOG(INFO) << "output: " << isAvailable;
        return 0;
    }

    int32_t CoreAudioModule::MaxMicrophoneVolume(uint32_t* maxVolume) const {
        CHECKinitialized_();
        uint32_t maxVol(0);
        if (audio_device_->MaxMicrophoneVolume(maxVol) == -1) {
            return -1;
        }
        *maxVolume = maxVol;
        return 0;
    }

    int32_t CoreAudioModule::MinMicrophoneVolume(uint32_t* minVolume) const {
        CHECKinitialized_();
        uint32_t minVol(0);
        if (audio_device_->MinMicrophoneVolume(minVol) == -1) {
            return -1;
        }
        *minVolume = minVol;
        return 0;
    }

    int16_t CoreAudioModule::PlayoutDevices() {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        uint16_t nPlayoutDevices = audio_device_->PlayoutDevices();
        RTC_LOG(INFO) << "output: " << nPlayoutDevices;
        return (int16_t)(nPlayoutDevices);
    }

    int32_t CoreAudioModule::SetPlayoutDevice(uint16_t index) {
        RTC_LOG(INFO) << __FUNCTION__ << "(" << index << ")";
        CHECKinitialized_();
        return audio_device_->SetPlayoutDevice(index);
    }

    int32_t CoreAudioModule::SetPlayoutDevice(WindowsDeviceType device) {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        return audio_device_->SetPlayoutDevice(device);
    }

    int32_t CoreAudioModule::PlayoutDeviceName(
        uint16_t index,
        char name[kAdmMaxDeviceNameSize],
        char guid[kAdmMaxGuidSize]) {
        RTC_LOG(INFO) << __FUNCTION__ << "(" << index << ", ...)";
        CHECKinitialized_();
        if (name == NULL) {
            return -1;
        }
        if (audio_device_->PlayoutDeviceName(index, name, guid) == -1) {
            return -1;
        }
        if (name != NULL) {
            RTC_LOG(INFO) << "output: name = " << name;
        }
        if (guid != NULL) {
            RTC_LOG(INFO) << "output: guid = " << guid;
        }
        return 0;
    }

    int32_t CoreAudioModule::RecordingDeviceName(
        uint16_t index,
        char name[kAdmMaxDeviceNameSize],
        char guid[kAdmMaxGuidSize]) {
        RTC_LOG(INFO) << __FUNCTION__ << "(" << index << ", ...)";
        CHECKinitialized_();
        if (name == NULL) {
            return -1;
        }
        if (audio_device_->RecordingDeviceName(index, name, guid) == -1) {
            return -1;
        }
        if (name != NULL) {
            RTC_LOG(INFO) << "output: name = " << name;
        }
        if (guid != NULL) {
            RTC_LOG(INFO) << "output: guid = " << guid;
        }
        return 0;
    }

    int16_t CoreAudioModule::RecordingDevices() {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        uint16_t nRecordingDevices = audio_device_->RecordingDevices();
        RTC_LOG(INFO) << "output: " << nRecordingDevices;
        return (int16_t)nRecordingDevices;
    }

    int32_t CoreAudioModule::SetRecordingDevice(uint16_t index) {
        RTC_LOG(INFO) << __FUNCTION__ << "(" << index << ")";
        CHECKinitialized_();
        return audio_device_->SetRecordingDevice(index);
    }

    int32_t CoreAudioModule::SetRecordingDevice(WindowsDeviceType device) {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        return audio_device_->SetRecordingDevice(device);
    }

    int32_t CoreAudioModule::InitPlayout() {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        if (PlayoutIsInitialized()) {
            return 0;
        }
        int32_t result = audio_device_->InitPlayout();
        RTC_LOG(INFO) << "output: " << result;
        RTC_HISTOGRAM_BOOLEAN("WebRTC.Audio.InitPlayoutSuccess",
            static_cast<int>(result == 0));
        return result;
    }

    int32_t CoreAudioModule::InitRecording() {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        if (RecordingIsInitialized()) {
            return 0;
        }
        int32_t result = audio_device_->InitRecording();
        RTC_LOG(INFO) << "output: " << result;
        RTC_HISTOGRAM_BOOLEAN("WebRTC.Audio.InitRecordingSuccess",
            static_cast<int>(result == 0));
        return result;
    }

    bool CoreAudioModule::PlayoutIsInitialized() const {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized__BOOL();
        return audio_device_->PlayoutIsInitialized();
    }

    bool CoreAudioModule::RecordingIsInitialized() const {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized__BOOL();
        return audio_device_->RecordingIsInitialized();
    }

    int32_t CoreAudioModule::StartPlayout() {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        if (Playing()) {
            return 0;
        }
        audio_device_buffer_.StartPlayout();
        int32_t result = audio_device_->StartPlayout();
        RTC_LOG(INFO) << "output: " << result;
        RTC_HISTOGRAM_BOOLEAN("WebRTC.Audio.StartPlayoutSuccess",
            static_cast<int>(result == 0));
        return result;
    }

    int32_t CoreAudioModule::StopPlayout() {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        int32_t result = audio_device_->StopPlayout();
        audio_device_buffer_.StopPlayout();
        RTC_LOG(INFO) << "output: " << result;
        RTC_HISTOGRAM_BOOLEAN("WebRTC.Audio.StopPlayoutSuccess",
            static_cast<int>(result == 0));
        return result;
    }

    bool CoreAudioModule::Playing() const {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized__BOOL();
        return audio_device_->Playing();
    }

    int32_t CoreAudioModule::StartRecording() {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        if (Recording()) {
            return 0;
        }
        audio_device_buffer_.StartRecording();
        int32_t result = audio_device_->StartRecording();
        RTC_LOG(INFO) << "output: " << result;
        RTC_HISTOGRAM_BOOLEAN("WebRTC.Audio.StartRecordingSuccess",
            static_cast<int>(result == 0));
        return result;
    }

    int32_t CoreAudioModule::StopRecording() {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized_();
        int32_t result = audio_device_->StopRecording();
        audio_device_buffer_.StopRecording();
        RTC_LOG(INFO) << "output: " << result;
        RTC_HISTOGRAM_BOOLEAN("WebRTC.Audio.StopRecordingSuccess",
            static_cast<int>(result == 0));
        return result;
    }

    bool CoreAudioModule::Recording() const {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized__BOOL();
        return audio_device_->Recording();
    }

    int32_t CoreAudioModule::RegisterAudioCallback(
        AudioTransport* audioCallback) {
        RTC_LOG(INFO) << __FUNCTION__;
        return audio_device_buffer_.RegisterAudioCallback(audioCallback);
    }

    int32_t CoreAudioModule::PlayoutDelay(uint16_t* delayMS) const {
        CHECKinitialized_();
        uint16_t delay = 0;
        if (audio_device_->PlayoutDelay(delay) == -1) {
            RTC_LOG(LERROR) << "failed to retrieve the playout delay";
            return -1;
        }
        *delayMS = delay;
        return 0;
    }

    bool CoreAudioModule::BuiltInAECIsAvailable() const {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized__BOOL();
        bool isAvailable = audio_device_->BuiltInAECIsAvailable();
        RTC_LOG(INFO) << "output: " << isAvailable;
        return isAvailable;
    }

    int32_t CoreAudioModule::EnableBuiltInAEC(bool enable) {
        RTC_LOG(INFO) << __FUNCTION__ << "(" << enable << ")";
        CHECKinitialized_();
        int32_t ok = audio_device_->EnableBuiltInAEC(enable);
        RTC_LOG(INFO) << "output: " << ok;
        return ok;
    }

    bool CoreAudioModule::BuiltInAGCIsAvailable() const {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized__BOOL();
        bool isAvailable = audio_device_->BuiltInAGCIsAvailable();
        RTC_LOG(INFO) << "output: " << isAvailable;
        return isAvailable;
    }

    int32_t CoreAudioModule::EnableBuiltInAGC(bool enable) {
        RTC_LOG(INFO) << __FUNCTION__ << "(" << enable << ")";
        CHECKinitialized_();
        int32_t ok = audio_device_->EnableBuiltInAGC(enable);
        RTC_LOG(INFO) << "output: " << ok;
        return ok;
    }

    bool CoreAudioModule::BuiltInNSIsAvailable() const {
        RTC_LOG(INFO) << __FUNCTION__;
        CHECKinitialized__BOOL();
        bool isAvailable = audio_device_->BuiltInNSIsAvailable();
        RTC_LOG(INFO) << "output: " << isAvailable;
        return isAvailable;
    }

    int32_t CoreAudioModule::EnableBuiltInNS(bool enable) {
        RTC_LOG(INFO) << __FUNCTION__ << "(" << enable << ")";
        CHECKinitialized_();
        int32_t ok = audio_device_->EnableBuiltInNS(enable);
        RTC_LOG(INFO) << "output: " << ok;
        return ok;
    }
}  // namespace webrtc
