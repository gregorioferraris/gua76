#include "gua76.h"
#include <lv2/core/lv2.h>
#include <lv2/log/logger.h>
#include <lv2/log/log.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// --- Costanti e Definizioni ---
#define M_PI_F 3.14159265358979323846f

// --- Limiter/Compressor Parameters ---
#define GR_METER_SMOOTH_MS 10.0f // Tempo in ms per smoothing del gain reduction meter
#define OUTPUT_METER_SMOOTH_MS 50.0f // Tempo in ms per smoothing del RMS output meter

// --- OVERSEMPLING/UPSAMPLING ---
#define UPSAMPLE_FACTOR 4 // Fattore di oversampling (2x, 4x, 8x, etc.)
// Useremo 3 filtri biquad in cascata per l'upsampling e il downsampling,
// per ottenere un filtro passa-basso di 6° ordine (36 dB/ottava).
#define NUM_BIQUADS_FOR_OS_FILTER 3 
#define OS_FILTER_Q 0.707f // Q di Butterworth per risposta piatta (o calibra per più risonanza)


// --- Funzioni di Utilità Generali ---

static float to_db(float linear_val) {
    if (linear_val <= 0.00000000001f) return -90.0f; // Prevent log(0)
    return 20.0f * log10f(linear_val);
}

static float db_to_linear(float db_val) {
    return powf(10.0f, db_val / 20.0f);
}

// Funzione di soft-clipping ispirata a un compressore FET
// Aggiunge la "punchiness" e la saturazione tipica.
static float apply_soft_clip(float sample, float drive, float threshold_linear, float ratio) {
    float sign = (sample >= 0) ? 1.0f : -1.0f;
    float abs_sample = fabsf(sample);

    float compressed_sample;
    if (abs_sample <= threshold_linear) {
        compressed_sample = abs_sample; // Nessuna compressione sotto la soglia
    } else {
        // Soft-knee come nel 1176, con una transizione più o meno graduale.
        // La formula è semplificata ma cattura l'essenza: la pendenza cambia.
        // Qui 'drive' agisce anche sulla "durezza" del clip oltre che sulla gain.
        float over_threshold = abs_sample - threshold_linear;
        compressed_sample = threshold_linear + over_threshold / ratio;
        
        // Aggiungiamo un po' di saturazione sigmoidale per il carattere FET
        // Questa parte è empirica e dà il sapore "gritty"
        float saturation_amount = drive * 0.1f; // Controllo dalla manopola "drive"
        compressed_sample += saturation_amount * (compressed_sample - compressed_sample * fabsf(compressed_sample));
    }
    return sign * fminf(compressed_sample, 1.0f); // Clip a 0dBFS per sicurezza
}

// Funzione per calcolare l'RMS per il meter di output
static float calculate_rms_level(const float* buffer, uint32_t n_samples, float current_rms, float alpha) {
    if (n_samples == 0) return current_rms;
    float sum_sq = 0.0f;
    for (uint32_t i = 0; i < n_samples; ++i) {
        sum_sq += buffer[i] * buffer[i];
    }
    float block_rms_linear = sqrtf(sum_sq / n_samples);
    return (current_rms * (1.0f - alpha)) + (block_rms_linear * alpha);
}


// --- Strutture e Funzioni per Filtri Biquad ---

typedef struct {
    float a0, a1, a2, b0, b1, b2; // Coefficienti
    float z1, z2;                 // Stati precedenti
} BiquadFilter;

static void biquad_init(BiquadFilter* f) {
    f->a0 = f->a1 = f->a2 = f->b0 = f->b1 = f->b2 = 0.0f;
    f->z1 = f->z2 = 0.0f;
}

static float biquad_process(BiquadFilter* f, float in) {
    float out = in * f->b0 + f->z1;
    f->z1 = in * f->b1 + f->z2 - f->a1 * out;
    f->z2 = in * f->b2 - f->a2 * out;
    return out;
}

