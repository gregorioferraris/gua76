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
#define PEAK_METER_DECAY_MS 1000.0f // Tempo di decadimento per i peak meter (slower release)

// Valori min/max per i parametri (mapping da 0.0-1.0 float a valori reali)
// Questi sono indicativi, da calibrare per il feeling del 1176
#define INPUT_GAIN_DB_MIN   -12.0f
#define INPUT_GAIN_DB_MAX    12.0f
#define OUTPUT_GAIN_DB_MIN  -12.0f
#define OUTPUT_GAIN_DB_MAX   12.0f

// Tempi Attack/Release del 1176 sono inversi (valore più basso sulla manopola = più veloce)
// E non sono lineari, ma qui li mappiamo su un range 0.0-1.0 per semplicità
#define ATTACK_TIME_US_FASTEST   20.0f   // 20 microseconds
#define ATTACK_TIME_US_SLOWEST   800.0f  // 800 microseconds

#define RELEASE_TIME_MS_FASTEST  50.0f   // 50 milliseconds
#define RELEASE_TIME_MS_SLOWEST  1100.0f // 1100 milliseconds (1.1 seconds)

// Range per il controllo Drive/Saturation
#define DRIVE_SATURATION_AMOUNT_MIN 0.0f // Nessuna saturazione aggiuntiva
#define DRIVE_SATURATION_AMOUNT_MAX 2.0f // Saturazione massima

// Ratios per 1176: 4:1, 8:1, 12:1, 20:1, All-Button (che è "quasi" un 20:1 ma con un comportamento unico)
static const float RATIO_VALUES[] = { 4.0f, 8.0f, 12.0f, 20.0f, 20.0f /* All-Button uses 20:1 effectively but with different curves */ };
// Threshold è tipicamente fisso in un 1176, lo impostiamo a un valore interno
#define COMPRESSOR_THRESHOLD_DB -20.0f // Fissato internamente

#define PAD_10DB_VALUE db_to_linear(-10.0f) // Valore lineare del pad -10dB

// --- OVERSEMPLING/UPSAMPLING ---
#define UPSAMPLE_FACTOR 8 // Fattore di oversampling (8x per qualità professionale)
// Useremo 3 filtri biquad in cascata per l'upsampling e il downsampling,
// per ottenere un filtro passa-basso di 6° ordine (36 dB/ottava).
#define NUM_BIQUADS_FOR_OS_FILTER 3 // 3 biquad -> 6° ordine (36 dB/ottava)
#define OS_FILTER_Q 0.707f // Q di Butterworth per risposta piatta


// --- SIDECHAIN FILTERS ---
#define NUM_BIQUADS_FOR_SIDECHAIN_FILTER 3 // Per 36dB/ottava

// --- Funzioni di Utilità Generali ---

static float to_db(float linear_val) {
    if (linear_val <= 0.00000000001f) return -90.0f; // Prevent log(0) for very small values
    return 20.0f * log10f(linear_val);
}

static float db_to_linear(float db_val) {
    return powf(10.0f, db_val / 20.0f);
}

// Funzione di soft-clipping/saturazione inspirata a un compressore FET
// Aggiunge la "punchiness" e la saturazione tipica.
// Il 'drive_amount' influisce sulla quantità di saturazione.
static float apply_soft_clip(float sample, float drive_amount) {
    float sign = (sample >= 0) ? 1.0f : -1.0f;
    float abs_sample = fabsf(sample);

    // Scaling dell'input per aumentare l'effetto con drive
    abs_sample *= (1.0f + drive_amount * 0.5f); // Scala l'input basato sul drive

    // Saturazione sigmoide. Questa funzione crea armoniche e un soft-knee.
    // Puoi sperimentare diverse curve, es. tanh, arctan, o polinomiali.
    // Questa è una semplice curva cubica che introduce la 3a armonica principale.
    float saturated_sample = abs_sample - (abs_sample * abs_sample * abs_sample) * (drive_amount * 0.1f);

    // Un leggero hard clipping finale per sicurezza o per emulare il limitatore dell'1176.
    return sign * fminf(fmaxf(saturated_sample, -1.0f), 1.0f);
}


// Funzione per calcolare il picco assoluto e applicare il decadimento (per i peak meter)
static float calculate_peak_level(const float* buffer, uint32_t n_samples, float current_peak_linear, float decay_alpha) {
    float max_abs_val = 0.0f;
    for (uint32_t i = 0; i < n_samples; ++i) {
        float abs_sample = fabsf(buffer[i]);
        if (abs_sample > max_abs_val) {
            max_abs_val = abs_sample;
        }
    }
    // Combined peak (new peak or decaying old peak)
    // Se il nuovo picco è maggiore, lo prendiamo. Altrimenti, decadiamo il vecchio.
    // Questo è un picco con "hold" e decadimento, tipico dei meter analogici.
    return fmaxf(max_abs_val, current_peak_linear * (1.0f - decay_alpha));
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
    float cos_omega = cosf(omega);
    float sin_omega = sinf(omega);
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
    f->a0 = 1.0f; // Questo non viene usato nel process, è solo per chiarezza, il denominatore è 1.0
}


