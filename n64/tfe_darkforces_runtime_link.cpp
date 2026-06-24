#include <debug.h>

#include <TFE_System/types.h>

namespace TFE_DarkForces
{
	#if !TFE_N64_LINK_DF_RUNTIME
	// Temporary bootstrap provider to unblock runtime bridge tracing.
	// When the real Dark Forces runtime is linked, its strong symbol should replace this.
	void __attribute__((weak)) startMissionFromSave(s32 levelIndex)
	{
		debugf("[df_runtime_stub] startMissionFromSave(%ld) reached stub provider\n", (long)levelIndex);
		debugf("[df_runtime_stub] real Dark Forces runtime link not enabled in this lane\n");
	}
	#endif
}