// Calcola i coefficienti per un filtro biquad (Low Pass o High Pass)
// freq_hz: frequenza di taglio
// q_val: fattore di qualità (risonanza)
// type: 0 per Low Pass, 1 per High Pass
static void calculate_biquad_coeffs(BiquadFilter* f, double samplerate, float freq_hz, float q_val, int type) {
    if (freq_hz <= 0.0f) freq_hz = 1.0f; // Evita divisione per zero o log(0)
    if (q_val <= 0.0f) q_val = 0.1f;    // Evita divisione per zero o Q troppo basso

    float omega = 2.0f * M_PI_F * freq_hz / samplerate;
    float sin_omega = sinf(omega);
    float cos_omega = cosf(omega);
    float alpha = sin_omega / (2.0f * q_val); // Q del filtro

    float b0, b1, b2, a0, a1, a2;

    if (type == 0) { // Low Pass Filter
        b0 = (1.0f - cos_omega) / 2.0f;
        b1 = 1.0f - cos_omega;
        b2 = (1.0f - cos_omega) / 2.0f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cos_omega;
        a2 = 1.0f - alpha;
    } else { // High Pass Filter
        b0 = (1.0f + cos_omega) / 2.0f;
        b1 = -(1.0f + cos_omega);
        b2 = (1.0f + cos_omega) / 2.0f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cos_omega;
        a2 = 1.0f - alpha;
    }

    // Normalizza i coefficienti per a0
    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;
    f->a1 = a1 / a0;
    f->a2 = a2 / a0;
    f->a0 = 1.0f; // Questo non viene usato nel process, è solo per chiarezza
}


// Struct del plugin
typedef struct {
    // Puntatori ai parametri di controllo
    float* input_gain_ptr;
    float* output_gain_ptr;
    float* attack_ptr;
    float* release_ptr;
    float* ratio_ptr;
    float* all_buttons_ptr; // Simulated "All Buttons In"
    float* bypass_ptr;

    // Puntatori per i meter (output del plugin, input per la GUI)
    float* gr_meter_ptr;
    float* output_meter_ptr;

    // Puntatori ai buffer audio
    const float* audio_in_l_ptr;
    const float* audio_in_r_ptr;
    float* audio_out_l_ptr;
    float* audio_out_r_ptr;

    // Variabili di stato del plugin
    double samplerate;
    double oversampled_samplerate; // Nuovo
    LV2_Log_Log* log;
    LV2_Log_Logger logger;

    // Variabili di stato del compressore
    float envelope_l; // Detector envelope per Left
    float envelope_r; // Detector envelope per Right
    float current_gr_l; // Current gain reduction for Left (linear)
    float current_gr_r; // Current gain reduction for Right (linear)

    // Variabili per smoothing dei meter
    float gr_meter_alpha;
    float output_meter_alpha;

    // Buffer per oversampling (per blocco di input completo) // Nuovo
    float* oversample_buffer_l; 
    float* oversample_buffer_r; 
    uint32_t oversample_buffer_size; 

    // Filtri per upsampling/downsampling (Biquad di 6° Ordine) // Nuovo
    BiquadFilter upsample_lp_filters_l[NUM_BIQUADS_FOR_OS_FILTER];
    BiquadFilter upsample_lp_filters_r[NUM_BIQUADS_FOR_OS_FILTER];
    BiquadFilter downsample_lp_filters_l[NUM_BIQUADS_FOR_OS_FILTER];
    BiquadFilter downsample_lp_filters_r[NUM_BIQUADS_FOR_OS_FILTER];

} Gua76;

// Funzione di istanziazione del plugin
static LV2_Handle
instantiate(const LV2_Descriptor* descriptor,
            double                    samplerate,
            const char* bundle_path,
            const LV2_Feature* const* features) {
    Gua76* self = (Gua76*)calloc(1, sizeof(Gua76));
    if (!self) return NULL;

    self->samplerate = samplerate;
    self->oversampled_samplerate = samplerate * UPSAMPLE_FACTOR; // Nuovo

    for (int i = 0; features[i]; ++i) {
        if (!strcmp(features[i]->URI, LV2_LOG__log)) {
            self->log = (LV2_Log_Log*)features[i]->data;
        }
    }
    lv2_log_logger_init(&self->logger, NULL, self->log);

    // Inizializzazione variabili di stato
    self->envelope_l = 0.0f;
    self->envelope_r = 0.0f;
    self->current_gr_l = 1.0f; // Start with no gain reduction
    self->current_gr_r = 1.0f;

    self->gr_meter_alpha = 1.0f - expf(-1.0f / (self->samplerate * (GR_METER_SMOOTH_MS / 1000.0f)));
    self->output_meter_alpha = 1.0f - expf(-1.0f / (self->samplerate * (OUTPUT_METER_SMOOTH_MS / 1000.0f)));

    // Inizializzazione filtri biquad per oversampling/downsampling // Nuovo
    for(int i = 0; i < NUM_BIQUADS_FOR_OS_FILTER; ++i) {
        biquad_init(&self->upsample_lp_filters_l[i]);
        biquad_init(&self->upsample_lp_filters_r[i]);
        biquad_init(&self->downsample_lp_filters_l[i]);
        biquad_init(&self->downsample_lp_filters_r[i]);
    }

    // Alloca buffer per oversampling // Nuovo
    self->oversample_buffer_size = 1024 * UPSAMPLE_FACTOR; // Max block size * OS_FACTOR
    self->oversample_buffer_l = (float*)calloc(self->oversample_buffer_size, sizeof(float));
    self->oversample_buffer_r = (float*)calloc(self->oversample_buffer_size, sizeof(float));

    if (!self->oversample_buffer_l || !self->oversample_buffer_r) {
        free(self->oversample_buffer_l);
        free(self->oversample_buffer_r);
        free(self);
        return NULL;
    }

    return (LV2_Handle)self;
}

