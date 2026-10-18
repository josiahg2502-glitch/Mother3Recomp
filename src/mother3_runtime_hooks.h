#pragma once
#include <cstddef>
#include <cstdint>

void mother3_install_runtime_hooks();
bool mother3_apply_hq_audio_patch(std::uint8_t* rom, std::size_t rom_size,
                                  const char* source_sha1);
