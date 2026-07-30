/*
 * main.cpp
 *
 * Forest Acoustic Classifier — ESP32-S3
 *
 * Continuous inference loop:
 *   boot → load model → init mic & spectrogram engine
 *        → forever: capture 2.5s audio → mel-spectrogram → TFLite inference → print result
 *
 * Classes (index order matches training — 5-class model, tree_falling removed):
 *   0: Axe  1: Chainsaw  2: Gunshot  3: Handsaw  4: Background
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// TFLite Micro
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/schema/schema_generated.h"

// Our headers
#include "model_data.h"
#include "spectrogram_params.h"
#include "audio_capture.h"
#include "spectrogram.h"

static const char *TAG = "main";

// -----------------------------------------------------------------------
// Class labels (order must match training label encoding)
// 5-class model trained with explicit order (train_acoustic_model_5class.ipynb):
//   ['axe', 'chainsaw', 'gunshot', 'handsaw', 'background']
// -----------------------------------------------------------------------
static const char *CLASS_LABELS[] = {
    "Axe",
    "Chainsaw",
    "Gunshot",
    "Handsaw",
    "Background",
};
static const int NUM_CLASSES = 5;

// -----------------------------------------------------------------------
// Tensor arena — put in external SPIRAM if available, else internal DRAM.
// 90 KB is the empirically verified minimum for the tiny model.
// -----------------------------------------------------------------------
#define TENSOR_ARENA_SIZE (270 * 1024)
static uint8_t tensor_arena[TENSOR_ARENA_SIZE];

extern "C" void app_main(void)
{
    printf("\n========================================\n");
    printf("  Forest Acoustic Classifier — ESP32-S3\n");
    printf("========================================\n");

    // ----------------------------------------------------------------
    // 1. Load and verify TFLite model
    // ----------------------------------------------------------------
    const tflite::Model *model = tflite::GetModel(model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Model schema mismatch! Got %" PRIu32 ", expected %d",
                 model->version(), TFLITE_SCHEMA_VERSION);
        return;
    }
    printf("[OK] Model loaded (%u bytes)\n", (unsigned)model_data_len);

    // ----------------------------------------------------------------
    // 2. Register operators used by the tiny model
    //    Conv2D, DepthwiseConv2D, MaxPool2D, FullyConnected, Softmax,
    //    Reshape, Mul, Add, Mean (GlobalAveragePooling2D), Quantize,
    //    Dequantize, BatchNorm (fused into Conv)
    // ----------------------------------------------------------------
    static tflite::MicroMutableOpResolver<12> resolver;
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddMaxPool2D();
    resolver.AddFullyConnected();
    resolver.AddSoftmax();
    resolver.AddReshape();
    resolver.AddMul();
    resolver.AddAdd();
    resolver.AddMean();          // GlobalAveragePooling2D
    resolver.AddQuantize();
    resolver.AddDequantize();
    resolver.AddLogistic();      // safety — some fused activations

    // ----------------------------------------------------------------
    // 3. Create interpreter and allocate tensors
    // ----------------------------------------------------------------
    static tflite::MicroInterpreter interpreter(
        model, resolver, tensor_arena, TENSOR_ARENA_SIZE);

    TfLiteStatus alloc_status = interpreter.AllocateTensors();
    if (alloc_status != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors() failed");
        return;
    }
    printf("[OK] Tensors allocated\n");

    // Print tensor info for debugging
    TfLiteTensor *input  = interpreter.input(0);
    TfLiteTensor *output = interpreter.output(0);
    printf("  Input:  type=%d, shape=[%d,%d,%d,%d], scale=%.6f, zp=%" PRId32 "\n",
           input->type,
           input->dims->data[0], input->dims->data[1],
           input->dims->data[2], input->dims->data[3],
           input->params.scale, input->params.zero_point);
    printf("  Output: type=%d, shape=[%d,%d], scale=%.6f, zp=%" PRId32 "\n",
           output->type,
           output->dims->data[0], output->dims->data[1],
           output->params.scale, (int32_t)output->params.zero_point);
    printf("  Arena:  %u / %u bytes used\n",
           (unsigned)interpreter.arena_used_bytes(),
           (unsigned)TENSOR_ARENA_SIZE);

    // ----------------------------------------------------------------
    // 4. Initialize hardware: microphone + spectrogram engine
    // ----------------------------------------------------------------
    printf("[...] Initializing I2S microphone (GPIO BCK=17, WS=15, DIN=16)...\n");
    if (audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio_init() failed");
        return;
    }
    printf("[OK] I2S microphone initialized\n");

    if (spectrogram_init() != ESP_OK) {
        ESP_LOGE(TAG, "spectrogram_init() failed — not enough heap");
        return;
    }
    printf("[OK] Spectrogram engine initialized\n");

    printf("\n========================================\n");
    printf("  Ready — capturing audio\n");
    printf("========================================\n\n");

    // ----------------------------------------------------------------
    // 5. Inference loop — runs forever
    // ----------------------------------------------------------------
    int8_t *input_data = input->data.int8;

    while (true) {
        printf("--- Capturing %.1fs + spectrogram...\n", (float)NUM_AUDIO_SAMPLES / SAMPLE_RATE);

        esp_err_t ret = spectrogram_compute(input_data);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "spectrogram_compute() failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        printf("[OK] Spectrogram computed\n");

        printf("[...] Running inference...\n");
        TfLiteStatus invoke_status = interpreter.Invoke();
        if (invoke_status != kTfLiteOk) {
            ESP_LOGE(TAG, "Invoke() failed");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        printf("[OK] Inference complete\n");

        // Dequantize output INT8 → float probabilities
        float probs[NUM_CLASSES];
        float out_scale = output->params.scale;
        int   out_zp    = output->params.zero_point;
        for (int i = 0; i < NUM_CLASSES; i++) {
            probs[i] = (output->data.int8[i] - out_zp) * out_scale;
        }

        // Find argmax
        int best_idx = 0;
        for (int i = 1; i < NUM_CLASSES; i++) {
            if (probs[i] > probs[best_idx]) best_idx = i;
        }

        // Print prediction
        printf("--- Prediction ---\n");
        printf("-----testing-------------\n");
        printf("  Class: %s (index %d)\n", CLASS_LABELS[best_idx], best_idx);
        printf("  Confidence: %.4f\n", probs[best_idx]);
        printf("  Raw logits: ");
        for (int i = 0; i < NUM_CLASSES; i++) {
            printf("%s: %.4f  ", CLASS_LABELS[i], probs[i]);
        }
        printf("\n--- Waiting 1 second before next capture ---\n\n");

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