// Struct del plugin
typedef struct {
    // Puntatori ai parametri di controllo (Input)
    float* input_ptr;
    float* output_ptr;
    float* attack_ptr;
    float* release_ptr;
    float* ratio_ptr;
    float* meter_mode_ptr;
    float* bypass_ptr;
    float* drive_saturation_ptr;
    float* oversampling_ptr;
    float* sidechain_hpf_on_ptr;
    float* sidechain_hpf_freq_ptr;
    float* sidechain_hpf_q_ptr; // Nuovo
    float* sidechain_lpf_on_ptr;
    float* sidechain_lpf_freq_ptr;
    float* sidechain_listen_ptr;
    float* midside_mode_ptr; // Nuovo
    float* midside_link_ptr; // Nuovo
    float* pad_10db_ptr; // Nuovo

    // Puntatori per i meter (Output del plugin, input per la GUI)
    float* peak_gr_ptr;
    float* peak_in_l_ptr;
    float* peak_in_r_ptr;
    float* peak_out_l_ptr;
    float* peak_out_r_ptr;

    // Puntatori ai buffer audio
    const float* audio_in_l_ptr;
    const float* audio_in_r_ptr;
    float* audio_out_l_ptr;
    float* audio_out_r_ptr;

    // Nuovi puntatori ai buffer sidechain esterni
    const float* sidechain_in_l_ptr;
    const float* sidechain_in_r_ptr;


    // Variabili di stato del plugin
    double samplerate;
    double oversampled_samplerate;
    LV2_Log_Log* log;
    LV2_Log_Logger logger;

    // Variabili di stato del compressore (per canale)
    float envelope_l; // Detector envelope per Left/Mid
    float envelope_r; // Detector envelope per Right/Side
    float current_gr_linear_l; // Current gain reduction for Left/Mid (linear)
    float current_gr_linear_r; // Current gain reduction for Right/Side (linear)
    float peak_in_linear_l; // Current peak input for L (linear)
    float peak_in_linear_r; // Current peak input for R (linear)
    float peak_out_linear_l; // Current peak output for L (linear)
    float peak_out_linear_r; // Current peak output for R (linear)


    // Variabili per smoothing dei meter
    float gr_meter_alpha;
    float output_meter_alpha;
    float peak_meter_decay_alpha; // Per il decadimento dei picchi

    // Buffer per oversampling (per blocco di input completo)
    float* oversample_buffer_l;
    float* oversample_buffer_r;
    float* oversample_sidechain_l;
    float* oversample_sidechain_r;
    uint32_t max_oversample_buffer_size; // Max block size * OS_FACTOR

    // Filtri per upsampling/downsampling (Biquad di 6° Ordine)
    BiquadFilter upsample_lp_filters_l[NUM_BIQUADS_FOR_OS_FILTER];
    BiquadFilter upsample_lp_filters_r[NUM_BIQUADS_FOR_OS_FILTER];
    BiquadFilter downsample_lp_filters_l[NUM_BIQUADS_FOR_OS_FILTER];
    BiquadFilter downsample_lp_filters_r[NUM_BIQUADS_FOR_OS_FILTER];

    // Filtri sidechain (per canale, 6° ordine: 3 biquad in cascata)
    BiquadFilter sc_hpf_filters_l[NUM_BIQUADS_FOR_SIDECHAIN_FILTER];
    BiquadFilter sc_lpf_filters_l[NUM_BIQUADS_FOR_SIDECHAIN_FILTER];
    BiquadFilter sc_hpf_filters_r[NUM_BIQUADS_FOR_SIDECHAIN_FILTER];
    BiquadFilter sc_lpf_filters_r[NUM_BIQUADS_FOR_SIDECHAIN_FILTER];

} Gua76;

