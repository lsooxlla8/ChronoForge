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
    CF_EFFECT_SEAMLESS_LOOP = 12,
    CF_EFFECT_RGB_TIME_SLIP = 13,
    CF_EFFECT_HORIZONTAL_SYNC_LOSS = 14,
    CF_EFFECT_CHROMA_CARRIER_DRIFT = 15,
    CF_EFFECT_STRIDE_ERROR = 16,
    CF_EFFECT_BLOCK_ADDRESS_CORRUPTION = 17,
    CF_EFFECT_BITPLANE_FORGE = 18,
    CF_EFFECT_SIGNAL_WEAVE = 19,
    CF_EFFECT_BLOCK_GRAFT = 20,
} CFEffectKind;

enum { CF_EFFECT_DESCRIPTOR_VERSION = 2, CF_EFFECT_PARAMETER_CAPACITY = 8 };

// Versioned internal parameter packet. Its interpretation is defined by kind:
// transpose:              options = X(0) / Y(1), Native(0) / Fit source canvas(1)
// luma-time shift:        values[0] = multiplier, options = source, edge
// radial time loom:       values = centerX, centerY, intensity, twist; options = edge, topology
// temporal pixel sort:    values[0] = threshold; options = criterion, direction
// tensor 3D rotation:     values = XY, XT, YT degrees; options[0] = fill mode
// spectral transform:     values[0] = angle; options = axis/plane, normalize, Native/Fit Source Size, Swap/Rotate
// selective prefilter:    options = spatial strength, temporal strength (Off/Light/Strong)
// seamless loop:          values = transition frames, weave softness; options[0] = Crossfade/Luma Weave/Ping-Pong
// RGB time slip:          values = R/G/B frame offsets, spatial split; options = split axis, edge
// horizontal sync loss:   values = shift fraction, band height, drift speed, tear density; options = driver, edge
// chroma carrier drift:    values = X/Y/time offsets, bleed; options = mode, edge
// stride error:            values = stride delta, base offset, temporal drift; options = channel mode, address edge
// block corruption:        values = block size, corruption, time reach, hold; options = mapping, edge
// bitplane forge:           values = working bits, plane mask, shift; options = operation, channel
// signal weave:             values = band size, phase drift, irregularity, B time offset; options = pattern, size matching
// block graft:              values = block size, density/threshold, hold, B time offset; options = trigger, size matching
typedef struct CFEffectDescriptorV2 {
    int32_t kind;
    uint32_t descriptor_version;
    float amount;
    uint64_t random_seed;
    uint32_t value_count;
    uint32_t option_count;
    float values[CF_EFFECT_PARAMETER_CAPACITY];
    int32_t options[CF_EFFECT_PARAMETER_CAPACITY];
} CFEffectDescriptorV2;

CFEffectDescriptorV2 cf_effect_descriptor_v2_make(
    int32_t kind,
    float amount,
    uint64_t random_seed,
    const float* values,
    uint32_t value_count,
    const int32_t* options,
    uint32_t option_count);

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
    const CFEffectDescriptorV2* effects,
    uint64_t effect_count,
    uint64_t max_working_set_bytes,
    CFVideoBuffer** output,
    char* error_message,
    uint64_t error_message_capacity);

// Renders one effect that consumes both a source tensor and a driver tensor.
// Accepted kinds are DIMENSIONAL_SPLICER, TENSOR_DISPLACEMENT, SIGNAL_WEAVE and BLOCK_GRAFT.
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
    CFEffectDescriptorV2 effect,
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
    const CFEffectDescriptorV2* effects,
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
    CFEffectDescriptorV2 effect,
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
