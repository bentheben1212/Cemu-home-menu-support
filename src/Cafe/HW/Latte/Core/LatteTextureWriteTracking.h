#pragma once

// Tracks CPU writes made through Cemu's HLE memory functions (memcpy, memset, OSBlockMove, OSBlockSet) into memory that
// backs cached textures. The per-frame texture change check samples texture data, so it can miss small CPU updates (for
// example new glyphs in a font texture); a recorded write makes the next check reload the texture.
// Enabled by default, disable with CEMU_TEXTURE_WRITE_TRACKING=0

class LatteTexture;

bool LatteTextureWriteTracking_IsEnabled();
// CPU side, any thread: a guest memory range was written
void LatteTextureWriteTracking_NotifyWrite(uint32 physAddr, uint32 size);
// GPU side: texture data range (texDataPtrLow/High) was (re)assigned or the texture is deleted
void LatteTextureWriteTracking_Register(LatteTexture* texture);
void LatteTextureWriteTracking_Unregister(LatteTexture* texture);
// GPU side: returns true if a tracked write touched the texture since its last check
bool LatteTextureWriteTracking_ConsumeWrites(LatteTexture* texture);
// per title statistics
void LatteTextureWriteTracking_CountExtraReload(LatteTexture* texture);
void LatteTextureWriteTracking_LogSummaryAndReset();