// Funzione per connettere le porte
static void
connect_port(LV2_Handle instance, uint32_t port, void* data_location) {
    Gua76* self = (Gua76*)instance;

    switch ((GUA76_PortIndex)port) {
        case GUA76_INPUT_GAIN:       self->input_gain_ptr = (float*)data_location; break;
        case GUA76_OUTPUT_GAIN:      self->output_gain_ptr = (float*)data_location; break;
        case GUA76_ATTACK:           self->attack_ptr = (float*)data_location; break;
        case GUA76_RELEASE:          self->release_ptr = (float*)data_location; break;
        case GUA76_RATIO:            self->ratio_ptr = (float*)data_location; break;
        case GUA76_ALL_BUTTONS:      self->all_buttons_ptr = (float*)data_location; break;
        case GUA76_BYPASS:           self->bypass_ptr = (float*)data_location; break;
        case GUA76_GR_METER:         self->gr_meter_ptr = (float*)data_location; break;
        case GUA76_OUTPUT_METER:     self->output_meter_ptr = (float*)data_location; break;
        case GUA76_AUDIO_IN_L:       self->audio_in_l_ptr = (const float*)data_location; break;
        case GUA76_AUDIO_IN_R:       self->audio_in_r_ptr = (const float*)data_location; break;
        case GUA76_AUDIO_OUT_L:      self->audio_out_l_ptr = (float*)data_location; break;
        case GUA76_AUDIO_OUT_R:      self->audio_out_r_ptr = (float*)data_location; break;
    }
}

// Funzione di attivazione (resettare lo stato del plugin)
static void
activate(LV2_Handle instance) {
    Gua76* self = (Gua76*)instance;
    self->envelope_l = 0.0f;
    self->envelope_r = 0.0f;
    self->current_gr_l = 1.0f;
    self->current_gr_r = 1.0f;
    *self->gr_meter_ptr = 0.0f;
    *self->output_meter_ptr = to_db(db_to_linear(-60.0f)); // Initialize output meter to a low dB value

    // Reinitalizza stati interni dei filtri biquad per oversampling/downsampling
    for(int i = 0; i < NUM_BIQUADS_FOR_OS_FILTER; ++i) {
        biquad_init(&self->upsample_lp_filters_l[i]);
        biquad_init(&self->upsample_lp_filters_r[i]);
        biquad_init(&self->downsample_lp_filters_l[i]);
        biquad_init(&self->downsample_lp_filters_r[i]);
    }
}

// Funzione di elaborazione audio (run)
static void
run(LV2_Handle instance, uint32_t sample_count) {
    Gua76* self = (Gua76*)instance;

    const float* in_l = self->audio_in_l_ptr;
    const float* in_r = self->audio_in_r_ptr;
    float* out_l = self->audio_out_l_ptr;
    float* out_r = self->audio_out_r_ptr;

    const float input_gain_db = *self->input_gain_ptr;
    const float output_gain_db = *self->output_gain_ptr;
    const float attack_ms = *self->attack_ptr;
    const float release_ms = *self->release_ptr;
    const float ratio_mode = *self->ratio_ptr;
    const float all_buttons = *self->all_buttons_ptr;
    const float bypass = *self->bypass_ptr;

    // --- Logica True Bypass ---
    if (bypass > 0.5f) {
        if (in_l != out_l) { memcpy(out_l, in_l, sizeof(float) * sample_count); }
        if (in_r != out_r) { memcpy(out_r, in_r, sizeof(float) * sample_count); }
        // Update meters in bypass for a realistic visual
        *self->gr_meter_ptr = 0.0f;
        *self->output_meter_ptr = to_db(calculate_rms_level(in_l, sample_count, db_to_linear(*self->output_meter_ptr), self->output_meter_alpha));
        return;
    }

    // --- Calcolo Parametri del Compressore ---
    const float input_gain_linear = db_to_linear(input_gain_db);
    const float output_gain_linear = db_to_linear(output_gain_db);

    // Modulazione Attack/Release per emulare il comportamento non lineare dell'1176
    // Più il segnale è forte, più veloci sono i tempi.
    // Queste sono delle costanti da calibrare per trovare il "feeling" giusto.
    // I valori qui sono indic
