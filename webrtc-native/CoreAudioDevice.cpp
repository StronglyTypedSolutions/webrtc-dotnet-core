#include "pch.h"
#include "CoreAudioDevice.h"


#pragma warning(disable : 4995)  // name was marked as #pragma deprecated

/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/audio_device/audio_device_config.h"

#include "modules/audio_device/win/audio_device_core_win.h"

#include <assert.h>
#include <string.h>

#include <comdef.h>
#include <dmo.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmsystem.h>
#include <strsafe.h>
#include <uuids.h>
#include <windows.h>

#include <iomanip>

#include "rtc_base/logging.h"
#include "rtc_base/platform_thread.h"
#include "rtc_base/string_utils.h"
#include "rtc_base/thread_annotations.h"
#include "system_wrappers/include/sleep.h"

// Macro that calls a COM method returning HRESULT value.
#define EXIT_ON_ERROR(hres) \
  do {                      \
    if (FAILED(hres))       \
      goto Exit;            \
  } while (0)

// Macro that continues to a COM error.
#define CONTINUE_ON_ERROR(hres) \
  do {                          \
    if (FAILED(hres))           \
      goto Next;                \
  } while (0)

// Macro that releases a COM object if not NULL.
#define SAFE_RELEASE(p) \
  do {                  \
    if ((p)) {          \
      (p)->Release();   \
      (p) = NULL;       \
    }                   \
  } while (0)

#define ROUND(x) ((x) >= 0 ? (int)((x) + 0.5) : (int)((x)-0.5))

// REFERENCE_TIME time units per millisecond
#define REFTIMES_PER_MILLISEC 10000

typedef struct tagTHREADNAME_INFO {
    DWORD dwType;      // must be 0x1000
    LPCSTR szName;     // pointer to name (in user addr space)
    DWORD dwThreadID;  // thread ID (-1=caller thread)
    DWORD dwFlags;     // reserved for future use, must be zero
} THREADNAME_INFO;

namespace webrtc {
    namespace {

        enum { COM_THREADING_MODEL = COINIT_MULTITHREADED };

        enum { kAecCaptureStreamIndex = 0, kAecRenderStreamIndex = 1 };

        // An implementation of IMediaBuffer, as required for
        // IMediaObject::ProcessOutput(). After consuming data provided by
        // ProcessOutput(), call SetLength() to update the buffer availability.
        //
        // Example implementation:
        // http://msdn.microsoft.com/en-us/library/dd376684(v=vs.85).aspx
        class MediaBufferImpl : public IMediaBuffer {
        public:
            explicit MediaBufferImpl(DWORD maxLength)
                : _data(new BYTE[maxLength]),
                _length(0),
                _maxLength(maxLength),
                _refCount(0) {}

            // IMediaBuffer methods.
            STDMETHOD(GetBufferAndLength(BYTE** ppBuffer, DWORD* pcbLength)) {
                if (!ppBuffer || !pcbLength) {
                    return E_POINTER;
                }

                *ppBuffer = _data;
                *pcbLength = _length;

                return S_OK;
            }

            STDMETHOD(GetMaxLength(DWORD* pcbMaxLength)) {
                if (!pcbMaxLength) {
                    return E_POINTER;
                }

                *pcbMaxLength = _maxLength;
                return S_OK;
            }

            STDMETHOD(SetLength(DWORD cbLength)) {
                if (cbLength > _maxLength) {
                    return E_INVALIDARG;
                }

                _length = cbLength;
                return S_OK;
            }

            // IUnknown methods.
            STDMETHOD_(ULONG, AddRef()) { return InterlockedIncrement(&_refCount); }

            STDMETHOD(QueryInterface(REFIID riid, void** ppv)) {
                if (!ppv) {
                    return E_POINTER;
                }
                else if (riid != IID_IMediaBuffer && riid != IID_IUnknown) {
                    return E_NOINTERFACE;
                }

                *ppv = static_cast<IMediaBuffer*>(this);
                AddRef();
                return S_OK;
            }

            STDMETHOD_(ULONG, Release()) {
                LONG refCount = InterlockedDecrement(&_refCount);
                if (refCount == 0) {
                    delete this;
                }

                return refCount;
            }

        private:
            ~MediaBufferImpl() { delete[] _data; }

            BYTE* _data;
            DWORD _length;
            const DWORD _maxLength;
            LONG _refCount;
        };
    }  // namespace

    // ============================================================================
    //                              Static Methods
    // ============================================================================

    // ----------------------------------------------------------------------------
    //  CoreAudioIsSupported
    // ----------------------------------------------------------------------------

    bool CoreAudioDevice::CoreAudioIsSupported() {
        RTC_LOG(LS_VERBOSE) << __FUNCTION__;

        bool MMDeviceIsAvailable(false);
        bool coreAudioIsSupported(false);

        HRESULT hr(S_OK);
        TCHAR buf[MAXERRORLENGTH];
        TCHAR errorText[MAXERRORLENGTH];

        // 1) Check if Windows version is Vista SP1 or later.
        //
        // CoreAudio is only available on Vista SP1 and later.
        //
        OSVERSIONINFOEX osvi;
        DWORDLONG dwlConditionMask = 0;
        int op = VER_LESS_EQUAL;

        // Initialize the OSVERSIONINFOEX structure.
        ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
        osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
        osvi.dwMajorVersion = 6;
        osvi.dwMinorVersion = 0;
        osvi.wServicePackMajor = 0;
        osvi.wServicePackMinor = 0;
        osvi.wProductType = VER_NT_WORKSTATION;

        // Initialize the condition mask.
        VER_SET_CONDITION(dwlConditionMask, VER_MAJORVERSION, op);
        VER_SET_CONDITION(dwlConditionMask, VER_MINORVERSION, op);
        VER_SET_CONDITION(dwlConditionMask, VER_SERVICEPACKMAJOR, op);
        VER_SET_CONDITION(dwlConditionMask, VER_SERVICEPACKMINOR, op);
        VER_SET_CONDITION(dwlConditionMask, VER_PRODUCT_TYPE, VER_EQUAL);

        DWORD dwTypeMask = VER_MAJORVERSION | VER_MINORVERSION |
            VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR |
            VER_PRODUCT_TYPE;

        // Perform the test.
        BOOL isVistaRTMorXP = VerifyVersionInfo(&osvi, dwTypeMask, dwlConditionMask);
        if (isVistaRTMorXP != 0) {
            RTC_LOG(LS_VERBOSE)
                << "*** Windows Core Audio is only supported on Vista SP1 or later"
                << " => will revert to the Wave API ***";
            return false;
        }

        // 2) Initializes the COM library for use by the calling thread.

        // The COM init wrapper sets the thread's concurrency model to MTA,
        // and creates a new apartment for the thread if one is required. The
        // wrapper also ensures that each call to CoInitializeEx is balanced
        // by a corresponding call to CoUninitialize.
        //
        ScopedCOMInitializer comInit(ScopedCOMInitializer::kMTA);
        if (!comInit.succeeded()) {
            // Things will work even if an STA thread is calling this method but we
            // want to ensure that MTA is used and therefore return false here.
            return false;
        }

        // 3) Check if the MMDevice API is available.
        //
        // The Windows Multimedia Device (MMDevice) API enables audio clients to
        // discover audio endpoint devices, determine their capabilities, and create
        // driver instances for those devices.
        // Header file Mmdeviceapi.h defines the interfaces in the MMDevice API.
        // The MMDevice API consists of several interfaces. The first of these is the
        // IMMDeviceEnumerator interface. To access the interfaces in the MMDevice
        // API, a client obtains a reference to the IMMDeviceEnumerator interface of a
        // device-enumerator object by calling the CoCreateInstance function.
        //
        // Through the IMMDeviceEnumerator interface, the client can obtain references
        // to the other interfaces in the MMDevice API. The MMDevice API implements
        // the following interfaces:
        //
        // IMMDevice            Represents an audio device.
        // IMMDeviceCollection  Represents a collection of audio devices.
        // IMMDeviceEnumerator  Provides methods for enumerating audio devices.
        // IMMEndpoint          Represents an audio endpoint device.
        //
        IMMDeviceEnumerator* pIMMD(nullptr);
        const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
        const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);

        hr = CoCreateInstance(
            CLSID_MMDeviceEnumerator,  // GUID value of MMDeviceEnumerator coclass
            nullptr, CLSCTX_ALL,
            IID_IMMDeviceEnumerator,  // GUID value of the IMMDeviceEnumerator
                                      // interface
            (void**)&pIMMD);

        if (FAILED(hr)) {
            RTC_LOG(LS_ERROR) << "CoreAudioDevice::CoreAudioIsSupported()"
                << " Failed to create the required COM object (hr=" << hr
                << ")";
            RTC_LOG(LS_VERBOSE) << "CoreAudioDevice::CoreAudioIsSupported()"
                << " CoCreateInstance(MMDeviceEnumerator) failed (hr="
                << hr << ")";

            const DWORD dwFlags =
                FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
            const DWORD dwLangID = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);

            // Gets the system's human readable message string for this HRESULT.
            // All error message in English by default.
            DWORD messageLength = ::FormatMessageW(dwFlags, nullptr, hr, dwLangID, errorText,
                MAXERRORLENGTH, nullptr);

            assert(messageLength <= MAXERRORLENGTH);

            // Trims tailing white space (FormatMessage() leaves a trailing cr-lf.).
            for (; messageLength && ::isspace(errorText[messageLength - 1]);
                --messageLength) {
                errorText[messageLength - 1] = '\0';
            }

