#pragma once

// Whether Retro Rewind may skip drawing a frame to keep the game at full speed.
bool RetroRewindFrameSkip() noexcept;
void SetRetroRewindFrameSkip(bool on) noexcept;
