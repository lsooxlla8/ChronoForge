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
    CF_EFFECT_SELECTIVE_PREFILTER = 6,
    CF_EFFECT_DIMENSIONAL_SPLICER = 7,
    CF_EFFECT_TENSOR_DISPLACEMENT = 8,
    CF_EFFECT_OPTICAL_FLOW_TIME_WARP = 9,
    CF_EFFECT_CHRONO_FEEDBACK = 10,
    CF_EFFECT_STRUCTURAL_DATAMOSH = 11,
} CFEffectKind;

// Generic ABI-stable parameter packet. Its interpretation is defined by kind:
// transpose:              options = X(0) / Y(1), Native(0) / Fit source canvas(1)
// luma-time shift:        values[0] = multiplier, options = source, edge
// radial time loom:       values = centerX, centerY, intensity, twist; options = edge, topology
// temporal pixel sort:    values[0] = threshold; options = criterion, direction
// tensor 3D rotation:     values = XY, XT, YT degrees; options[0] = fill mode
// spectral transform:     values[0] = angle; options = axis/plane, normalize, Native/Fit Source Size, Swap/Rotate
// selective prefilter:    options = spatial strength, temporal strength (Off/Light/Strong)
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

// Renders one effect that consumes both a source tensor and a driver tensor.
// Currently accepted kinds are DIMENSIONAL_SPLICER and TENSOR_DISPLACEMENT.
int32_t cf_render_cross_tensor_effect(
    const float* source,
    uint64_t source_frames,
    uint64_t source_height,
    uint64_t source_width,
    uint64_t source_channels,
    uint32_t frame_rate_numerator,
    uint32_t frame_rate_denominator,
    const float* driver,
    uint64_t driver_frames,
    uint64_t driver_height,
    uint64_t driver_width,
    uint64_t driver_channels,
    CFEffectDescriptor effect,
    uint64_t max_working_set_bytes,
    CFVideoBuffer** output,
    char* error_message,
    uint64_t error_message_capacity);

// Processes a headerless linear-float T,H,W,C tensor through memory-mapped
// cache files. Local and temporal effects stay out-of-core; FFT operates one
// frequency line at a time and uses max_working_set_bytes only for line buffers.
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

int32_t cf_render_file_cross_tensor_effect(
    const char* source_path,
    CFFileTensorInfo source_info,
    const char* driver_path,
    CFFileTensorInfo driver_info,
    const char* output_path,
    CFEffectDescriptor effect,
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