// Funzione di istanziazione del plugin
static LV2_Handle
instantiate(const LV2_Descriptor* descriptor,
            double              samplerate,
            const char* bundle_path,
            const LV2_Feature* const* features) {
    Gua76* self = (Gua76*)calloc(1, sizeof(Gua76));
    if (!self) return NULL;

    self->samplerate = samplerate;
    self->oversampled_samplerate = samplerate * UPSAMPLE_FACTOR;

    for (int i = 0; features[i]; ++i) {
        if (!strcmp(features[i]->URI, LV2_LOG__log)) {
            self->log = (LV2_Log_Log*)features[i]->data;
        }
    }
    lv2_log_logger_init(&self->logger, NULL, self->log);

    // Inizializzazione variabili di stato del compressore
    self->envelope_l = 0.0f;
    self->envelope_r = 0.0f;
    self->current_gr_linear_l = 1.0f; // Inizia senza gain reduction (0dB)
    self->current_gr_linear_r = 1.0f;
    self->peak_in_linear_l = db_to_linear(-90.0f); // Inizializza i meter a -90dB
    self->peak_in_linear_r = db_to_linear(-90.0f);
    self->peak_out_linear_l = db_to_linear(-90.0f);
    self->peak_out_linear_r = db_to_linear(-90.0f);


    // Calcolo coefficienti di smoothing per i meter
    self->gr_meter_alpha = 1.0f - expf(-1.0f / (self->samplerate * (GR_METER_SMOOTH_MS / 1000.0f)));
    self->output_meter_alpha = 1.0f - expf(-1.0f / (self->samplerate * (OUTPUT_METER_SMOOTH_MS / 1000.0f)));
    self->peak_meter_decay_alpha = 1.0f - expf(-1.0f / (self->samplerate * (PEAK_METER_DECAY_MS / 1000.0f)));

    // Inizializzazione filtri biquad per oversampling/downsampling e sidechain
    for(int i = 0; i < NUM_BIQUADS_FOR_OS_FILTER; ++i) { // Per i filtri OS (6° ordine)
        biquad_init(&self->upsample_lp_filters_l[i]);
        biquad_init(&self->upsample_lp_filters_r[i]);
        biquad_init(&self->downsample_lp_filters_l[i]);
        biquad_init(&self->downsample_lp_filters_r[i]);
    }
    for(int i = 0; i < NUM_BIQUADS_FOR_SIDECHAIN_FILTER; ++i) { // Per i filtri sidechain (6° ordine)
        biquad_init(&self->sc_hpf_filters_l[i]);
        biquad_init(&self->sc_lpf_filters_l[i]);
        biquad_init(&self->sc_hpf_filters_r[i]);
        biquad_init(&self->sc_lpf_filters_r[i]);
    }


    // Alloca buffer per oversampling (max block size * OS_FACTOR)
    // LV2 hosts possono passare sample_count fino a 4096 o più, quindi dimensioniamo di conseguenza
    self->max_oversample_buffer_size = 4096 * UPSAMPLE_FACTOR;
    self->oversample_buffer_l = (float*)calloc(self->max_oversample_buffer_size, sizeof(float));
    self->oversample_buffer_r = (float*)calloc(self->max_oversample_buffer_size, sizeof(float));
    self->oversample_sidechain_l = (float*)calloc(self->max_oversample_buffer_size, sizeof(float));
    self->oversample_sidechain_r = (float*)calloc(self->max_oversample_buffer_size, sizeof(float));


    if (!self->oversample_buffer_l || !self->oversample_buffer_r || !self->oversample_sidechain_l || !self->oversample_sidechain_r) {
        free(self->oversample_buffer_l);
        free(self->oversample_buffer_r);
        free(self->oversample_sidechain_l);
        free(self->oversample_sidechain_r);
        free(self);
        return NULL;
    }

    return (LV2_Handle)self;
}

// Funzione per connettere le porte
static void
connect_port(LV2_Handle instance, uint32_t port, void* data_location) {
    Gua76* self = (Gua76*)instance;

    switch ((Gua76PortIndex)port) {
        case GUA76_AUDIO_IN_L:          self->audio_in_l_ptr = (const float*)data_location; break;
        case GUA76_AUDIO_IN_R:          self->audio_in_r_ptr = (const float*)data_location; break;
        case GUA76_AUDIO_OUT_L:         self->audio_out_l_ptr = (float*)data_location; break;
        case GUA76_AUDIO_OUT_R:         self->audio_out_r_ptr = (float*)data_location; break;

        case GUA76_SIDECHAIN_IN_L:      self->sidechain_in_l_ptr = (const float*)data_location; break;
        case GUA76_SIDECHAIN_IN_R:      self->sidechain_in_r_ptr = (const float*)data_location; break;

        case GUA76_INPUT:               self->input_ptr = (float*)data_location; break;
        case GUA76_OUTPUT:              self->output_ptr = (float*)data_location; break;
        case GUA76_ATTACK:              self->attack_ptr = (float*)data_location; break;
        case GUA76_RELEASE:             self->release_ptr = (float*)data_location; break;
        case GUA76_RATIO:               self->ratio_ptr = (float*)data_location; break;
        case GUA76_METER_MODE:          self->meter_mode_ptr = (float*)data_location; break;
        case GUA76_BYPASS:              self->bypass_ptr = (float*)data_location; break;
        case GUA76_DRIVE_SATURATION:    self->drive_saturation_ptr = (float*)data_location; break;
        case GUA76_OVERSAMPLING:        self->oversampling_ptr = (float*)data_location; break;
        case GUA76_SIDECHAIN_HPF_ON:    self->sidechain_hpf_on_ptr = (float*)data_location; break;
        case GUA76_SIDECHAIN_HPF_FREQ:  self->sidechain_hpf_freq_ptr = (float*)data_location; break;
        case GUA77_SIDECHAIN_HPF_Q:     self->sidechain_hpf_q_ptr = (float*)data_location; break; // Nuovo
        case GUA76_SIDECHAIN_LPF_ON:    self->sidechain_lpf_on_ptr = (float*)data_location; break;
        case GUA76_SIDECHAIN_LPF_FREQ:  self->sidechain_lpf_freq_ptr = (float*)data_location; break;
        case GUA76_SIDECHAIN_LISTEN:    self->sidechain_listen_ptr = (float*)data_location; break;
        case GUA76_MIDSIDE_MODE:        self->midside_mode_ptr = (float*)data_location; break; // Nuovo
        case GUA76_MIDSIDE_LINK:        self->midside_link_ptr = (float*)data_location; break; // Nuovo
        case GUA76_PAD_10DB:            self->pad_10db_ptr = (float*)data_location; break;     // Nuovo

        case GUA76_PEAK_GR:             self->peak_gr_ptr = (float*)data_location; break;
        case GUA76_PEAK_IN_L:           self->peak_in_l_ptr = (float*)data_location; break;
        case GUA76_PEAK_IN_R:           self->peak_in_r_ptr = (float*)data_location; break;
        case GUA76_PEAK_OUT_L:          self->peak_out_l_ptr = (float*)data_location; break;
        case GUA76_PEAK_OUT_R:          self->peak_out_r_ptr = (float*)data_location; break;
    }
}

