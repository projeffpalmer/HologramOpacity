#include "HologramOpacityModule.h"

DEFINE_LOG_CATEGORY(LogHologramOpacity);

void FHologramOpacityModule::StartupModule()
{
	UE_LOG(LogHologramOpacity, Display, TEXT("Hologram Opacity module loaded"));
}

void FHologramOpacityModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FHologramOpacityModule, HologramOpacity);
