// mww.cc - see mww.h. The feature frontend is Google's microfrontend (the
// esp-micro-speech-features component) with the settings every microWakeWord
// model was trained with; the network runs on TensorFlow Lite Micro.
#include "mww.h"

#include <algorithm>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "frontend.h"
#include "frontend_util.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char *TAG = "mww";

namespace {

constexpr int FEATURES = 40;                 // log-mel bands per slice
constexpr int WINDOW_MS = 30;
constexpr int SAMPLE_RATE = 16000;
constexpr int SETTLE_SLICES = 74;            // ignore this many slices after a reset
constexpr int VAR_ARENA = 1024;              // the network's own state between slices
constexpr int MAX_WINDOW = 16;

struct FrontendState frontend;
bool frontend_ready;
const tflite::Model *model;
tflite::MicroMutableOpResolver<20> *resolver;
tflite::MicroInterpreter *interpreter;
uint8_t *arena;
uint8_t var_arena[VAR_ARENA];
uint8_t cutoff;                              // probability_cutoff * 255
int window_size;
uint8_t recent[MAX_WINDOW];
int recent_at;
int stride_at;
int settle;
float last;                                  // the last window's average, for the capture's fire log

bool frontend_init(int step_ms)
{
    FrontendConfig cfg;
    FrontendFillConfigWithDefaults(&cfg);
    cfg.window.size_ms = WINDOW_MS;
    cfg.window.step_size_ms = step_ms;
    cfg.filterbank.num_channels = FEATURES;
    cfg.filterbank.lower_band_limit = 125.0f;
    cfg.filterbank.upper_band_limit = 7500.0f;
    cfg.noise_reduction.smoothing_bits = 10;
    cfg.noise_reduction.even_smoothing = 0.025f;
    cfg.noise_reduction.odd_smoothing = 0.06f;
    cfg.noise_reduction.min_signal_remaining = 0.05f;
    cfg.pcan_gain_control.enable_pcan = 1;
    cfg.pcan_gain_control.strength = 0.95f;
    cfg.pcan_gain_control.offset = 80.0f;
    cfg.pcan_gain_control.gain_bits = 21;
    cfg.log_scale.enable_log = 1;
    cfg.log_scale.scale_shift = 6;
    frontend_ready = FrontendPopulateState(&cfg, &frontend, SAMPLE_RATE) != 0;
    return frontend_ready;
}

bool network_init(const mww_config_t *c)
{
    model = tflite::GetModel(c->model);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "model schema %d, wanted %d", (int)model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }
    resolver = new tflite::MicroMutableOpResolver<20>();
    resolver->AddCallOnce();
    resolver->AddVarHandle();
    resolver->AddReshape();
    resolver->AddReadVariable();
    resolver->AddStridedSlice();
    resolver->AddConcatenation();
    resolver->AddAssignVariable();
    resolver->AddConv2D();
    resolver->AddMul();
    resolver->AddAdd();
    resolver->AddMean();
    resolver->AddFullyConnected();
    resolver->AddLogistic();
    resolver->AddQuantize();
    resolver->AddDepthwiseConv2D();
    resolver->AddAveragePool2D();
    resolver->AddMaxPool2D();
    resolver->AddPad();
    resolver->AddPack();
    resolver->AddSplitV();
    arena = (uint8_t *)heap_caps_malloc(c->tensor_arena_size, MALLOC_CAP_SPIRAM);   // as ESPHome does
    if (!arena) return false;
    tflite::MicroAllocator *va = tflite::MicroAllocator::Create(var_arena, VAR_ARENA);
    tflite::MicroResourceVariables *vars = tflite::MicroResourceVariables::Create(va, 20);
    interpreter = new tflite::MicroInterpreter(model, *resolver, arena, c->tensor_arena_size, vars);
    if (interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "the model does not fit its arena of %d", c->tensor_arena_size);
        return false;
    }
    TfLiteTensor *in = interpreter->input(0), *out = interpreter->output(0);
    if (in->dims->size != 3 || in->dims->data[0] != 1 || in->dims->data[2] != FEATURES || in->type != kTfLiteInt8
        || out->dims->size != 2 || out->dims->data[1] != 1 || out->type != kTfLiteUInt8) {
        ESP_LOGE(TAG, "not a microWakeWord version 2 model");
        return false;
    }
    ESP_LOGI(TAG, "model takes %d slices a step, arena %d", (int)in->dims->data[1], c->tensor_arena_size);
    return true;
}

// One 30 ms slice of features, as the model was trained to see them.
bool slice(const uint16_t *values)
{
    TfLiteTensor *in = interpreter->input(0);
    int stride = in->dims->data[1];
    int8_t *row = in->data.int8 + FEATURES * stride_at;
    for (int i = 0; i < FEATURES; i++) {
        int32_t v = ((int32_t)values[i] * 256 + 333) / 666 - 128;     // 666 = 25.6 * 26
        row[i] = (int8_t)std::clamp<int32_t>(v, INT8_MIN, INT8_MAX);
    }
    if (++stride_at < stride) return false;
    stride_at = 0;
    if (interpreter->Invoke() != kTfLiteOk) return false;
    recent[recent_at] = interpreter->output(0)->data.uint8[0];
    recent_at = (recent_at + 1) % window_size;
    if (settle > 0) { settle--; return false; }
    int sum = 0;
    for (int i = 0; i < window_size; i++) sum += recent[i];
    last = sum / (255.0f * window_size);
    return sum > (int)cutoff * window_size;
}

}  // namespace

bool mww_init(const mww_config_t *c)
{
    window_size = std::clamp(c->sliding_window_size, 1, MAX_WINDOW);
    cutoff = (uint8_t)(c->probability_cutoff * 255);
    if (!frontend_init(c->feature_step_ms) || !network_init(c)) return false;
    mww_reset();
    return true;
}

void mww_reset(void)
{
    memset(recent, 0, sizeof recent);
    recent_at = 0;
    settle = SETTLE_SLICES;
    if (interpreter) interpreter->Reset();
}

float mww_last(void) { return last; }

bool mww_feed(const int16_t *pcm, size_t samples)
{
    if (!interpreter || !frontend_ready) return false;
    bool heard = false;
    while (samples > 0) {
        size_t used = 0;
        FrontendOutput out = FrontendProcessSamples(&frontend, pcm, samples, &used);
        if (used == 0) break;
        pcm += used;
        samples -= used;
        if (out.values && out.size == FEATURES && slice(out.values)) heard = true;
    }
    if (heard) mww_reset();
    return heard;
}
