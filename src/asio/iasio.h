// Clean-room declaration of the Steinberg ASIO driver COM interface (binary compatible
// with hosts built against the ASIO SDK). Written from the publicly documented ABI; no
// Steinberg SDK code is included.
#pragma once
#include <windows.h>
#include <unknwn.h>

typedef long   ASIOBool;
typedef long   ASIOError;
typedef double ASIOSampleRate;
typedef long   ASIOSampleType;
enum { ASIOFalse = 0, ASIOTrue = 1 };

struct ASIOSamples   { unsigned long hi; unsigned long lo; };
struct ASIOTimeStamp { unsigned long hi; unsigned long lo; };

enum {
    ASIOSTInt16MSB = 0, ASIOSTInt24MSB = 1, ASIOSTInt32MSB = 2, ASIOSTFloat32MSB = 3, ASIOSTFloat64MSB = 4,
    ASIOSTInt32MSB16 = 8, ASIOSTInt32MSB18 = 9, ASIOSTInt32MSB20 = 10, ASIOSTInt32MSB24 = 11,
    ASIOSTInt16LSB = 16, ASIOSTInt24LSB = 17, ASIOSTInt32LSB = 18, ASIOSTFloat32LSB = 19, ASIOSTFloat64LSB = 20,
    ASIOSTInt32LSB16 = 24, ASIOSTInt32LSB18 = 25, ASIOSTInt32LSB20 = 26, ASIOSTInt32LSB24 = 27,
};

enum {
    ASE_OK = 0, ASE_SUCCESS = 0x3f4847a0, ASE_NotPresent = -1000, ASE_HWMalfunction, ASE_InvalidParameter,
    ASE_InvalidMode, ASE_SPNotAdvancing, ASE_NoClock, ASE_NoMemory,
};

struct ASIOClockSource { long index; long associatedChannel; long associatedGroup; ASIOBool isCurrentSource; char name[32]; };
struct ASIOChannelInfo { long channel; ASIOBool isInput; ASIOBool isActive; long channelGroup; ASIOSampleType type; char name[32]; };
struct ASIOBufferInfo  { ASIOBool isInput; long channelNum; void* buffers[2]; };
struct ASIOTimeCode    { double speed; ASIOSamples timeCodeSamples; unsigned long flags; char future[64]; };
struct ASIOTimeInfo    { double speed; ASIOTimeStamp systemTime; ASIOSamples samplePosition; ASIOSampleRate sampleRate; unsigned long flags; char reserved[12]; };
struct ASIOTime        { long reserved[4]; ASIOTimeInfo timeInfo; ASIOTimeCode timeCode; };

enum { kSystemTimeValid = 1, kSamplePositionValid = 2, kSampleRateValid = 4, kSpeedValid = 8, kSampleRateChanged = 16, kClockSourceChanged = 32 };

struct ASIOCallbacks {
    void      (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void      (*sampleRateDidChange)(ASIOSampleRate sRate);
    long      (*asioMessage)(long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
};

enum {
    kAsioSelectorSupported = 1, kAsioEngineVersion, kAsioResetRequest, kAsioBufferSizeChange, kAsioResyncRequest,
    kAsioLatenciesChanged, kAsioSupportsTimeInfo, kAsioSupportsTimeCode, kAsioMMCCommand, kAsioSupportsInputMonitor,
    kAsioSupportsInputGain, kAsioSupportsInputMeter, kAsioSupportsOutputGain, kAsioSupportsOutputMeter, kAsioOverload,
};

struct IASIO : public IUnknown {
    virtual ASIOBool  init(void* sysHandle) = 0;
    virtual void      getDriverName(char* name) = 0;
    virtual long      getDriverVersion() = 0;
    virtual void      getErrorMessage(char* string) = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
    virtual ASIOError setClockSource(long reference) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(long selector, void* opt) = 0;
    virtual ASIOError outputReady() = 0;
};