            StringCchPrintf(buf, MAXERRORLENGTH, TEXT("Error details: "));
            StringCchCat(buf, MAXERRORLENGTH, errorText);
            RTC_LOG(LS_VERBOSE) << buf;
        }
        else {
            MMDeviceIsAvailable = true;
            RTC_LOG(LS_VERBOSE)
                << "CoreAudioDevice::CoreAudioIsSupported()"
                << " CoCreateInstance(MMDeviceEnumerator) succeeded (hr=" << hr << ")";
            SAFE_RELEASE(pIMMD);
        }

        // 4) Verify that we can create and initialize our Core Audio class.
        //
        // Also, perform a limited "API test" to ensure that Core Audio is supported
        // for all devices.
        //
        if (MMDeviceIsAvailable) {
            coreAudioIsSupported = false;

            CoreAudioDevice* p = new CoreAudioDevice();
            if (p == nullptr) {
                return false;
            }

            int ok(0);
            int temp_ok(0);
            bool available(false);

            if (p->Init() != InitStatus::OK) {
                ok |= -1;
            }

            int16_t numDevsRec = p->RecordingDevices();
            for (uint16_t i = 0; i < numDevsRec; i++) {
                ok |= p->SetRecordingDevice(i);
                temp_ok = p->RecordingIsAvailable(available);
                ok |= temp_ok;
                ok |= (available == false);
                if (available) {
                    ok |= p->InitMicrophone();
                }
                if (ok) {
                    RTC_LOG(LS_WARNING)
                        << "CoreAudioDevice::CoreAudioIsSupported()"
                        << " Failed to use Core Audio Recording for device id=" << i;
                }
            }

            ok |= p->Terminate();

            if (ok == 0) {
                coreAudioIsSupported = true;
            }

            delete p;
        }

        if (coreAudioIsSupported) {
            RTC_LOG(LS_VERBOSE) << "*** Windows Core Audio is supported ***";
        }
        else {
            RTC_LOG(LS_VERBOSE) << "*** Windows Core Audio is NOT supported"
                << " => will revert to the Wave API ***";
        }

        return (coreAudioIsSupported);
    }

    // ============================================================================
    //                            Construction & Destruction
    // ============================================================================

    // ----------------------------------------------------------------------------
    //  CoreAudioDevice() - ctor
    // ----------------------------------------------------------------------------

    CoreAudioDevice::CoreAudioDevice()
        : _avrtLibrary(nullptr),
        _winSupportAvrt(false),
        _comInit(ScopedCOMInitializer::kMTA),
        _ptrAudioBuffer(nullptr),
        _ptrEnumerator(nullptr),
        _ptrLoopbackCollection(nullptr),
        _ptrDeviceIn(nullptr),
        _ptrClientIn(nullptr),
        _ptrLoopbackClient(nullptr),
        _ptrCaptureVolume(nullptr),
        _ptrRenderSimpleVolume(nullptr),
        _hCaptureSamplesReadyEvent(nullptr),
        _hRecThread(nullptr),
        _hCaptureStartedEvent(nullptr),
        _hShutdownCaptureEvent(nullptr),
        _hMmTask(nullptr),
        // _playAudioFrameSize(0),
        // _playSampleRate(0),
        // _playBlockSize(0),
        // _playChannels(2),
        // _sndCardPlayDelay(0),
        _sndCardRecDelay(0),
        // _writtenSamples(0),
        _readSamples(0),
        _recAudioFrameSize(0),
        _recSampleRate(0),
        _recBlockSize(0),
        _recChannels(2),
        _initialized(false),
        _recording(false),
        _recIsInitialized(false),
        _playIsInitialized(false),
        _speakerIsInitialized(false),
        _microphoneIsInitialized(false),
        _usingInputDeviceIndex(false),
        _inputDevice(AudioDeviceModule::kDefaultCommunicationDevice),
        _inputDeviceIndex(0) {
        RTC_LOG(LS_INFO) << __FUNCTION__ << " created";
        assert(_comInit.succeeded());

        // Try to load the Avrt DLL
        if (!_avrtLibrary) {
            // Get handle to the Avrt DLL module.
            _avrtLibrary = LoadLibrary(TEXT("Avrt.dll"));
            if (_avrtLibrary) {
                // Handle is valid (should only happen if OS larger than vista & win7).
                // Try to get the function addresses.
                RTC_LOG(LS_VERBOSE) << "CoreAudioDevice::CoreAudioDevice()"
                    << " The Avrt DLL module is now loaded";

                _PAvRevertMmThreadCharacteristics =
                    (PAvRevertMmThreadCharacteristics)GetProcAddress(
                        _avrtLibrary, "AvRevertMmThreadCharacteristics");
                _PAvSetMmThreadCharacteristicsA =
                    (PAvSetMmThreadCharacteristicsA)GetProcAddress(
                        _avrtLibrary, "AvSetMmThreadCharacteristicsA");
                _PAvSetMmThreadPriority = (PAvSetMmThreadPriority)GetProcAddress(
                    _avrtLibrary, "AvSetMmThreadPriority");

                if (_PAvRevertMmThreadCharacteristics &&
                    _PAvSetMmThreadCharacteristicsA && _PAvSetMmThreadPriority) {
                    RTC_LOG(LS_VERBOSE)
                        << "CoreAudioDevice::CoreAudioDevice()"
                        << " AvRevertMmThreadCharacteristics() is OK";
                    RTC_LOG(LS_VERBOSE)
                        << "CoreAudioDevice::CoreAudioDevice()"
                        << " AvSetMmThreadCharacteristicsA() is OK";
                    RTC_LOG(LS_VERBOSE)
                        << "CoreAudioDevice::CoreAudioDevice()"
                        << " AvSetMmThreadPriority() is OK";
                    _winSupportAvrt = true;
                }
            }
        }

        // Create our samples ready events - we want auto reset events that start in
        // the not-signaled state. The state of an auto-reset event object remains
        // signaled until a single waiting thread is released, at which time the
        // system automatically sets the state to nonsignaled. If no threads are
        // waiting, the event object's state remains signaled. (Except for
        // _hShutdownCaptureEvent, which is used to shutdown multiple threads).
        _hCaptureSamplesReadyEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        _hShutdownCaptureEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        _hCaptureStartedEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        _perfCounterFreq.QuadPart = 1;
        _perfCounterFactor = 0.0;

        // list of number of channels to use on recording side
        _recChannelsPrioList[0] = 2;  // stereo is prio 1
        _recChannelsPrioList[1] = 1;  // mono is prio 2
        _recChannelsPrioList[2] = 4;  // quad is prio 3

        // We know that this API will work since it has already been verified in
        // CoreAudioIsSupported, hence no need to check for errors here as well.

        // Retrive the IMMDeviceEnumerator API (should load the MMDevAPI.dll)
        // TODO(henrika): we should probably move this allocation to Init() instead
        // and deallocate in Terminate() to make the implementation more symmetric.
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&_ptrEnumerator));
        assert(NULL != _ptrEnumerator);
    }

    // ----------------------------------------------------------------------------
    //  CoreAudioDevice() - dtor
    // ----------------------------------------------------------------------------

    CoreAudioDevice::~CoreAudioDevice() {
        RTC_LOG(LS_INFO) << __FUNCTION__ << " destroyed";

        Terminate();

        // The IMMDeviceEnumerator is created during construction. Must release
        // it here and not in Terminate() since we don't recreate it in Init().
        SAFE_RELEASE(_ptrEnumerator);

        _ptrAudioBuffer = nullptr;

        if (nullptr != _hCaptureSamplesReadyEvent) {
            CloseHandle(_hCaptureSamplesReadyEvent);
            _hCaptureSamplesReadyEvent = nullptr;
        }

        if (nullptr != _hCaptureStartedEvent) {
            CloseHandle(_hCaptureStartedEvent);
            _hCaptureStartedEvent = nullptr;
        }

        if (nullptr != _hShutdownCaptureEvent) {
            CloseHandle(_hShutdownCaptureEvent);
            _hShutdownCaptureEvent = nullptr;
        }

        if (_avrtLibrary) {
            BOOL freeOK = FreeLibrary(_avrtLibrary);
            if (!freeOK) {
                RTC_LOG(LS_WARNING)
                    << "CoreAudioDevice::~CoreAudioDevice()"
                    << " failed to free the loaded Avrt DLL module correctly";
            }
            else {
                RTC_LOG(LS_WARNING) << "CoreAudioDevice::~CoreAudioDevice()"
                    << " the Avrt DLL module is now unloaded";
            }
        }
    }

    // ============================================================================
    //                                     API
    // ============================================================================

    // ----------------------------------------------------------------------------
    //  AttachAudioBuffer
    // ----------------------------------------------------------------------------

    void CoreAudioDevice::AttachAudioBuffer(AudioDeviceBuffer* audioBuffer) {
        _ptrAudioBuffer = audioBuffer;

        // Inform the AudioBuffer about default settings for this implementation.
        // Set all values to zero here since the actual settings will be done by
        // InitPlayout and InitRecording later.
        _ptrAudioBuffer->SetRecordingSampleRate(0);
        _ptrAudioBuffer->SetPlayoutSampleRate(0);
        _ptrAudioBuffer->SetRecordingChannels(0);
        _ptrAudioBuffer->SetPlayoutChannels(0);
    }

    // ----------------------------------------------------------------------------
    //  ActiveAudioLayer
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::ActiveAudioLayer(
        AudioDeviceModule::AudioLayer& audioLayer) const {
        audioLayer = AudioDeviceModule::kWindowsCoreAudio;
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  Init
    // ----------------------------------------------------------------------------

    AudioDeviceGeneric::InitStatus CoreAudioDevice::Init() {
        rtc::CritScope lock(&_critSect);

        if (_initialized) {
            return InitStatus::OK;
        }

        // Enumerate all audio loopback endpoint devices.
        // Note that, some of these will not be able to select by the user.
        // The complete collection is for internal use only.
        _EnumerateEndpointDevicesAll();

        _initialized = true;

        return InitStatus::OK;
    }

    // ----------------------------------------------------------------------------
    //  Terminate
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::Terminate() {
        rtc::CritScope lock(&_critSect);

        if (!_initialized) {
            return 0;
        }

        _initialized = false;
        _speakerIsInitialized = false;
        _microphoneIsInitialized = false;
        _recording = false;

        SAFE_RELEASE(_ptrLoopbackCollection);
        SAFE_RELEASE(_ptrDeviceIn);
        SAFE_RELEASE(_ptrClientIn);
        SAFE_RELEASE(_ptrLoopbackClient);
        SAFE_RELEASE(_ptrCaptureVolume);
        SAFE_RELEASE(_ptrRenderSimpleVolume);

        return 0;
    }

    // ----------------------------------------------------------------------------
    //  Initialized
    // ----------------------------------------------------------------------------

    bool CoreAudioDevice::Initialized() const {
        return (_initialized);
    }

    // ----------------------------------------------------------------------------
    //  InitSpeaker
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::InitSpeaker() {
        // [PV] No speaker in the cloud, we're never receiving audio
        _speakerIsInitialized = true;
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  InitMicrophone
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::InitMicrophone() {
        // [PV] Microphone is actually loopback of default system speaker
        rtc::CritScope lock(&_critSect);

        if (_recording) {
            return -1;
        }

        if (_ptrDeviceIn == nullptr) {
            return -1;
        }

        if (_usingInputDeviceIndex) {
            int16_t nDevices = RecordingDevices();
            if (_inputDeviceIndex > (nDevices - 1)) {
                RTC_LOG(LS_ERROR) << "current device selection is invalid => unable to"
                    << " initialize";
                return -1;
            }
        }

        int32_t ret(0);

        SAFE_RELEASE(_ptrDeviceIn);
        if (_usingInputDeviceIndex) {
            // Refresh the selected capture endpoint device using current index
            ret = _GetListDevice(_inputDeviceIndex, &_ptrDeviceIn);
        }
        else {
            // [PV] Changed for loopback
            // ERole role = (_inputDevice == AudioDeviceModule::kDefaultDevice)
            //     ? eConsole
            //     : eCommunications;
            // Refresh the selected capture endpoint device using role
            // [PV] Changed to eRender for loopback
            ret = _GetDefaultDevice(eMultimedia, &_ptrDeviceIn);
        }

        if (ret != 0 || (_ptrDeviceIn == nullptr)) {
            RTC_LOG(LS_ERROR) << "failed to initialize the capturing enpoint device";
            SAFE_RELEASE(_ptrDeviceIn);
            return -1;
        }

        SAFE_RELEASE(_ptrCaptureVolume);
        ret = _ptrDeviceIn->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&_ptrCaptureVolume));
        if (ret != 0 || _ptrCaptureVolume == nullptr) {
            RTC_LOG(LS_ERROR) << "failed to initialize the capture volume";
            SAFE_RELEASE(_ptrCaptureVolume);
            return -1;
        }

        _microphoneIsInitialized = true;

        return 0;
    }

    // ----------------------------------------------------------------------------
    //  SpeakerIsInitialized
    // ----------------------------------------------------------------------------

    bool CoreAudioDevice::SpeakerIsInitialized() const {
        return (_speakerIsInitialized);
    }

    // ----------------------------------------------------------------------------
    //  MicrophoneIsInitialized
    // ----------------------------------------------------------------------------

    bool CoreAudioDevice::MicrophoneIsInitialized() const {
        return (_microphoneIsInitialized);
    }

    // ----------------------------------------------------------------------------
    //  SpeakerVolumeIsAvailable
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::SpeakerVolumeIsAvailable(bool& available) {
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  SetSpeakerVolume
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::SetSpeakerVolume(uint32_t volume) {
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  SpeakerVolume
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::SpeakerVolume(uint32_t& volume) const {
        volume = 0;
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  MaxSpeakerVolume
    //
    //  The internal range for Core Audio is 0.0 to 1.0, where 0.0 indicates
    //  silence and 1.0 indicates full volume (no attenuation).
    //  We add our (webrtc-internal) own max level to match the Wave API and
    //  how it is used today in VoE.
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::MaxSpeakerVolume(uint32_t& maxVolume) const {
        maxVolume = 0;
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  MinSpeakerVolume
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::MinSpeakerVolume(uint32_t& minVolume) const {
        minVolume = 0;
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  SpeakerMuteIsAvailable
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::SpeakerMuteIsAvailable(bool& available) {
        available = false;
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  SetSpeakerMute
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::SetSpeakerMute(bool enable) {
        if (!_speakerIsInitialized) {
            return -1;
        }
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  SpeakerMute
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::SpeakerMute(bool& enabled) const {
        if (!_speakerIsInitialized) {
            return -1;
        }
        enabled = false;
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  MicrophoneMuteIsAvailable
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::MicrophoneMuteIsAvailable(bool& available) {
        available = false;
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  SetMicrophoneMute
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::SetMicrophoneMute(bool enable) {
        if (!_microphoneIsInitialized) {
            return -1;
        }
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  MicrophoneMute
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::MicrophoneMute(bool& enabled) const {
        if (!_microphoneIsInitialized) {
            return -1;
        }
        enabled = false;
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  StereoRecordingIsAvailable
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::StereoRecordingIsAvailable(bool& available) {
        available = true;
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  SetStereoRecording
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::SetStereoRecording(bool enable) {
        rtc::CritScope lock(&_critSect);

        if (enable) {
            _recChannelsPrioList[0] = 2;  // try stereo first
            _recChannelsPrioList[1] = 1;
            _recChannels = 2;
        }
        else {
            _recChannelsPrioList[0] = 1;  // try mono first
            _recChannelsPrioList[1] = 2;
            _recChannels = 1;
        }

        return 0;
    }

    // ----------------------------------------------------------------------------
    //  StereoRecording
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::StereoRecording(bool& enabled) const {
        if (_recChannels == 2)
            enabled = true;
        else
            enabled = false;

        return 0;
    }

    // ----------------------------------------------------------------------------
    //  StereoPlayoutIsAvailable
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::StereoPlayoutIsAvailable(bool& available) {
        available = false;
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  SetStereoPlayout
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::SetStereoPlayout(bool enable) {
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  StereoPlayout
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::StereoPlayout(bool& enabled) const {
        enabled = false;
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  MicrophoneVolumeIsAvailable
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::MicrophoneVolumeIsAvailable(bool& available) {
        rtc::CritScope lock(&_critSect);

        if (_ptrDeviceIn == nullptr) {
            return -1;
        }

        HRESULT hr = S_OK;
        IAudioEndpointVolume* pVolume = nullptr;
        float volume(0.0f);

        hr = _ptrDeviceIn->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&pVolume));
        EXIT_ON_ERROR(hr);

        hr = pVolume->GetMasterVolumeLevelScalar(&volume);
        if (FAILED(hr)) {
            available = false;
        }
        available = true;

        SAFE_RELEASE(pVolume);
        return 0;

    Exit:
        _TraceCOMError(hr);
        SAFE_RELEASE(pVolume);
        return -1;
    }

    // ----------------------------------------------------------------------------
    //  SetMicrophoneVolume
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::SetMicrophoneVolume(uint32_t volume) {
        RTC_LOG(LS_VERBOSE) << "CoreAudioDevice::SetMicrophoneVolume(volume="
            << volume << ")";

        {
            rtc::CritScope lock(&_critSect);

            if (!_microphoneIsInitialized) {
                return -1;
            }

            if (_ptrDeviceIn == nullptr) {
                return -1;
            }
        }

        if (volume < static_cast<uint32_t>(MIN_CORE_MICROPHONE_VOLUME) ||
            volume > static_cast<uint32_t>(MAX_CORE_MICROPHONE_VOLUME)) {
            return -1;
        }

        HRESULT hr = S_OK;
        // scale input volume to valid range (0.0 to 1.0)
        const float fLevel = static_cast<float>(volume) / MAX_CORE_MICROPHONE_VOLUME;
        _volumeMutex.Enter();
        _ptrCaptureVolume->SetMasterVolumeLevelScalar(fLevel, nullptr);
        _volumeMutex.Leave();
        EXIT_ON_ERROR(hr);

        return 0;

    Exit:
        _TraceCOMError(hr);
        return -1;
    }

    // ----------------------------------------------------------------------------
    //  MicrophoneVolume
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::MicrophoneVolume(uint32_t& volume) const {
        {
            rtc::CritScope lock(&_critSect);

            if (!_microphoneIsInitialized) {
                return -1;
            }

            if (_ptrDeviceIn == nullptr) {
                return -1;
            }
        }

        HRESULT hr = S_OK;
        float fLevel(0.0f);
        volume = 0;
        _volumeMutex.Enter();
        hr = _ptrCaptureVolume->GetMasterVolumeLevelScalar(&fLevel);
        _volumeMutex.Leave();
        EXIT_ON_ERROR(hr);

        // scale input volume range [0.0,1.0] to valid output range
        volume = static_cast<uint32_t>(fLevel * MAX_CORE_MICROPHONE_VOLUME);

        return 0;

    Exit:
        _TraceCOMError(hr);
        return -1;
    }

    // ----------------------------------------------------------------------------
    //  MaxMicrophoneVolume
    //
    //  The internal range for Core Audio is 0.0 to 1.0, where 0.0 indicates
    //  silence and 1.0 indicates full volume (no attenuation).
    //  We add our (webrtc-internal) own max level to match the Wave API and
    //  how it is used today in VoE.
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::MaxMicrophoneVolume(uint32_t& maxVolume) const {
        RTC_LOG(LS_VERBOSE) << __FUNCTION__;

        if (!_microphoneIsInitialized) {
            return -1;
        }

        maxVolume = static_cast<uint32_t>(MAX_CORE_MICROPHONE_VOLUME);

        return 0;
    }

    // ----------------------------------------------------------------------------
    //  MinMicrophoneVolume
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::MinMicrophoneVolume(uint32_t& minVolume) const {
        if (!_microphoneIsInitialized) {
            return -1;
        }

        minVolume = static_cast<uint32_t>(MIN_CORE_MICROPHONE_VOLUME);

        return 0;
    }

    // ----------------------------------------------------------------------------
    //  PlayoutDevices
    // ----------------------------------------------------------------------------

    int16_t CoreAudioDevice::PlayoutDevices() {
        rtc::CritScope lock(&_critSect);

        if (_RefreshDeviceList() != -1) {
            return (_DeviceListCount());
        }

        return -1;
    }

    // ----------------------------------------------------------------------------
    //  SetPlayoutDevice I (II)
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::SetPlayoutDevice(uint16_t index) {
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  SetPlayoutDevice II (II)
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::SetPlayoutDevice(AudioDeviceModule::WindowsDeviceType device) {
        // [PV] We don't send received audio to the speakers of the virtual machine
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  PlayoutDeviceName
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::PlayoutDeviceName(
        uint16_t index,
        char name[kAdmMaxDeviceNameSize],
        char guid[kAdmMaxGuidSize]) {
        name[0] = 0;
        guid[0] = 0;
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  RecordingDeviceName
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::RecordingDeviceName(
        uint16_t index,
        char name[kAdmMaxDeviceNameSize],
        char guid[kAdmMaxGuidSize]) {
        bool defaultCommunicationDevice(false);
        const int16_t nDevices(
            RecordingDevices());  // also updates the list of devices

        // Special fix for the case when the user selects '-1' as index (<=> Default
        // Communication Device)
        if (index == (uint16_t)(-1)) {
            defaultCommunicationDevice = true;
            index = 0;
            RTC_LOG(LS_VERBOSE) << "Default Communication endpoint device will be used";
        }

        if ((index > (nDevices - 1)) || (name == nullptr)) {
            return -1;
        }

        memset(name, 0, kAdmMaxDeviceNameSize);

        if (guid != nullptr) {
            memset(guid, 0, kAdmMaxGuidSize);
        }

        rtc::CritScope lock(&_critSect);

        int32_t ret(-1);
        WCHAR szDeviceName[MAX_PATH];
        const int bufferLen = sizeof(szDeviceName) / sizeof(szDeviceName)[0];

        // Get the endpoint device's friendly-name
        if (defaultCommunicationDevice) {
            ret = _GetDefaultDeviceName(eCommunications, szDeviceName,bufferLen);
        }
        else {
            ret = _GetListDeviceName(index, szDeviceName, bufferLen);
        }

        if (ret == 0) {
            // Convert the endpoint device's friendly-name to UTF-8
            if (WideCharToMultiByte(CP_UTF8, 0, szDeviceName, -1, name,
                kAdmMaxDeviceNameSize, nullptr, nullptr) == 0) {
                RTC_LOG(LS_ERROR)
                    << "WideCharToMultiByte(CP_UTF8) failed with error code "
                    << GetLastError();
            }
        }

        // Get the endpoint ID string (uniquely identifies the device among all audio
        // endpoint devices)
        if (defaultCommunicationDevice) {
            ret =
                _GetDefaultDeviceID(eCommunications, szDeviceName, bufferLen);
        }
        else {
            ret = _GetListDeviceID(index, szDeviceName, bufferLen);
        }

        if (guid != nullptr && ret == 0) {
            // Convert the endpoint device's ID string to UTF-8
            if (WideCharToMultiByte(CP_UTF8, 0, szDeviceName, -1, guid, kAdmMaxGuidSize,
                                    nullptr, nullptr) == 0) {
                RTC_LOG(LS_ERROR)
                    << "WideCharToMultiByte(CP_UTF8) failed with error code "
                    << GetLastError();
            }
        }

        return ret;
    }

    // ----------------------------------------------------------------------------
    //  RecordingDevices
    // ----------------------------------------------------------------------------

    int16_t CoreAudioDevice::RecordingDevices() {
        rtc::CritScope lock(&_critSect);

        // [PV] Changed to eRender for loopback
        if (_RefreshDeviceList() != -1) {
            return (_DeviceListCount());
        }

        return -1;
    }

    // ----------------------------------------------------------------------------
    //  SetRecordingDevice I (II)
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::SetRecordingDevice(uint16_t index) {
        if (_recIsInitialized) {
            return -1;
        }

        // Get current number of available capture endpoint devices and refresh the
        // capture collection.
        UINT nDevices = RecordingDevices();

        if (index < 0 || index >(nDevices - 1)) {
            RTC_LOG(LS_ERROR) << "device index is out of range [0," << (nDevices - 1)
                << "]";
            return -1;
        }

        rtc::CritScope lock(&_critSect);

        HRESULT hr(S_OK);

        assert(_ptrLoopbackCollection != NULL);

        // Select an endpoint capture device given the specified index
        SAFE_RELEASE(_ptrDeviceIn);
        hr = _ptrLoopbackCollection->Item(index, &_ptrDeviceIn);
        if (FAILED(hr)) {
            _TraceCOMError(hr);
            SAFE_RELEASE(_ptrDeviceIn);
            return -1;
        }

        WCHAR szDeviceName[MAX_PATH];
        const int bufferLen = sizeof(szDeviceName) / sizeof(szDeviceName)[0];

        // Get the endpoint device's friendly-name
        if (_GetDeviceName(_ptrDeviceIn, szDeviceName, bufferLen) == 0) {
            RTC_LOG(LS_VERBOSE) << "friendly name: \"" << szDeviceName << "\"";
        }

        _usingInputDeviceIndex = true;
        _inputDeviceIndex = index;

        return 0;
    }

    // ----------------------------------------------------------------------------
    //  SetRecordingDevice II (II)
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::SetRecordingDevice(
        AudioDeviceModule::WindowsDeviceType device) {
        if (_recIsInitialized) {
            return -1;
        }

        ERole role(eMultimedia);

        // if (device == AudioDeviceModule::kDefaultDevice) {
        //     role = eConsole;
        // }
        // else if (device == AudioDeviceModule::kDefaultCommunicationDevice) {
        //     role = eCommunications;
        // }

        rtc::CritScope lock(&_critSect);

        // Refresh the list of capture endpoint devices
        // NOTE: [PV] Changed from eCapture to eRender for loopback
        _RefreshDeviceList();

        HRESULT hr(S_OK);

        assert(_ptrEnumerator != NULL);

        //  Select an endpoint capture device given the specified role
        SAFE_RELEASE(_ptrDeviceIn);
        // NOTE: [PV] Changed from eCapture to eRender for loopback
        hr = _ptrEnumerator->GetDefaultAudioEndpoint(eRender, role, &_ptrDeviceIn);
        if (FAILED(hr)) {
            _TraceCOMError(hr);
            SAFE_RELEASE(_ptrDeviceIn);
            return -1;
        }

        WCHAR szDeviceName[MAX_PATH];
        const int bufferLen = sizeof(szDeviceName) / sizeof(szDeviceName)[0];

        // Get the endpoint device's friendly-name
        if (_GetDeviceName(_ptrDeviceIn, szDeviceName, bufferLen) == 0) {
            RTC_LOG(LS_VERBOSE) << "friendly name: \"" << szDeviceName << "\"";
        }

        _usingInputDeviceIndex = false;
        _inputDevice = device;

        return 0;
    }

    // ----------------------------------------------------------------------------
    //  PlayoutIsAvailable
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::PlayoutIsAvailable(bool& available) {
        available = false;

        // Try to initialize the playout side
        int32_t res = InitPlayout();

        // Cancel effect of initialization
        StopPlayout();

        if (res != -1) {
            available = true;
        }

        return 0;
    }

    // ----------------------------------------------------------------------------
    //  RecordingIsAvailable
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::RecordingIsAvailable(bool& available) {
        available = false;

        // Try to initialize the recording side
        int32_t res = InitRecording();

        // Cancel effect of initialization
        StopRecording();

        if (res != -1) {
            available = true;
        }

        return 0;
    }

    // ----------------------------------------------------------------------------
    //  InitPlayout
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::InitPlayout() {
        _playIsInitialized = true;
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  InitRecording
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::InitRecording() {
        rtc::CritScope lock(&_critSect);

        if (_recording) {
            return -1;
        }

        if (_recIsInitialized) {
            return 0;
        }

        if (QueryPerformanceFrequency(&_perfCounterFreq) == 0) {
            return -1;
        }
        _perfCounterFactor = 10000000.0 / (double)_perfCounterFreq.QuadPart;

        if (_ptrDeviceIn == nullptr) {
            return -1;
        }

        // Initialize the microphone (devices might have been added or removed)
        if (InitMicrophone() == -1) {
            RTC_LOG(LS_WARNING) << "InitMicrophone() failed";
        }

        // Ensure that the updated capturing endpoint device is valid
        if (_ptrDeviceIn == nullptr) {
            return -1;
        }

        HRESULT hr = S_OK;
        WAVEFORMATEX* pWfxIn = nullptr;
        WAVEFORMATEXTENSIBLE Wfx = WAVEFORMATEXTENSIBLE();
        WAVEFORMATEX* pWfxClosestMatch = nullptr;
        UINT bufferFrameCount(0);
        const int freqs[6] = { 48000, 44100, 16000, 96000, 32000, 8000 };

        // Create COM object with IAudioClient interface.
        SAFE_RELEASE(_ptrClientIn);
        hr = _ptrDeviceIn->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            (void**)&_ptrClientIn);
        EXIT_ON_ERROR(hr);

        // Retrieve the stream format that the audio engine uses for its internal
        // processing (mixing) of shared-mode streams.
        hr = _ptrClientIn->GetMixFormat(&pWfxIn);
        if (SUCCEEDED(hr)) {
            RTC_LOG(LS_VERBOSE) << "Audio Engine's current capturing mix format:";
            // format type
            RTC_LOG(LS_VERBOSE) << "wFormatTag     : 0x"
                << rtc::ToHex(pWfxIn->wFormatTag) << " ("
                << pWfxIn->wFormatTag << ")";
            // number of channels (i.e. mono, stereo...)
            RTC_LOG(LS_VERBOSE) << "nChannels      : " << pWfxIn->nChannels;
            // sample rate
            RTC_LOG(LS_VERBOSE) << "nSamplesPerSec : " << pWfxIn->nSamplesPerSec;
            // for buffer estimation
            RTC_LOG(LS_VERBOSE) << "nAvgBytesPerSec: " << pWfxIn->nAvgBytesPerSec;
            // block size of data
            RTC_LOG(LS_VERBOSE) << "nBlockAlign    : " << pWfxIn->nBlockAlign;
            // number of bits per sample of mono data
            RTC_LOG(LS_VERBOSE) << "wBitsPerSample : " << pWfxIn->wBitsPerSample;
            RTC_LOG(LS_VERBOSE) << "cbSize         : " << pWfxIn->cbSize;
        }

        // Set wave format
        Wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        Wfx.Format.wBitsPerSample = 16;
        Wfx.Format.cbSize = 22;
        Wfx.dwChannelMask = 0;
        Wfx.Samples.wValidBitsPerSample = Wfx.Format.wBitsPerSample;
        Wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

        hr = S_FALSE;

        // Iterate over frequencies and channels, in order of priority
        for (unsigned int freq = 0; freq < sizeof(freqs) / sizeof(freqs[0]); freq++) {
            for (unsigned int chan = 0;
                chan < sizeof(_recChannelsPrioList) / sizeof(_recChannelsPrioList[0]);
                chan++) {
                Wfx.Format.nChannels = _recChannelsPrioList[chan];
                Wfx.Format.nSamplesPerSec = freqs[freq];
                Wfx.Format.nBlockAlign =
                    Wfx.Format.nChannels * Wfx.Format.wBitsPerSample / 8;
                Wfx.Format.nAvgBytesPerSec =
                    Wfx.Format.nSamplesPerSec * Wfx.Format.nBlockAlign;
                // If the method succeeds and the audio endpoint device supports the
                // specified stream format, it returns S_OK. If the method succeeds and
                // provides a closest match to the specified format, it returns S_FALSE.
                hr = _ptrClientIn->IsFormatSupported(
                    AUDCLNT_SHAREMODE_SHARED, (WAVEFORMATEX*)&Wfx, &pWfxClosestMatch);
                if (hr == S_OK) {
                    break;
                }
                else {
                    if (pWfxClosestMatch) {
                        RTC_LOG(INFO) << "nChannels=" << Wfx.Format.nChannels
                            << ", nSamplesPerSec=" << Wfx.Format.nSamplesPerSec
                            << " is not supported. Closest match: "
                            << "nChannels=" << pWfxClosestMatch->nChannels
                            << ", nSamplesPerSec="
                            << pWfxClosestMatch->nSamplesPerSec;
                        CoTaskMemFree(pWfxClosestMatch);
                        pWfxClosestMatch = nullptr;
                    }
                    else {
                        RTC_LOG(INFO) << "nChannels=" << Wfx.Format.nChannels
                            << ", nSamplesPerSec=" << Wfx.Format.nSamplesPerSec
                            << " is not supported. No closest match.";
                    }
                }
            }
            if (hr == S_OK)
                break;
        }

        if (hr == S_OK) {
            _recAudioFrameSize = Wfx.Format.nBlockAlign;
            _recSampleRate = Wfx.Format.nSamplesPerSec;
            _recBlockSize = Wfx.Format.nSamplesPerSec / 100;
            _recChannels = Wfx.Format.nChannels;

            RTC_LOG(LS_VERBOSE) << "VoE selected this capturing format:";
            RTC_LOG(LS_VERBOSE) << "wFormatTag        : 0x"
                << rtc::ToHex(Wfx.Format.wFormatTag) << " ("
                << Wfx.Format.wFormatTag << ")";
            RTC_LOG(LS_VERBOSE) << "nChannels         : " << Wfx.Format.nChannels;
            RTC_LOG(LS_VERBOSE) << "nSamplesPerSec    : " << Wfx.Format.nSamplesPerSec;
            RTC_LOG(LS_VERBOSE) << "nAvgBytesPerSec   : " << Wfx.Format.nAvgBytesPerSec;
            RTC_LOG(LS_VERBOSE) << "nBlockAlign       : " << Wfx.Format.nBlockAlign;
            RTC_LOG(LS_VERBOSE) << "wBitsPerSample    : " << Wfx.Format.wBitsPerSample;
            RTC_LOG(LS_VERBOSE) << "cbSize            : " << Wfx.Format.cbSize;
            RTC_LOG(LS_VERBOSE) << "Additional settings:";
            RTC_LOG(LS_VERBOSE) << "_recAudioFrameSize: " << _recAudioFrameSize;
            RTC_LOG(LS_VERBOSE) << "_recBlockSize     : " << _recBlockSize;
            RTC_LOG(LS_VERBOSE) << "_recChannels      : " << _recChannels;
        }

        // Create a capturing stream.
        hr = _ptrClientIn->Initialize(
            AUDCLNT_SHAREMODE_SHARED,  // share Audio Engine with other applications
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |  // processing of the audio buffer by
                                                 // the client will be event driven
            AUDCLNT_STREAMFLAGS_NOPERSIST |   // volume and mute settings for an
                                             // audio session will not persist
                                             // across system restarts
            AUDCLNT_STREAMFLAGS_LOOPBACK,    // [PV] Enabling loopback
            0,                    // required for event-driven shared mode
            0,                    // periodicity
            (WAVEFORMATEX*)&Wfx,  // selected wave format
        nullptr);                // session GUID

        if (hr != S_OK) {
            RTC_LOG(LS_ERROR) << "IAudioClient::Initialize() failed:";
        }
        EXIT_ON_ERROR(hr);

        if (_ptrAudioBuffer) {
            // Update the audio buffer with the selected parameters
            _ptrAudioBuffer->SetRecordingSampleRate(_recSampleRate);
            _ptrAudioBuffer->SetRecordingChannels((uint8_t)_recChannels);
        }
        else {
            // We can enter this state during CoreAudioIsSupported() when no
            // AudioDeviceImplementation has been created, hence the AudioDeviceBuffer
            // does not exist. It is OK to end up here since we don't initiate any media
            // in CoreAudioIsSupported().
            RTC_LOG(LS_VERBOSE)
                << "AudioDeviceBuffer must be attached before streaming can start";
        }

        // Get the actual size of the shared (endpoint buffer).
        // Typical value is 960 audio frames <=> 20ms @ 48kHz sample rate.
        hr = _ptrClientIn->GetBufferSize(&bufferFrameCount);
        if (SUCCEEDED(hr)) {
            RTC_LOG(LS_VERBOSE) << "IAudioClient::GetBufferSize() => "
                << bufferFrameCount << " (<=> "
                << bufferFrameCount * _recAudioFrameSize << " bytes)";
        }

        // Set the event handle that the system signals when an audio buffer is ready
        // to be processed by the client.
        hr = _ptrClientIn->SetEventHandle(_hCaptureSamplesReadyEvent);
        EXIT_ON_ERROR(hr);

        // Get an IAudioCaptureClient interface.
        SAFE_RELEASE(_ptrLoopbackClient);
        hr = _ptrClientIn->GetService(__uuidof(IAudioCaptureClient),
            (void**)&_ptrLoopbackClient);
        EXIT_ON_ERROR(hr);

        // Mark capture side as initialized
        _recIsInitialized = true;

        CoTaskMemFree(pWfxIn);
        CoTaskMemFree(pWfxClosestMatch);

        RTC_LOG(LS_VERBOSE) << "capture side is now initialized";
        return 0;

    Exit:
        _TraceCOMError(hr);
        CoTaskMemFree(pWfxIn);
        CoTaskMemFree(pWfxClosestMatch);
        SAFE_RELEASE(_ptrClientIn);
        SAFE_RELEASE(_ptrLoopbackClient);
        return -1;
    }

    // ----------------------------------------------------------------------------
    //  StartRecording
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::StartRecording() {
        if (!_recIsInitialized) {
            return -1;
        }

        if (_hRecThread != nullptr) {
            return 0;
        }

        if (_recording) {
            return 0;
        }

        {
            rtc::CritScope critScoped(&_critSect);

            // Create thread which will drive the capturing
            LPTHREAD_START_ROUTINE lpStartAddress = WSAPICaptureThread;

            assert(_hRecThread == NULL);
            _hRecThread = CreateThread(nullptr, 0, lpStartAddress, this, 0, nullptr);
            if (_hRecThread == nullptr) {
                RTC_LOG(LS_ERROR) << "failed to create the recording thread";
                return -1;
            }

            // Set thread priority to highest possible
            SetThreadPriority(_hRecThread, THREAD_PRIORITY_TIME_CRITICAL);

        }  // critScoped

        DWORD ret = WaitForSingleObject(_hCaptureStartedEvent, 1000);
        if (ret != WAIT_OBJECT_0) {
            RTC_LOG(LS_VERBOSE) << "capturing did not start up properly";
            return -1;
        }
        RTC_LOG(LS_VERBOSE) << "capture audio stream has now started...";

        _recording = true;

        return 0;
    }

    // ----------------------------------------------------------------------------
    //  StopRecording
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::StopRecording() {
        int32_t err = 0;

        if (!_recIsInitialized) {
            return 0;
        }

        _Lock();

        if (_hRecThread == nullptr) {
            RTC_LOG(LS_VERBOSE)
                << "no capturing stream is active => close down WASAPI only";
            SAFE_RELEASE(_ptrClientIn);
            SAFE_RELEASE(_ptrLoopbackClient);
            _recIsInitialized = false;
            _recording = false;
            _UnLock();
            return 0;
        }

        // Stop the driving thread...
        RTC_LOG(LS_VERBOSE) << "closing down the webrtc_core_audio_capture_thread...";
        // Manual-reset event; it will remain signalled to stop all capture threads.
        SetEvent(_hShutdownCaptureEvent);

        _UnLock();
        DWORD ret = WaitForSingleObject(_hRecThread, 2000);
        if (ret != WAIT_OBJECT_0) {
            RTC_LOG(LS_ERROR)
                << "failed to close down webrtc_core_audio_capture_thread";
            err = -1;
        }
        else {
            RTC_LOG(LS_VERBOSE) << "webrtc_core_audio_capture_thread is now closed";
        }
        _Lock();

        ResetEvent(_hShutdownCaptureEvent);  // Must be manually reset.
        // Ensure that the thread has released these interfaces properly.
        assert(err == -1 || _ptrClientIn == NULL);
        assert(err == -1 || _ptrLoopbackClient == NULL);

        _recIsInitialized = false;
        _recording = false;

        // These will create thread leaks in the result of an error,
        // but we can at least resume the call.
        CloseHandle(_hRecThread);
        _hRecThread = nullptr;

        _UnLock();

        return err;
    }

    // ----------------------------------------------------------------------------
    //  RecordingIsInitialized
    // ----------------------------------------------------------------------------

    bool CoreAudioDevice::RecordingIsInitialized() const {
        return (_recIsInitialized);
    }

    // ----------------------------------------------------------------------------
    //  Recording
    // ----------------------------------------------------------------------------

    bool CoreAudioDevice::Recording() const {
        return (_recording);
    }

    // ----------------------------------------------------------------------------
    //  PlayoutIsInitialized
    // ----------------------------------------------------------------------------

    bool CoreAudioDevice::PlayoutIsInitialized() const {
        return (_playIsInitialized);
    }

    // ----------------------------------------------------------------------------
    //  StartPlayout
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::StartPlayout() {
        if (!_playIsInitialized) {
            return -1;
        }
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  StopPlayout
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::StopPlayout() {
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  PlayoutDelay
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::PlayoutDelay(uint16_t& delayMS) const {
        delayMS = 0;
        return 0;
    }

    bool CoreAudioDevice::BuiltInAECIsAvailable() const {
        // [PV] Don't do any software AEC, this is all virtual, the mic is the speaker in the cloud
        return true;
    }

    // ----------------------------------------------------------------------------
    //  Playing
    // ----------------------------------------------------------------------------

    bool CoreAudioDevice::Playing() const {
        return false;
    }

    // ============================================================================
    //                                 Private Methods
    // ============================================================================

    // ----------------------------------------------------------------------------
    //  [static] WSAPICaptureThread
    // ----------------------------------------------------------------------------

    DWORD WINAPI CoreAudioDevice::WSAPICaptureThread(LPVOID context) {
        return reinterpret_cast<CoreAudioDevice*>(context)->DoCaptureThread();
    }

    DWORD CoreAudioDevice::InitCaptureThreadPriority() {
        _hMmTask = nullptr;

        rtc::SetCurrentThreadName("webrtc_core_audio_capture_thread");

        // Use Multimedia Class Scheduler Service (MMCSS) to boost the thread
        // priority.
        if (_winSupportAvrt) {
            DWORD taskIndex(0);
            _hMmTask = _PAvSetMmThreadCharacteristicsA("Pro Audio", &taskIndex);
            if (_hMmTask) {
                if (!_PAvSetMmThreadPriority(_hMmTask, AVRT_PRIORITY_CRITICAL)) {
                    RTC_LOG(LS_WARNING) << "failed to boost rec-thread using MMCSS";
                }
                RTC_LOG(LS_VERBOSE)
                    << "capture thread is now registered with MMCSS (taskIndex="
                    << taskIndex << ")";
            }
            else {
                RTC_LOG(LS_WARNING) << "failed to enable MMCSS on capture thread (err="
                    << GetLastError() << ")";
                _TraceCOMError(GetLastError());
            }
        }

        return S_OK;
    }

    void CoreAudioDevice::RevertCaptureThreadPriority() {
        if (_winSupportAvrt) {
            if (nullptr != _hMmTask) {
                _PAvRevertMmThreadCharacteristics(_hMmTask);
            }
        }

        _hMmTask = nullptr;
    }

    // ----------------------------------------------------------------------------
    //  DoCaptureThread
    // ----------------------------------------------------------------------------

    DWORD CoreAudioDevice::DoCaptureThread() {
        bool keepRecording = true;
        HANDLE waitArray[2] = { _hShutdownCaptureEvent, _hCaptureSamplesReadyEvent };
        HRESULT hr = S_OK;

        LARGE_INTEGER t1;

        BYTE* syncBuffer = nullptr;
        UINT32 syncBufIndex = 0;

        _readSamples = 0;

        // Initialize COM as MTA in this thread.
        ScopedCOMInitializer comInit(ScopedCOMInitializer::kMTA);
        if (!comInit.succeeded()) {
            RTC_LOG(LS_ERROR) << "failed to initialize COM in capture thread";
            return 1;
        }

        hr = InitCaptureThreadPriority();
        if (FAILED(hr)) {
            return hr;
        }

        _Lock();

        // Get size of capturing buffer (length is expressed as the number of audio
        // frames the buffer can hold). This value is fixed during the capturing
        // session.
        //
        UINT32 bufferLength = 0;

        REFERENCE_TIME latency;
        REFERENCE_TIME devPeriod = 0;
        REFERENCE_TIME devPeriodMin = 0;
        UINT32 syncBufferSize;
        double extraDelayMS;
        double endpointBufferSizeMS;

        if (_ptrClientIn == nullptr) {
            RTC_LOG(LS_ERROR)
                << "input state has been modified before capture loop starts.";
            return 1;
        }
        hr = _ptrClientIn->GetBufferSize(&bufferLength);
        EXIT_ON_ERROR(hr);
        RTC_LOG(LS_VERBOSE) << "[CAPT] size of buffer       : " << bufferLength;

        // Allocate memory for sync buffer.
        // It is used for compensation between native 44.1 and internal 44.0 and
        // for cases when the capture buffer is larger than 10ms.
        //
        syncBufferSize = 2 * (bufferLength * _recAudioFrameSize);
        syncBuffer = new BYTE[syncBufferSize];
        if (syncBuffer == nullptr) {
            return (DWORD)E_POINTER;
        }
        RTC_LOG(LS_VERBOSE) << "[CAPT] size of sync buffer  : " << syncBufferSize
            << " [bytes]";

        // Get maximum latency for the current stream (will not change for the
        // lifetime of the IAudioClient object).
        //
        _ptrClientIn->GetStreamLatency(&latency);
        RTC_LOG(LS_VERBOSE) << "[CAPT] max stream latency   : " << (DWORD)latency
            << " (" << (double)(latency / 10000.0) << " ms)";

        // Get the length of the periodic interval separating successive processing
        // passes by the audio engine on the data in the endpoint buffer.
        //
        _ptrClientIn->GetDevicePeriod(&devPeriod, &devPeriodMin);
        RTC_LOG(LS_VERBOSE) << "[CAPT] device period        : " << (DWORD)devPeriod
            << " (" << (double)(devPeriod / 10000.0) << " ms)";

        extraDelayMS = (double)((latency + devPeriod) / 10000.0);
        RTC_LOG(LS_VERBOSE) << "[CAPT] extraDelayMS         : " << extraDelayMS;

        endpointBufferSizeMS =
            10.0 * ((double)bufferLength / (double)_recBlockSize);
        RTC_LOG(LS_VERBOSE) << "[CAPT] endpointBufferSizeMS : "
            << endpointBufferSizeMS;

        // Start up the capturing stream.
        //
        hr = _ptrClientIn->Start();
        EXIT_ON_ERROR(hr);

        _UnLock();

        // Set event which will ensure that the calling thread modifies the recording
        // state to true.
        //
        SetEvent(_hCaptureStartedEvent);

        // >> ---------------------------- THREAD LOOP ----------------------------

        while (keepRecording) {
            // Wait for a capture notification event or a shutdown event
            DWORD waitResult = WaitForMultipleObjects(2, waitArray, FALSE, 500);
            switch (waitResult) {
            case WAIT_OBJECT_0 + 0:  // _hShutdownCaptureEvent
                keepRecording = false;
                break;
            case WAIT_OBJECT_0 + 1:  // _hCaptureSamplesReadyEvent
                break;
            case WAIT_TIMEOUT:  // timeout notification
                RTC_LOG(LS_WARNING) << "capture event timed out after 0.5 seconds";
                goto Exit;
            default:  // unexpected error
                RTC_LOG(LS_WARNING) << "unknown wait termination on capture side";
                goto Exit;
            }

            while (keepRecording) {
                BYTE* pData = nullptr;
                UINT32 framesAvailable = 0;
                DWORD flags = 0;
                UINT64 recTime = 0;
                UINT64 recPos = 0;

                _Lock();

                // Sanity check to ensure that essential states are not modified
                // during the unlocked period.
                if (_ptrLoopbackClient == nullptr || _ptrClientIn == nullptr) {
                    _UnLock();
                    RTC_LOG(LS_ERROR)
                        << "input state has been modified during unlocked period";
                    goto Exit;
                }

                //  Find out how much capture data is available
                //
                hr = _ptrLoopbackClient->GetBuffer(
                    &pData,            // packet which is ready to be read by used
                    &framesAvailable,  // #frames in the captured packet (can be zero)
                    &flags,            // support flags (check)
                    &recPos,    // device position of first audio frame in data packet
                    &recTime);  // value of performance counter at the time of recording
                                // the first audio frame

                if (SUCCEEDED(hr)) {
                    if (AUDCLNT_S_BUFFER_EMPTY == hr) {
                        // Buffer was empty => start waiting for a new capture notification
                        // event
                        _UnLock();
                        break;
                    }

                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        // Treat all of the data in the packet as silence and ignore the
                        // actual data values.
                        RTC_LOG(LS_WARNING) << "AUDCLNT_BUFFERFLAGS_SILENT";
                        pData = nullptr;
                    }

                    assert(framesAvailable != 0);

                    if (pData) {
                        CopyMemory(&syncBuffer[syncBufIndex * _recAudioFrameSize], pData,
                            framesAvailable * _recAudioFrameSize);
                    }
                    else {
                        ZeroMemory(&syncBuffer[syncBufIndex * _recAudioFrameSize],
                            framesAvailable * _recAudioFrameSize);
                    }
                    assert(syncBufferSize >= (syncBufIndex * _recAudioFrameSize) +
                        framesAvailable * _recAudioFrameSize);

                    // Release the capture buffer
                    //
                    hr = _ptrLoopbackClient->ReleaseBuffer(framesAvailable);
                    EXIT_ON_ERROR(hr);

                    _readSamples += framesAvailable;
                    syncBufIndex += framesAvailable;

                    QueryPerformanceCounter(&t1);

                    // // Get the current recording and playout delay.
                    uint32_t sndCardRecDelay = (uint32_t)(
                        ((((UINT64)t1.QuadPart * _perfCounterFactor) - recTime) / 10000) +
                        (10 * syncBufIndex) / _recBlockSize - 10);
                    uint32_t sndCardPlayDelay = 0; //  static_cast<uint32_t>(_sndCardPlayDelay);
                    //
                    _sndCardRecDelay = sndCardRecDelay;

                    while (syncBufIndex >= _recBlockSize) {
                        if (_ptrAudioBuffer) {
                            _ptrAudioBuffer->SetRecordedBuffer((const int8_t*)syncBuffer,
                                _recBlockSize);
                            _ptrAudioBuffer->SetVQEData(sndCardPlayDelay, sndCardRecDelay);

                            _ptrAudioBuffer->SetTypingStatus(KeyPressed());

                            _UnLock();  // release lock while making the callback
                            _ptrAudioBuffer->DeliverRecordedData();
                            _Lock();  // restore the lock

                            // Sanity check to ensure that essential states are not modified
                            // during the unlocked period
                            if (_ptrLoopbackClient == nullptr || _ptrClientIn == nullptr) {
                                _UnLock();
                                RTC_LOG(LS_ERROR) << "input state has been modified during"
                                    << " unlocked period";
                                goto Exit;
                            }
                        }

                        // store remaining data which was not able to deliver as 10ms segment
                        MoveMemory(&syncBuffer[0],
                            &syncBuffer[_recBlockSize * _recAudioFrameSize],
                            (syncBufIndex - _recBlockSize) * _recAudioFrameSize);
                        syncBufIndex -= _recBlockSize;
                        sndCardRecDelay -= 10;
                    }
                }
                else {
                    // If GetBuffer returns AUDCLNT_E_BUFFER_ERROR, the thread consuming the
                    // audio samples must wait for the next processing pass. The client
                    // might benefit from keeping a count of the failed GetBuffer calls. If
                    // GetBuffer returns this error repeatedly, the client can start a new
                    // processing loop after shutting down the current client by calling
                    // IAudioClient::Stop, IAudioClient::Reset, and releasing the audio
                    // client.
                    RTC_LOG(LS_ERROR) << "IAudioCaptureClient::GetBuffer returned"
                        << " AUDCLNT_E_BUFFER_ERROR, hr = 0x"
                        << rtc::ToHex(hr);
                    goto Exit;
                }

                _UnLock();
            }
        }

        // ---------------------------- THREAD LOOP ---------------------------- <<

        if (_ptrClientIn) {
            hr = _ptrClientIn->Stop();
        }

    Exit:
        if (FAILED(hr)) {
            _ptrClientIn->Stop();
            _UnLock();
            _TraceCOMError(hr);
        }

        RevertCaptureThreadPriority();

        _Lock();

        if (keepRecording) {
            if (_ptrClientIn != nullptr) {
                hr = _ptrClientIn->Stop();
                if (FAILED(hr)) {
                    _TraceCOMError(hr);
                }
                hr = _ptrClientIn->Reset();
                if (FAILED(hr)) {
                    _TraceCOMError(hr);
                }
            }

            RTC_LOG(LS_ERROR)
                << "Recording error: capturing thread has ended pre-maturely";
        }
        else {
            RTC_LOG(LS_VERBOSE) << "_Capturing thread is now terminated properly";
        }

        SAFE_RELEASE(_ptrClientIn);
        SAFE_RELEASE(_ptrLoopbackClient);

        _UnLock();

        if (syncBuffer) {
            delete[] syncBuffer;
        }

        return (DWORD)hr;
    }

    int32_t CoreAudioDevice::EnableBuiltInAEC(bool enable) {
        if (_recIsInitialized) {
            RTC_LOG(LS_ERROR)
                << "Attempt to set Windows AEC with recording already initialized";
            return -1;
        }
        return 0;
    }

    void CoreAudioDevice::_Lock() RTC_NO_THREAD_SAFETY_ANALYSIS {
        _critSect.Enter();
    }

    void CoreAudioDevice::_UnLock() RTC_NO_THREAD_SAFETY_ANALYSIS {
        _critSect.Leave();
    }

    int CoreAudioDevice::SetBoolProperty(IPropertyStore* ptrPS,
        REFPROPERTYKEY key,
        VARIANT_BOOL value) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_BOOL;
        pv.boolVal = value;
        HRESULT hr = ptrPS->SetValue(key, pv);
        PropVariantClear(&pv);
        if (FAILED(hr)) {
            _TraceCOMError(hr);
            return -1;
        }
        return 0;
    }

    int CoreAudioDevice::SetVtI4Property(IPropertyStore* ptrPS,
        REFPROPERTYKEY key,
        LONG value) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_I4;
        pv.lVal = value;
        HRESULT hr = ptrPS->SetValue(key, pv);
        PropVariantClear(&pv);
        if (FAILED(hr)) {
            _TraceCOMError(hr);
            return -1;
        }
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  _RefreshDeviceList
    //
    //  Creates a new list of endpoint rendering or capture devices after
    //  deleting any previously created (and possibly out-of-date) list of
    //  such devices.
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::_RefreshDeviceList() {
        RTC_LOG(LS_VERBOSE) << __FUNCTION__;

        HRESULT hr = S_OK;
        IMMDeviceCollection* pCollection = nullptr;

        assert(_ptrEnumerator != NULL);

        // Create a fresh list of devices using the specified direction
        hr = _ptrEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
            &pCollection);
        if (FAILED(hr)) {
            _TraceCOMError(hr);
            SAFE_RELEASE(pCollection);
            return -1;
        }

        SAFE_RELEASE(_ptrLoopbackCollection);
        _ptrLoopbackCollection = pCollection;

        return 0;
    }

    // ----------------------------------------------------------------------------
    //  _DeviceListCount
    //
    //  Gets a count of the endpoint rendering or capture devices in the
    //  current list of such devices.
    // ----------------------------------------------------------------------------

    int16_t CoreAudioDevice::_DeviceListCount() {
        RTC_LOG(LS_VERBOSE) << __FUNCTION__;

        HRESULT hr = S_OK;
        UINT count = 0;
        if (nullptr != _ptrLoopbackCollection) {
            hr = _ptrLoopbackCollection->GetCount(&count);
        }

        if (FAILED(hr)) {
            _TraceCOMError(hr);
            return -1;
        }

        return static_cast<int16_t>(count);
    }

    // ----------------------------------------------------------------------------
    //  _GetListDeviceName
    //
    //  Gets the friendly name of an endpoint rendering or capture device
    //  from the current list of such devices. The caller uses an index
    //  into the list to identify the device.
    //
    //  Uses: _ptrRenderCollection or _ptrCaptureCollection which is updated
    //  in _RefreshDeviceList().
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::_GetListDeviceName(
        int index,
        LPWSTR szBuffer,
        int bufferLen) {
        RTC_LOG(LS_VERBOSE) << __FUNCTION__;

        HRESULT hr = S_OK;
        IMMDevice* pDevice = nullptr;

        if (nullptr != _ptrLoopbackCollection) {
            hr = _ptrLoopbackCollection->Item(index, &pDevice);
        }

        if (FAILED(hr)) {
            _TraceCOMError(hr);
            SAFE_RELEASE(pDevice);
            return -1;
        }

        int32_t res = _GetDeviceName(pDevice, szBuffer, bufferLen);
        SAFE_RELEASE(pDevice);
        return res;
    }

    // ----------------------------------------------------------------------------
    //  _GetDefaultDeviceName
    //
    //  Gets the friendly name of an endpoint rendering or capture device
    //  given a specified device role.
    //
    //  Uses: _ptrEnumerator
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::_GetDefaultDeviceName(
        ERole role,
        LPWSTR szBuffer,
        int bufferLen) {
        RTC_LOG(LS_VERBOSE) << __FUNCTION__;

        HRESULT hr = S_OK;
        IMMDevice* pDevice = nullptr;

        assert(role == eConsole || role == eCommunications);
        assert(_ptrEnumerator != NULL);

        hr = _ptrEnumerator->GetDefaultAudioEndpoint(eRender, role, &pDevice);

        if (FAILED(hr)) {
            _TraceCOMError(hr);
            SAFE_RELEASE(pDevice);
            return -1;
        }

        int32_t res = _GetDeviceName(pDevice, szBuffer, bufferLen);
        SAFE_RELEASE(pDevice);
        return res;
    }

    // ----------------------------------------------------------------------------
    //  _GetListDeviceID
    //
    //  Gets the unique ID string of an endpoint rendering or capture device
    //  from the current list of such devices. The caller uses an index
    //  into the list to identify the device.
    //
    //  Uses: _ptrRenderCollection or _ptrCaptureCollection which is updated
    //  in _RefreshDeviceList().
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::_GetListDeviceID(
        int index,
        LPWSTR szBuffer,
        int bufferLen) {
        RTC_LOG(LS_VERBOSE) << __FUNCTION__;

        HRESULT hr = S_OK;
        IMMDevice* pDevice = nullptr;

        if (nullptr != _ptrLoopbackCollection) {
            hr = _ptrLoopbackCollection->Item(index, &pDevice);
        }

        if (FAILED(hr)) {
            _TraceCOMError(hr);
            SAFE_RELEASE(pDevice);
            return -1;
        }

        int32_t res = _GetDeviceID(pDevice, szBuffer, bufferLen);
        SAFE_RELEASE(pDevice);
        return res;
    }

    // ----------------------------------------------------------------------------
    //  _GetDefaultDeviceID
    //
    //  Gets the uniqe device ID of an endpoint rendering or capture device
    //  given a specified device role.
    //
    //  Uses: _ptrEnumerator
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::_GetDefaultDeviceID(
        ERole role,
        LPWSTR szBuffer,
        int bufferLen) {
        RTC_LOG(LS_VERBOSE) << __FUNCTION__;

        HRESULT hr = S_OK;
        IMMDevice* pDevice = nullptr;

        assert(role == eConsole || role == eCommunications);
        assert(_ptrEnumerator != NULL);

        hr = _ptrEnumerator->GetDefaultAudioEndpoint(eRender, role, &pDevice);

        if (FAILED(hr)) {
            _TraceCOMError(hr);
            SAFE_RELEASE(pDevice);
            return -1;
        }

        int32_t res = _GetDeviceID(pDevice, szBuffer, bufferLen);
        SAFE_RELEASE(pDevice);
        return res;
    }

    int32_t CoreAudioDevice::_GetDefaultDeviceIndex(
        ERole role,
        int* index) {
        RTC_LOG(LS_VERBOSE) << __FUNCTION__;

        HRESULT hr = S_OK;
        WCHAR szDefaultDeviceID[MAX_PATH] = { 0 };
        WCHAR szDeviceID[MAX_PATH] = { 0 };

        const size_t kDeviceIDLength = sizeof(szDeviceID) / sizeof(szDeviceID[0]);
        assert(kDeviceIDLength ==
            sizeof(szDefaultDeviceID) / sizeof(szDefaultDeviceID[0]));

        if (_GetDefaultDeviceID(role, szDefaultDeviceID, kDeviceIDLength) ==
            -1) {
            return -1;
        }

        IMMDeviceCollection* collection = _ptrLoopbackCollection;

        if (!collection) {
            RTC_LOG(LS_ERROR) << "Device collection not valid";
            return -1;
        }

        UINT count = 0;
        hr = collection->GetCount(&count);
        if (FAILED(hr)) {
            _TraceCOMError(hr);
            return -1;
        }

        *index = -1;
        for (UINT i = 0; i < count; i++) {
            memset(szDeviceID, 0, sizeof(szDeviceID));
            rtc::scoped_refptr<IMMDevice> device;
            {
                IMMDevice* ptrDevice = nullptr;
                hr = collection->Item(i, &ptrDevice);
                if (FAILED(hr) || ptrDevice == nullptr) {
                    _TraceCOMError(hr);
                    return -1;
                }
                device = ptrDevice;
                SAFE_RELEASE(ptrDevice);
            }

            if (_GetDeviceID(device, szDeviceID, kDeviceIDLength) == -1) {
                return -1;
            }

            if (wcsncmp(szDefaultDeviceID, szDeviceID, kDeviceIDLength) == 0) {
                // Found a match.
                *index = i;
                break;
            }
        }

        if (*index == -1) {
            RTC_LOG(LS_ERROR) << "Unable to find collection index for default device";
            return -1;
        }

        return 0;
    }

    // ----------------------------------------------------------------------------
    //  _GetDeviceName
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::_GetDeviceName(IMMDevice* pDevice,
        LPWSTR pszBuffer,
        int bufferLen) {
        RTC_LOG(LS_VERBOSE) << __FUNCTION__;

        static const WCHAR szDefault[] = L"<Device not available>";

        HRESULT hr = E_FAIL;
        IPropertyStore* pProps = nullptr;
        PROPVARIANT varName;

        assert(pszBuffer != NULL);
        assert(bufferLen > 0);

        if (pDevice != nullptr) {
            hr = pDevice->OpenPropertyStore(STGM_READ, &pProps);
            if (FAILED(hr)) {
                RTC_LOG(LS_ERROR) << "IMMDevice::OpenPropertyStore failed, hr = 0x"
                    << rtc::ToHex(hr);
            }
        }

        // Initialize container for property value.
        PropVariantInit(&varName);

        if (SUCCEEDED(hr)) {
            // Get the endpoint device's friendly-name property.
            hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
            if (FAILED(hr)) {
                RTC_LOG(LS_ERROR) << "IPropertyStore::GetValue failed, hr = 0x"
                    << rtc::ToHex(hr);
            }
        }

        if ((SUCCEEDED(hr)) && (VT_EMPTY == varName.vt)) {
            hr = E_FAIL;
            RTC_LOG(LS_ERROR) << "IPropertyStore::GetValue returned no value,"
                << " hr = 0x" << rtc::ToHex(hr);
        }

        if ((SUCCEEDED(hr)) && (VT_LPWSTR != varName.vt)) {
            // The returned value is not a wide null terminated string.
            hr = E_UNEXPECTED;
            RTC_LOG(LS_ERROR) << "IPropertyStore::GetValue returned unexpected"
                << " type, hr = 0x" << rtc::ToHex(hr);
        }

        if (SUCCEEDED(hr) && (varName.pwszVal != nullptr)) {
            // Copy the valid device name to the provided ouput buffer.
            wcsncpy_s(pszBuffer, bufferLen, varName.pwszVal, _TRUNCATE);
        }
        else {
            // Failed to find the device name.
            wcsncpy_s(pszBuffer, bufferLen, szDefault, _TRUNCATE);
        }

        PropVariantClear(&varName);
        SAFE_RELEASE(pProps);

        return 0;
    }

    // ----------------------------------------------------------------------------
    //  _GetDeviceID
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::_GetDeviceID(IMMDevice* pDevice,
        LPWSTR pszBuffer,
        int bufferLen) {
        RTC_LOG(LS_VERBOSE) << __FUNCTION__;

        static const WCHAR szDefault[] = L"<Device not available>";

        HRESULT hr = E_FAIL;
        LPWSTR pwszID = nullptr;

        assert(pszBuffer != NULL);
        assert(bufferLen > 0);

        if (pDevice != nullptr) {
            hr = pDevice->GetId(&pwszID);
        }

        if (hr == S_OK) {
            // Found the device ID.
            wcsncpy_s(pszBuffer, bufferLen, pwszID, _TRUNCATE);
        }
        else {
            // Failed to find the device ID.
            wcsncpy_s(pszBuffer, bufferLen, szDefault, _TRUNCATE);
        }

        CoTaskMemFree(pwszID);
        return 0;
    }

    // ----------------------------------------------------------------------------
    //  _GetDefaultDevice
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::_GetDefaultDevice(
        ERole role,
        IMMDevice** ppDevice) {
        RTC_LOG(LS_VERBOSE) << __FUNCTION__;

        HRESULT hr(S_OK);

        assert(_ptrEnumerator != NULL);

        hr = _ptrEnumerator->GetDefaultAudioEndpoint(eRender, role, ppDevice);
        if (FAILED(hr)) {
            _TraceCOMError(hr);
            return -1;
        }

        return 0;
    }

    // ----------------------------------------------------------------------------
    //  _GetListDevice
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::_GetListDevice(
        int index,
        IMMDevice** ppDevice) {
        HRESULT hr(S_OK);

        assert(_ptrEnumerator != NULL);

        IMMDeviceCollection* pCollection = nullptr;

        hr = _ptrEnumerator->EnumAudioEndpoints(
            eRender,
            DEVICE_STATE_ACTIVE,  // only active endpoints are OK
            &pCollection);
        if (FAILED(hr)) {
            _TraceCOMError(hr);
            SAFE_RELEASE(pCollection);
            return -1;
        }

        hr = pCollection->Item(index, ppDevice);
        if (FAILED(hr)) {
            _TraceCOMError(hr);
            SAFE_RELEASE(pCollection);
            return -1;
        }

        return 0;
    }

    // ----------------------------------------------------------------------------
    //  _EnumerateEndpointDevicesAll
    // ----------------------------------------------------------------------------

    int32_t CoreAudioDevice::_EnumerateEndpointDevicesAll() const {
        RTC_LOG(LS_VERBOSE) << __FUNCTION__;

        assert(_ptrEnumerator != NULL);

        HRESULT hr = S_OK;
        IMMDeviceCollection* pCollection = nullptr;
        IMMDevice* pEndpoint = nullptr;
        IPropertyStore* pProps = nullptr;
        IAudioEndpointVolume* pEndpointVolume = nullptr;
        LPWSTR pwszID = nullptr;
        UINT count = 0;


        // Generate a collection of audio endpoint devices in the system.
        // Get states for *all* endpoint devices.
        // Output: IMMDeviceCollection interface.
        hr = _ptrEnumerator->EnumAudioEndpoints(
            eRender,  // data-flow direction (input parameter)
            DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED | DEVICE_STATE_UNPLUGGED,
            &pCollection);  // release interface when done

        EXIT_ON_ERROR(hr);

        // use the IMMDeviceCollection interface...

        // Retrieve a count of the devices in the device collection.
        hr = pCollection->GetCount(&count);
        EXIT_ON_ERROR(hr);
        RTC_LOG(LS_VERBOSE) << "#loopback endpoint devices (counting all): " << count;

        if (count == 0) {
            return 0;
        }

        // Each loop prints the name of an endpoint device.
        for (ULONG i = 0; i < count; i++) {
            DWORD dwHwSupportMask = 0;
            UINT nChannelCount(0);
            UINT nStep(0);
            UINT nStepCount(0);

            RTC_LOG(LS_VERBOSE) << "Endpoint " << i << ":";

            // Get pointer to endpoint number i.
            // Output: IMMDevice interface.
            hr = pCollection->Item(i, &pEndpoint);
            CONTINUE_ON_ERROR(hr);

            // use the IMMDevice interface of the specified endpoint device...

            // Get the endpoint ID string (uniquely identifies the device among all
            // audio endpoint devices)
            hr = pEndpoint->GetId(&pwszID);
            CONTINUE_ON_ERROR(hr);
            RTC_LOG(LS_VERBOSE) << "ID string    : " << pwszID;

            // Retrieve an interface to the device's property store.
            // Output: IPropertyStore interface.
            hr = pEndpoint->OpenPropertyStore(STGM_READ, &pProps);
            CONTINUE_ON_ERROR(hr);

            // use the IPropertyStore interface...

            PROPVARIANT varName;
            // Initialize container for property value.
            PropVariantInit(&varName);

            // Get the endpoint's friendly-name property.
            // Example: "Speakers (Realtek High Definition Audio)"
            hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
            CONTINUE_ON_ERROR(hr);
            RTC_LOG(LS_VERBOSE) << "friendly name: \"" << varName.pwszVal << "\"";

            // Get the endpoint's current device state
            DWORD dwState;
            hr = pEndpoint->GetState(&dwState);
            CONTINUE_ON_ERROR(hr);
            if (dwState & DEVICE_STATE_ACTIVE)
                RTC_LOG(LS_VERBOSE) << "state (0x" << rtc::ToHex(dwState)
                << ")  : *ACTIVE*";
            if (dwState & DEVICE_STATE_DISABLED)
                RTC_LOG(LS_VERBOSE) << "state (0x" << rtc::ToHex(dwState)
                << ")  : DISABLED";
            if (dwState & DEVICE_STATE_NOTPRESENT)
                RTC_LOG(LS_VERBOSE) << "state (0x" << rtc::ToHex(dwState)
                << ")  : NOTPRESENT";
            if (dwState & DEVICE_STATE_UNPLUGGED)
                RTC_LOG(LS_VERBOSE) << "state (0x" << rtc::ToHex(dwState)
                << ")  : UNPLUGGED";

            // Check the hardware volume capabilities.
            hr = pEndpoint->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                (void**)&pEndpointVolume);
            CONTINUE_ON_ERROR(hr);
            hr = pEndpointVolume->QueryHardwareSupport(&dwHwSupportMask);
            CONTINUE_ON_ERROR(hr);
            if (dwHwSupportMask & ENDPOINT_HARDWARE_SUPPORT_VOLUME)
                // The audio endpoint device supports a hardware volume control
                RTC_LOG(LS_VERBOSE) << "hwmask (0x" << rtc::ToHex(dwHwSupportMask)
                << ") : HARDWARE_SUPPORT_VOLUME";
            if (dwHwSupportMask & ENDPOINT_HARDWARE_SUPPORT_MUTE)
                // The audio endpoint device supports a hardware mute control
                RTC_LOG(LS_VERBOSE) << "hwmask (0x" << rtc::ToHex(dwHwSupportMask)
                << ") : HARDWARE_SUPPORT_MUTE";
            if (dwHwSupportMask & ENDPOINT_HARDWARE_SUPPORT_METER)
                // The audio endpoint device supports a hardware peak meter
                RTC_LOG(LS_VERBOSE) << "hwmask (0x" << rtc::ToHex(dwHwSupportMask)
                << ") : HARDWARE_SUPPORT_METER";

            // Check the channel count (#channels in the audio stream that enters or
            // leaves the audio endpoint device)
            hr = pEndpointVolume->GetChannelCount(&nChannelCount);
            CONTINUE_ON_ERROR(hr);
            RTC_LOG(LS_VERBOSE) << "#channels    : " << nChannelCount;

            if (dwHwSupportMask & ENDPOINT_HARDWARE_SUPPORT_VOLUME) {
                // Get the volume range.
                float fLevelMinDB(0.0);
                float fLevelMaxDB(0.0);
                float fVolumeIncrementDB(0.0);
                hr = pEndpointVolume->GetVolumeRange(&fLevelMinDB, &fLevelMaxDB,
                    &fVolumeIncrementDB);
                CONTINUE_ON_ERROR(hr);
                RTC_LOG(LS_VERBOSE) << "volume range : " << fLevelMinDB << " (min), "
                    << fLevelMaxDB << " (max), " << fVolumeIncrementDB
                    << " (inc) [dB]";

                // The volume range from vmin = fLevelMinDB to vmax = fLevelMaxDB is
                // divided into n uniform intervals of size vinc = fVolumeIncrementDB,
                // where n = (vmax ?vmin) / vinc. The values vmin, vmax, and vinc are
                // measured in decibels. The client can set the volume level to one of n +
                // 1 discrete values in the range from vmin to vmax.
                int n = (int)((fLevelMaxDB - fLevelMinDB) / fVolumeIncrementDB);
                RTC_LOG(LS_VERBOSE) << "#intervals   : " << n;

                // Get information about the current step in the volume range.
                // This method represents the volume level of the audio stream that enters
                // or leaves the audio endpoint device as an index or "step" in a range of
                // discrete volume levels. Output value nStepCount is the number of steps
                // in the range. Output value nStep is the step index of the current
                // volume level. If the number of steps is n = nStepCount, then step index
                // nStep can assume values from 0 (minimum volume) to n ?1 (maximum
                // volume).
                hr = pEndpointVolume->GetVolumeStepInfo(&nStep, &nStepCount);
                CONTINUE_ON_ERROR(hr);
                RTC_LOG(LS_VERBOSE) << "volume steps : " << nStep << " (nStep), "
                    << nStepCount << " (nStepCount)";
            }
        Next:
            if (FAILED(hr)) {
                RTC_LOG(LS_VERBOSE) << "Error when logging device information";
            }
            CoTaskMemFree(pwszID);
            pwszID = nullptr;
            PropVariantClear(&varName);
            SAFE_RELEASE(pProps);
            SAFE_RELEASE(pEndpoint);
            SAFE_RELEASE(pEndpointVolume);
        }
        SAFE_RELEASE(pCollection);
        return 0;

    Exit:
        _TraceCOMError(hr);
        CoTaskMemFree(pwszID);
        pwszID = nullptr;
        SAFE_RELEASE(pCollection);
        SAFE_RELEASE(pEndpoint);
        SAFE_RELEASE(pEndpointVolume);
        SAFE_RELEASE(pProps);
        return -1;
    }

    // ----------------------------------------------------------------------------
    //  _TraceCOMError
    // ----------------------------------------------------------------------------

    void CoreAudioDevice::_TraceCOMError(HRESULT hr) const {
        TCHAR buf[MAXERRORLENGTH];
        TCHAR errorText[MAXERRORLENGTH];

        const DWORD dwFlags =
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        const DWORD dwLangID = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);

        // Gets the system's human readable message string for this HRESULT.
        // All error message in English by default.
        DWORD messageLength = ::FormatMessageW(dwFlags, nullptr, hr, dwLangID, errorText,
            MAXERRORLENGTH, nullptr);

        assert(messageLength <= MAXERRORLENGTH);

        // Trims tailing white space (FormatMessage() leaves a trailing cr-lf.).
        for (; messageLength && ::isspace(errorText[messageLength - 1]);
            --messageLength) {
            errorText[messageLength - 1] = '\0';
        }

        RTC_LOG(LS_ERROR) << "Core Audio method failed (hr=" << hr << ")";
        StringCchPrintf(buf, MAXERRORLENGTH, TEXT("Error details: "));
        StringCchCat(buf, MAXERRORLENGTH, errorText);
        RTC_LOG(LS_ERROR) << WideToUTF8(buf);
    }

    // ----------------------------------------------------------------------------
    //  WideToUTF8
    // ----------------------------------------------------------------------------

    char* CoreAudioDevice::WideToUTF8(const TCHAR* src) const {
#ifdef UNICODE
        const size_t kStrLen = sizeof(_str);
        memset(_str, 0, kStrLen);
        // Get required size (in bytes) to be able to complete the conversion.
        unsigned int required_size =
            (unsigned int)WideCharToMultiByte(CP_UTF8, 0, src, -1, _str, 0, nullptr, nullptr);
        if (required_size <= kStrLen) {
            // Process the entire input string, including the terminating null char.
            if (WideCharToMultiByte(CP_UTF8, 0, src, -1, _str, kStrLen, nullptr, nullptr) == 0)
                memset(_str, 0, kStrLen);
        }
        return _str;
#else
        return const_cast<char*>(src);
#endif
    }

    bool CoreAudioDevice::KeyPressed() const {
        int key_down = 0;
        for (int key = VK_SPACE; key < VK_NUMLOCK; key++) {
            short res = GetAsyncKeyState(key);
            key_down |= res & 0x1;  // Get the LSB
        }
        return (key_down > 0);
    }
}  // namespace webrtc

