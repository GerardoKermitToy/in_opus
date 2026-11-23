// Optional compile-time verification against the actual public SDK headers.
// The headers are reference-only and are not redistributed or linked.
#include "winamp_input.hpp"
#define UNICODE_INPUT_PLUGIN
#include <IN2.H>

static_assert(IN_VER_OLD == winamp::kInputVersion);
static_assert(sizeof(In_Module) == sizeof(winamp::InModule));
static_assert(sizeof(Out_Module) == sizeof(winamp::OutModule));
#define INPUT_FIELD(name) static_assert(offsetof(In_Module, name) == offsetof(winamp::InModule, name))
INPUT_FIELD(version); INPUT_FIELD(description); INPUT_FIELD(hMainWindow); INPUT_FIELD(hDllInstance);
INPUT_FIELD(FileExtensions); INPUT_FIELD(is_seekable); INPUT_FIELD(UsesOutputPlug);
INPUT_FIELD(Config); INPUT_FIELD(About); INPUT_FIELD(Init); INPUT_FIELD(Quit);
INPUT_FIELD(GetFileInfo); INPUT_FIELD(InfoBox); INPUT_FIELD(IsOurFile); INPUT_FIELD(Play);
INPUT_FIELD(Pause); INPUT_FIELD(UnPause); INPUT_FIELD(IsPaused); INPUT_FIELD(Stop);
INPUT_FIELD(GetLength); INPUT_FIELD(GetOutputTime); INPUT_FIELD(SetOutputTime);
INPUT_FIELD(SetVolume); INPUT_FIELD(SetPan); INPUT_FIELD(SAVSAInit); INPUT_FIELD(SAVSADeInit);
INPUT_FIELD(SAAddPCMData); INPUT_FIELD(SAGetMode); INPUT_FIELD(SAAdd);
INPUT_FIELD(VSAAddPCMData); INPUT_FIELD(VSAGetMode); INPUT_FIELD(VSAAdd); INPUT_FIELD(VSASetInfo);
INPUT_FIELD(dsp_isactive); INPUT_FIELD(dsp_dosamples); INPUT_FIELD(EQSet); INPUT_FIELD(SetInfo);
INPUT_FIELD(outMod); INPUT_FIELD(service);
#undef INPUT_FIELD
#define OUTPUT_FIELD(name) static_assert(offsetof(Out_Module, name) == offsetof(winamp::OutModule, name))
OUTPUT_FIELD(version); OUTPUT_FIELD(description); OUTPUT_FIELD(id);
OUTPUT_FIELD(hMainWindow); OUTPUT_FIELD(hDllInstance); OUTPUT_FIELD(Config); OUTPUT_FIELD(About);
OUTPUT_FIELD(Init); OUTPUT_FIELD(Quit); OUTPUT_FIELD(Open); OUTPUT_FIELD(Close);
OUTPUT_FIELD(Write); OUTPUT_FIELD(CanWrite); OUTPUT_FIELD(IsPlaying); OUTPUT_FIELD(Pause);
OUTPUT_FIELD(SetVolume); OUTPUT_FIELD(SetPan); OUTPUT_FIELD(Flush);
OUTPUT_FIELD(GetOutputTime); OUTPUT_FIELD(GetWrittenTime);
#undef OUTPUT_FIELD
