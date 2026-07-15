#ifndef CHRONOFORGE_BRIDGE_H
#define CHRONOFORGE_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CFVideoBuffer CFVideoBuffer;
typedef int32_t (*CFRenderProgressCallback)(double fraction, const char* stage, void* context);

typedef struct CFFileTensorInfo {
    uint64_t frames;
    uint64_t height;
    uint64_t width;
    uint64_t channels;
    uint32_t frame_rate_numerator;
    uint32_t frame_rate_denominator;
} CFFileTensorInfo;

typedef enum CFEffectKind {
    CF_EFFECT_SPACE_TIME_TRANSPOSE = 0,
    CF_EFFECT_LUMA_TIME_SHIFT = 1,
    CF_EFFECT_RADIAL_CHRONO_FUNNEL = 2,
    CF_EFFECT_TEMPORAL_PIXEL_SORT = 3,
    CF_EFFECT_TENSOR_3D_ROTATION = 4,
    CF_EFFECT_SPECTRAL_FFT_SWAP = 5,
} CFEffectKind;

// Generic ABI-stable parameter packet. Its interpretation is defined by kind:
// transpose:              options = X(0) / Y(1), Native(0) / Fit source canvas(1)
// luma-time shift:        values[0] = multiplier, options = source, edge
// radial funnel:          values = centerX, centerY, intensity; options[0] = edge
// temporal pixel sort:    values[0] = threshold; options = criterion, direction
// tensor 3D rotation:     values = XY, XT, YT degrees; options[0] = fill mode
// spectral FFT swap:      options = axis, normalize
typedef struct CFEffectDescriptor {
    int32_t kind;
    float values[4];
    int32_t options[4];
} CFEffectDescriptor;

CFEffectDescriptor cf_effect_descriptor_make(
    int32_t kind,
    float value0,
    float value1,
    float value2,
    float value3,
    int32_t option0,
    int32_t option1,
    int32_t option2,
    int32_t option3);

const char* cf_core_version(void);

// Returns 0 on success. Input samples use canonical T,H,W,C order and linear
// float colour. The returned opaque buffer must be released with
// cf_video_buffer_destroy.
int32_t cf_render_effect_chain(
    const float* input,
    uint64_t frames,
    uint64_t height,
    uint64_t width,
    uint64_t channels,
    uint32_t frame_rate_numerator,
    uint32_t frame_rate_denominator,
    const CFEffectDescriptor* effects,
    uint64_t effect_count,
    uint64_t max_working_set_bytes,
    CFVideoBuffer** output,
    char* error_message,
    uint64_t error_message_capacity);

// Processes a headerless linear-float T,H,W,C tensor through memory-mapped
// cache files. Local and temporal effects stay out-of-core; the FFT node still
// obeys max_working_set_bytes and fails before an unsafe allocation.
int32_t cf_render_file_effect_chain(
    const char* input_path,
    const char* output_path,
    const char* scratch_directory,
    CFFileTensorInfo input_info,
    const CFEffectDescriptor* effects,
    uint64_t effect_count,
    uint64_t max_working_set_bytes,
    CFRenderProgressCallback progress,
    void* progress_context,
    CFFileTensorInfo* output_info,
    char* error_message,
    uint64_t error_message_capacity);

uint64_t cf_video_buffer_frames(const CFVideoBuffer* buffer);
uint64_t cf_video_buffer_height(const CFVideoBuffer* buffer);
uint64_t cf_video_buffer_width(const CFVideoBuffer* buffer);
uint64_t cf_video_buffer_channels(const CFVideoBuffer* buffer);
uint64_t cf_video_buffer_value_count(const CFVideoBuffer* buffer);
const float* cf_video_buffer_values(const CFVideoBuffer* buffer);
void cf_video_buffer_destroy(CFVideoBuffer* buffer);

#ifdef __cplusplus
}
#endif

#endif