// Funzione di attivazione (resettare lo stato del plugin)
static void
activate(LV2_Handle instance) {
    Gua76* self = (Gua76*)instance;
    self->envelope_l = 0.0f;
    self->envelope_r = 0.0f;
    self->current_gr_linear_l = 1.0f;
    self->current_gr_linear_r = 1.0f;
    self->peak_in_linear_l = db_to_linear(-90.0f);
    self->peak_in_linear_r = db_to_linear(-90.0f);
    self->peak_out_linear_l = db_to_linear(-90.0f);
    self->peak_out_linear_r = db_to_linear(-90.0f);


    *self->peak_gr_ptr = 0.0f;
    *self->peak_in_l_ptr = -90.0f;
    *self->peak_in_r_ptr = -90.0f;
    *self->peak_out_l_ptr = -90.0f;
    *self->peak_out_r_ptr = -90.0f;


    // Reinitalizza stati interni dei filtri biquad (cruciale per prevenire clicks e rumori)
    for(int i = 0; i < NUM_BIQUADS_FOR_OS_FILTER; ++i) { // Per i filtri OS
        biquad_init(&self->upsample_lp_filters_l[i]);
        biquad_init(&self->upsample_lp_filters_r[i]);
        biquad_init(&self->downsample_lp_filters_l[i]);
        biquad_init(&self->downsample_lp_filters_r[i]);
    }
    for(int i = 0; i < NUM_BIQUADS_FOR_SIDECHAIN_FILTER; ++i) { // Per i filtri sidechain
        biquad_init(&self->sc_hpf_filters_l[i]);
        biquad_init(&self->sc_lpf_filters_l[i]);
        biquad_init(&self->sc_hpf_filters_r[i]);
        biquad_init(&self->sc_lpf_filters_r[i]);
    }

    // Ricalcola i coefficienti dei filtri anti-aliasing (solo una volta all'attivazione)
    // Frequenza di taglio: Nyquist della frequenza originale (samplerate / 2) divisa per il fattore di oversampling
    float os_filter_freq = (float)(self->samplerate / 2.0 / UPSAMPLE_FACTOR);
    for(int i = 0; i < NUM_BIQUADS_FOR_OS_FILTER; ++i) {
        calculate_biquad_coeffs(&self->upsample_lp_filters_l[i], self->oversampled_samplerate, os_filter_freq, OS_FILTER_Q, 0); // LP
        calculate_biquad_coeffs(&self->upsample_lp_filters_r[i], self->oversampled_samplerate, os_filter_freq, OS_FILTER_Q, 0); // LP
        calculate_biquad_coeffs(&self->downsample_lp_filters_l[i], self->oversampled_samplerate, os_filter_freq, OS_FILTER_Q, 0); // LP
        calculate_biquad_coeffs(&self->downsample_lp_filters_r[i], self->oversampled_samplerate, os_filter_freq, OS_FILTER_Q, 0); // LP
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

    // Sidechain input - se connesso, usa quello, altrimenti usa l'input principale
    const float* sc_in_l = self->sidechain_in_l_ptr ? self->sidechain_in_l_ptr : in_l;
    const float* sc_in_r = self->sidechain_in_r_ptr ? self->sidechain_in_r_ptr : in_r;

    // Leggi i valori dei parametri dal host (sono sempre aggiornati)
    const float input_norm = *self->input_ptr;
    const float output_norm = *self->output_ptr;
    const float attack_norm = *self->attack_ptr;   // 0.0=fast, 1.0=slow
    const float release_norm = *self->release_ptr; // 0.0=fast, 1.0=slow
    const int   ratio_enum = (int)*self->ratio_ptr;
    const int   meter_mode_enum = (int)*self->meter_mode_ptr;
    const bool  bypass = (*self->bypass_ptr > 0.5f);
    const float drive_saturation_norm = *self->drive_saturation_ptr;
    const bool  oversampling_on = (*self->oversampling_ptr > 0.5f);
    const bool  sc_hpf_on = (*self->sidechain_hpf_on_ptr > 0.5f);
    const float sc_hpf_freq = *self->sidechain_hpf_freq_ptr;
    const float sc_filter_q = *self->sidechain_hpf_q_ptr; // Nuovo
    const bool  sc_lpf_on = (*self->sidechain_lpf_on_ptr > 0.5f);
    const float sc_lpf_freq = *self->sidechain_lpf_freq_ptr;
    const bool  sidechain_listen = (*self->sidechain_listen_ptr > 0.5f);
    const bool  midside_mode_on = (*self->midside_mode_ptr > 0.5f); // Nuovo
    const bool  midside_link = (*self->midside_link_ptr > 0.5f);   // Nuovo
    const bool  pad_10db_on = (*self->pad_10db_ptr > 0.5f);         // Nuovo


    // --- Calcolo Parametri del Compressore ---
    float input_gain_linear = db_to_linear(input_norm * (INPUT_GAIN_DB_MAX - INPUT_GAIN_DB_MIN) + INPUT_GAIN_DB_MIN);
    const float output_gain_linear = db_to_linear(output_norm * (OUTPUT_GAIN_DB_MAX - OUTPUT_GAIN_DB_MIN) + OUTPUT_GAIN_DB_MIN);
    const float compressor_threshold_linear = db_to_linear(COMPRESSOR_THRESHOLD_DB);
    const float drive_amount = drive_saturation_norm * DRIVE_SATURATION_AMOUNT_MAX;

    if (pad_10db_on) { // Applica il pad prima dell'input gain
        input_gain_linear *= PAD_10DB_VALUE;
    }

    // Mappatura non lineare Attack/Release per il 1176 "feeling"
    // I tempi effettivi sono spesso mappati in modo inverso logaritmico o esponenziale dalla manopola
    // Per un feel più 1176, usiamo una potenza per dare più risoluzione verso i tempi veloci.
    float attack_time_us_mapped = ATTACK_TIME_US_FASTEST + (ATTACK_TIME_US_SLOWEST - ATTACK_TIME_US_FASTEST) * powf(attack_norm, 2.0f);
    float release_time_ms_mapped = RELEASE_TIME_MS_FASTEST + (RELEASE_TIME_MS_SLOWEST - RELEASE_TIME_MS_FASTEST) * powf(release_norm, 2.0f);


    // Ottieni il rapporto di compressione dal selettore
    float current_ratio = RATIO_VALUES[ratio_enum];
    bool is_all_button_mode = (ratio_enum == 4); // Special case for All-Button

    // --- Aggiorna i coefficienti dei filtri sidechain se i parametri cambiano ---
    // Usiamo variabili statiche per tracciare i cambiamenti e ricalcolare solo quando necessario
    static float prev_sc_hpf_freq = -1.0f;
    static float prev_sc_lpf_freq = -1.0f;
    static float prev_sc_filter_q = -1.0f;

    // Calcola i coefficienti dei filtri sidechain (3 biquad in cascata per 6° ordine)
    if (sc_hpf_on && (fabsf(sc_hpf_freq - prev_sc_hpf_freq) > 0.01f || fabsf(sc_filter_q - prev_sc_filter_q) > 0.01f)) {
        for (int k = 0; k < NUM_BIQUADS_FOR_SIDECHAIN_FILTER; ++k) {
            calculate_biquad_coeffs(&self->sc_hpf_filters_l[k], self->samplerate, sc_hpf_freq, sc_filter_q, 1); // HPF
            calculate_biquad_coeffs(&self->sc_hpf_filters_r[k], self->samplerate, sc_hpf_freq, sc_filter_q, 1);
        }
        prev_sc_hpf_freq = sc_hpf_freq;
        prev_sc_filter_q = sc_filter_q;
    }
    if (sc_lpf_on && (fabsf(sc_lpf_freq - prev_sc_lpf_freq) > 0.01f || fabsf(sc_filter_q - prev_sc_filter_q) > 0.01f)) {
        for (int k = 0; k < NUM_BIQUADS_FOR_SIDECHAIN_FILTER; ++k) {
            calculate_biquad_coeffs(&self->sc_lpf_filters_l[k], self->samplerate, sc_lpf_freq, sc_filter_q, 0); // LPF
            calculate_biquad_coeffs(&self->sc_lpf_filters_r[k], self->samplerate, sc_lpf_freq, sc_filter_q, 0);
        }
        prev_sc_lpf_freq = sc_lpf_freq;
        prev_sc_filter_q = sc_filter_q;
    }


    // --- Logica True Bypass ---
    if (bypass) {
        if (in_l != out_l) { memcpy(out_l, in_l, sizeof(float) * sample_count); }
        if (in_r != out_r) { memcpy(out_r, in_r, sizeof(float) * sample_count); }
        // Aggiorna meter in bypass per un visuale realistico (mostrano input)
        *self->peak_gr_ptr = 0.0f; // No GR
        self->peak_in_linear_l = calculate_peak_level(in_l, sample_count, self->peak_in_linear_l, self->peak_meter_decay_alpha);
        self->peak_in_linear_r = calculate_peak_level(in_r, sample_count, self->peak_in_linear_r, self->peak_meter_decay_alpha);
        self->peak_out_linear_l = self->peak_in_linear_l; // Output = Input in bypass
        self->peak_out_linear_r = self->peak_in_linear_r;

        *self->peak_in_l_ptr = to_db(self->peak_in_linear_l);
        *self->peak_in_r_ptr = to_db(self->peak_in_linear_r);
        *self->peak_out_l_ptr = to_db(self->peak_out_linear_l);
        *self->peak_out_r_ptr = to_db(self->peak_out_linear_r);
        return;
    }

    // --- Mid-Side Encoding (se attivo) ---
    float temp_in_l[sample_count];
    float temp_in_r[sample_count];
    float temp_sc_l[sample_count];
    float temp_sc_r[sample_count];

    if (midside_mode_on) {
        for (uint32_t i = 0; i < sample_count; ++i) {
            temp_in_l[i] = (in_l[i] + in_r[i]) * 0.5f; // Mid
            temp_in_r[i] = (in_l[i] - in_r[i]) * 0.5f; // Side
            temp_sc_l[i] = (sc_in_l[i] + sc_in_r[i]) * 0.5f; // Mid Sidechain
            temp_sc_r[i] = (sc_in_l[i] - sc_in_r[i]) * 0.5f; // Side Sidechain
        }
        in_l = temp_in_l;
        in_r = temp_in_r;
        sc_in_l = temp_sc_l;
        sc_in_r = temp_sc_r;
    }


    // --- Loop di elaborazione per blocco di campioni ---
    // Gestione dell'oversampling: dobbiamo elaborare il blocco completo
    // I passaggi: Upsample input -> Filter -> Process (OS) -> Filter -> Downsample output
    uint32_t current_oversample_buffer_size = sample_count * UPSAMPLE_FACTOR;

    // Assicurati che i buffer siano sufficientemente grandi
    if (current_oversample_buffer_size > self->max_oversample_buffer_size) {
        lv2_log_warning(&self->logger, "Oversample buffer too small! Skipping oversampling.\n");
        // Non dovremmo mai arrivare qui con una allocazione dinamica corretta
        return;
    }

    // Copia e Upsample con interpolazione semplice (in una vera implementazione sarebbe un interpolatore più sofisticato)
    for (uint32_t i = 0; i < sample_count; ++i) {
        for (uint32_t j = 0; j < UPSAMPLE_FACTOR; ++j) {
            float alpha = (float)j / UPSAMPLE_FACTOR;
            // Interpolazione lineare per oversampling
            self->oversample_buffer_l[i * UPSAMPLE_FACTOR + j] = in_l[i] * (1.0f - alpha) + (i + 1 < sample_count ? in_l[i+1] : in_l[i]) * alpha;
            self->oversample_buffer_r[i * UPSAMPLE_FACTOR + j] = in_r[i] * (1.0f - alpha) + (i + 1 < sample_count ? in_r[i+1] : in_r[i]) * alpha;
            self->oversample_sidechain_l[i * UPSAMPLE_FACTOR + j] = sc_in_l[i] * (1.0f - alpha) + (i + 1 < sample_count ? sc_in_l[i+1] : sc_in_l[i]) * alpha;
            self->oversample_sidechain_r[i * UPSAMPLE_FACTOR + j] = sc_in_r[i] * (1.0f - alpha) + (i + 1 < sample_count ? sc_in_r[i+1] : sc_in_r[i]) * alpha;
        }
    }


    // Loop a sample rate di oversampling
    for (uint32_t i = 0; i < current_oversample_buffer_size; ++i) {
        float current_sample_l = self->oversample_buffer_l[i];
        float current_sample_r = self->oversample_buffer_r[i];
        float current_sc_l = self->oversample_sidechain_l[i];
        float current_sc_r = self->oversample_sidechain_r[i];

        // --- Oversampling Stage 2: Filtri Anti-Aliasing (Low-Pass) ---
        if (oversampling_on) {
            for (int k = 0; k < NUM_BIQUADS_FOR_OS_FILTER; ++k) {
                current_sample_l = biquad_process(&self->upsample_lp_filters_l[k], current_sample_l);
                current_sample_r = biquad_process(&self->upsample_lp_filters_r[k], current_sample_r);
            }
        }

        // --- Sidechain Processing (a Oversampled Rate per maggiore accuratezza) ---
        float processed_sc_l = current_sc_l;
        float processed_sc_r = current_sc_r;

        if (sc_hpf_on) {
            for (int k = 0; k < NUM_BIQUADS_FOR_SIDECHAIN_FILTER; ++k) {
                processed_sc_l = biquad_process(&self->sc_hpf_filters_l[k], processed_sc_l);
                processed_sc_r = biquad_process(&self->sc_hpf_filters_r[k], processed_sc_r);
            }
        }
        if (sc_lpf_on) {
            for (int k = 0; k < NUM_BIQUADS_FOR_SIDECHAIN_FILTER; ++k) {
                processed_sc_l = biquad_process(&self->sc_lpf_filters_l[k], processed_sc_l);
                processed_sc_r = biquad_process(&self->sc_lpf_filters_r[k], processed_sc_r);
            }
        }

        // --- Envelope Detector (Peak Detector, ispirato 1176 con non linearità) ---
        // L'1176 è un peak detector, con tempi di attacco e rilascio che dipendono dal segnale.
        // Più alto il segnale, più veloce il tempo effettivo.
        float current_abs_l_sc = fabsf(processed_sc_l);
        float current_abs_r_sc = fabsf(processed_sc_r);

        // Attack/Release alphas dipendenti dall'ampiezza per la non linearità dell'1176
        // Se il segnale è molto forte, l'attacco e il rilascio sono più rapidi
        float dynamic_attack_alpha_l = 1.0f - expf(-1.0f / (self->oversampled_samplerate * (attack_time_us_mapped / 1000000.0f * (1.0f + 0.5f * fminf(1.0f, current_abs_l_sc * 2.0f)))));
        float dynamic_release_alpha_l = 1.0f - expf(-1.0f / (self->oversampled_samplerate * (release_time_ms_mapped / 1000.0f * (1.0f + 0.5f * fminf(1.0f, self->envelope_l * 0.5f)))));
        float dynamic_attack_alpha_r = 1.0f - expf(-1.0f / (self->oversampled_samplerate * (attack_time_us_mapped / 1000000.0f * (1.0f + 0.5f * fminf(1.0f, current_abs_r_sc * 2.0f)))));
        float dynamic_release_alpha_r = 1.0f - expf(-1.0f / (self->oversampled_samplerate * (release_time_ms_mapped / 1000.0f * (1.0f + 0.5f * fminf(1.0f, self->envelope_r * 0.5f)))));

        // Envelope update
        if (current_abs_l_sc > self->envelope_l) {
            self->envelope_l = (self->envelope_l * (1.0f - dynamic_attack_alpha_l)) + (current_abs_l_sc * dynamic_attack_alpha_l);
        } else {
            self->envelope_l = (self->envelope_l * (1.0f - dynamic_release_alpha_l)) + (current_abs_l_sc * dynamic_release_alpha_l);
        }
        if (current_abs_r_sc > self->envelope_r) {
            self->envelope_r = (self->envelope_r * (1.0f - dynamic_attack_alpha_r)) + (current_abs_r_sc * dynamic_attack_alpha_r);
        } else {
            self->envelope_r = (self->envelope_r * (1.0f - dynamic_release_alpha_r)) + (current_abs_r_sc * dynamic_release_alpha_r);
        }

        // --- Gain Computer ---
        float gain_reduction_linear_l = 1.0f;
        float gain_reduction_linear_r = 1.0f;

        float detector_envelope_l = self->envelope_l;
        float detector_envelope_r = self->envelope_r;

        if (midside_mode_on && midside_link) {
            // Se Mid-Side e Link attivo, il detector usa il massimo tra M e S
            detector_envelope_l = fmaxf(self->envelope_l, self->envelope_r);
            detector_envelope_r = detector_envelope_l; // Linka il detector anche per Side
        }


        if (is_all_button_mode) {
            // "All-Button" Mode: Aggressive, higher ratio, often a "knee" that dips below 0dB GR
            // e una compressione più aggressiva e un leggero aumento della distorsione.
            if (detector_envelope_l > compressor_threshold_linear) {
                float over_threshold = detector_envelope_l - compressor_threshold_linear;
                float compressed_envelope = compressor_threshold_linear + (over_threshold / (current_ratio * 1.5f)); // Ratio più alto
                gain_reduction_linear_l = compressed_envelope / detector_envelope_l;
            }
            if (detector_envelope_r > compressor_threshold_linear) { // Anche se linkato, calcolo per r (sarà uguale a l)
                float over_threshold = detector_envelope_r - compressor_threshold_linear;
                float compressed_envelope = compressor_threshold_linear + (over_threshold / (current_ratio * 1.5f));
                gain_reduction_linear_r = compressed_envelope / detector_envelope_r;
            }
            // Aggiungi un po' di distorsione armonica aggiuntiva in All-Button mode
            current_sample_l = apply_soft_clip(current_sample_l, drive_amount + 0.2f); // Più drive
            current_sample_r = apply_soft_clip(current_sample_r, drive_amount + 0.2f);
        } else {
            // Canale Left/Mid
            if (detector_envelope_l > compressor_threshold_linear) {
                float over_threshold = detector_envelope_l - compressor_threshold_linear;
                float compressed_envelope = compressor_threshold_linear + (over_threshold / current_ratio);
                gain_reduction_linear_l = compressed_envelope / detector_envelope_l;
            }
            // Canale Right/Side
            if (detector_envelope_r > compressor_threshold_linear) {
                float over_threshold = detector_envelope_r - compressor_threshold_linear;
                float compressed_envelope = compressor_threshold_linear + (over_threshold / current_ratio);
                gain_reduction_linear_r = compressed_envelope / detector_envelope_r;
            }
        }

        // Smooth la Gain Reduction per evitare zippering
        self->current_gr_linear_l = (self->current_gr_linear_l * (1.0f - dynamic_attack_alpha_l)) + (gain_reduction_linear_l * dynamic_attack_alpha_l);
        self->current_gr_linear_r = (self->current_gr_linear_r * (1.0f - dynamic_attack_alpha_r)) + (gain_reduction_linear_r * dynamic_attack_alpha_r);


        // --- Applicazione del Gain e Output ---
        // Applica l'input gain, la gain reduction, e l'output gain
        float final_l = current_sample_l * input_gain_linear * self->current_gr_linear_l * output_gain_linear;
        float final_r = current_sample_r * input_gain_linear * self->current_gr_linear_r * output_gain_linear;

        // Applica il soft clipping/saturazione finale (per il "carattere" 1176)
        final_l = apply_soft_clip(final_l, drive_amount);
        final_r = apply_soft_clip(final_r, drive_amount);

        // Se Sidechain Listen è attivo, dirotta il segnale sidechain processato all'output
        if (sidechain_listen) {
            final_l = processed_sc_l;
            final_r = processed_sc_r;
        }

        self->oversample_buffer_l[i] = final_l;
        self->oversample_buffer_r[i] = final_r;
    } // Fine loop per-oversampled sample


    // --- Oversampling Stage 3: Filtro Anti-Aliasing (Low-Pass) e Downsample ---
    for (uint32_t i = 0; i < sample_count; ++i) {
        float downsampled_l = self->oversample_buffer_l[i * UPSAMPLE_FACTOR];
        float downsampled_r = self->oversample_buffer_r[i * UPSAMPLE_FACTOR];

        if (oversampling_on) {
            for (int k = 0; k < NUM_BIQUADS_FOR_OS_FILTER; ++k) {
                downsampled_l = biquad_process(&self->downsample_lp_filters_l[k], downsampled_l);
                downsampled_r = biquad_process(&self->downsample_lp_filters_r[k], downsampled_r);
            }
        }
        out_l[i] = downsampled_l;
        out_r[i] = downsampled_r;
    }

    // --- Mid-Side Decoding (se attivo) ---
    if (midside_mode_on) {
        for (uint32_t i = 0; i < sample_count; ++i) {
            float mid = out_l[i];
            float side = out_r[i];
            out_l[i] = mid + side;
            out_r[i] = mid - side;
        }
    }


    // --- Aggiornamento dei Meter (a fine blocco) ---
    // GR Meter (prende il massimo della GR tra L/Mid e R/Side, in dB)
    float max_gr = fmaxf(self->current_gr_linear_l, self->current_gr_linear_r);
    *self->peak_gr_ptr = to_db(max_gr); // GR è mostrata come valore negativo (es. -6dB)

    // Input/Output Peak Meters (utilizzano la funzione calculate_peak_level)
    self->peak_in_linear_l = calculate_peak_level(in_l, sample_count, self->peak_in_linear_l, self->peak_meter_decay_alpha);
    self->peak_in_linear_r = calculate_peak_level(in_r, sample_count, self->peak_in_linear_r, self->peak_meter_decay_alpha);
    self->peak_out_linear_l = calculate_peak_level(out_l, sample_count, self->peak_out_linear_l, self->peak_meter_decay_alpha);
    self->peak_out_linear_r = calculate_peak_level(out_r, sample_count, self->peak_out_linear_r, self->peak_meter_decay_alpha);

    // Scrivi i valori dei meter ai puntatori di output per la GUI
    *self->peak_in_l_ptr = to_db(self->peak_in_linear_l);
    *self->peak_in_r_ptr = to_db(self->peak_in_linear_r);
    *self->peak_out_l_ptr = to_db(self->peak_out_linear_l);
    *self->peak_out_r_ptr = to_db(self->peak_out_linear_r);

    // Il meter mode dal parametro controlla quale valore la GUI mostrerà, non il plugin
    // Quindi il plugin invia sempre tutti i valori di picco.
}

// Funzione di pulizia (liberare memoria)
static void
cleanup(LV2_Handle instance) {
    Gua76* self = (Gua76*)instance;
    free(self->oversample_buffer_l);
    free(self->oversample_buffer_r);
    free(self->oversample_sidechain_l);
    free(self->oversample_sidechain_r);
    free(self);
}

// Funzione per restituire interfacce (come l'idle interface)
static const void*
extension_data(const char* uri) {
    return NULL;
}

// La funzione deactivate è necessaria per LV2, anche se vuota
static void deactivate(LV2_Handle instance) {
    (void)instance;
}

// Descrittore del plugin LV2
static const LV2_Descriptor descriptor = {
    GUA76_URI,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate, // Necessario per LV2, anche se vuoto
    cleanup,
    extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor(uint32_t index) {
    if (index == 0) return &descriptor;
    return NULL;
}
