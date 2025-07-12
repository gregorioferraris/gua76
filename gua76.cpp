#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>
#include <lv2/log/logger.h>
#include <lv2/log/log.h>
#include <lv2/worker/worker.h> // Se volessi fare calcoli complessi off-thread

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Plugin URI (deve corrispondere a gua76.ttl)
#define GUA76_URI "http://moddevices.com/plugins/mod-devel/gua76"

// Enum delle porte (DEVONO corrispondere agli indici in gua76.ttl)
typedef enum {
    GUA76_INPUT_GAIN = 0,
    GUA76_OUTPUT_GAIN = 1,
    GUA76_INPUT_PAD_10DB = 2,
    GUA76_BYPASS = 3,
    GUA76_NORMALIZE_OUTPUT = 4,
    GUA76_MS_MODE_ACTIVE = 5,
    GUA76_EXTERNAL_SC_ACTIVE = 6,
    GUA76_ATTACK = 7,
    GUA76_RELEASE = 8,
    GUA76_RATIO = 9,
    GUA76_INPUT_CLIP_DRIVE = 10,
    GUA76_OUTPUT_CLIP_DRIVE = 11,
    GUA76_SC_HPF_FREQ = 12,
    GUA76_SC_LPF_FREQ = 13,
    GUA76_SC_HPF_Q = 14, // NEW
    GUA76_SC_LPF_Q = 15, // NEW
    GUA76_METER_DISPLAY_MODE = 16, // NEW
    GUA76_GAIN_REDUCTION_METER = 17, // NEW (Output for GUI)
    GUA76_INPUT_RMS = 18, // NEW (Output for GUI)
    GUA76_OUTPUT_RMS = 19, // NEW (Output for GUI)
    GUA76_AUDIO_IN_L = 20,
    GUA76_AUDIO_IN_R = 21,
    GUA76_AUDIO_OUT_L = 22,
    GUA76_AUDIO_OUT_R = 23,
    GUA76_AUDIO_SIDECHAIN_IN_L = 24,
    GUA76_AUDIO_SIDECHAIN_IN_R = 25
} GUA76_PortIndex;

// Struttura del plugin (Instance)
typedef struct {
    float* gain_in_ptr;
    float* gain_out_ptr;
    float* input_pad_ptr;
    float* bypass_ptr;
    float* normalize_output_ptr;
    float* ms_mode_active_ptr;
    float* external_sc_active_ptr;
    float* attack_ptr;
    float* release_ptr;
    float* ratio_ptr;
    float* input_clip_drive_ptr;
    float* output_clip_drive_ptr;
    float* sc_hpf_freq_ptr;
    float* sc_lpf_freq_ptr;
    float* sc_hpf_q_ptr; // NEW
    float* sc_lpf_q_ptr; // NEW
    float* meter_display_mode_ptr; // NEW

    float* gain_reduction_meter_ptr; // NEW (Output)
    float* input_rms_ptr;           // NEW (Output)
    float* output_rms_ptr;          // NEW (Output)

    const float* audio_in_l_ptr;
    const float* audio_in_r_ptr;
    float* audio_out_l_ptr;
    float* audio_out_r_ptr;
    const float* audio_sidechain_in_l_ptr;
    const float* audio_sidechain_in_r_ptr;

    double samplerate;

    LV2_URID_Map* map;
    LV2_URID      atom_float;
    LV2_URID      midi_event;
    LV2_Log_Log* log;
    LV2_Log_Logger logger;

    // TODO: Aggiungi qui le variabili di stato per il tuo algoritmo di compressione
    // es: envelope detector state, gain computer state, filter state, etc.
    float current_gain_reduction; // Per calcolare e inviare al meter GR
    float current_input_rms;      // Per calcolare e inviare al meter Input RMS
    float current_output_rms;     // Per calcolare e inviare al meter Output RMS

    // Smoothing per i parametri (evita click/zip)
    float smoothed_input_gain;
    float smoothed_output_gain;
    float smoothed_attack;
    float smoothed_release;
    // ... e così via per tutti i parametri che possono cambiare fluidamente
    // I parametri a scatti (ratio, attack/release_stepped) non necessitano di smoothing per il loro valore base,
    // ma la transizione della compressione che ne deriva sì.

    // Variabili per il calcolo RMS
    float rms_alpha; // Costante di tempo per smoothing RMS
    float rms_sum_sq_in_l;
    float rms_sum_sq_in_r;
    float rms_sum_sq_out_l;
    float rms_sum_sq_out_r;


} Gua76;

// Funzione di utilità per il calcolo RMS (molto semplificata per ora)
static float calculate_rms_level(const float* buffer, uint32_t n_samples, float current_rms, float alpha) {
    float sum_sq = 0.0f;
    for (uint32_t i = 0; i < n_samples; ++i) {
        sum_sq += buffer[i] * buffer[i];
    }
    float block_rms = sqrtf(sum_sq / n_samples);

    // Exponential smoothing per un valore più stabile
    return (current_rms * (1.0f - alpha)) + (block_rms * alpha);
}

// Converti livello lineare a dB
static float to_db(float linear_val) {
    if (linear_val <= 0.000000001f) return -90.0f; // Protezione per log(0)
    return 20.0f * log10f(linear_val);
}

// Funzione di allocazione e inizializzazione del plugin
static LV2_Handle
instantiate(const LV2_Descriptor* descriptor,
            double                    samplerate,
            const char* bundle_path,
            const LV2_Feature* const* features) {
    Gua76* self = (Gua76*)calloc(1, sizeof(Gua76));
    if (!self) return NULL;

    self->samplerate = samplerate;

    // Cerca le feature necessarie
    for (int i = 0; features[i]; ++i) {
        if (!strcmp(features[i]->URI, LV2_URID__map)) {
            self->map = (LV2_URID_Map*)features[i]->data;
        } else if (!strcmp(features[i]->URI, LV2_LOG__log)) {
            self->log = (LV2_Log_Log*)features[i]->data;
        }
    }

    if (!self->map) {
        fprintf(stderr, "gua76: Host does not support urid:map\n");
        free(self);
        return NULL;
    }
    lv2_log_logger_init(&self->logger, self->map, self->log);

    // Mappa gli URI necessari
    self->atom_float = self->map->map(self->map->handle, LV2_ATOM__Float);
    self->midi_event = self->map->map(self->map->handle, LV2_MIDI__MidiEvent);

    // Inizializza i valori smoothed ai valori di default
    self->smoothed_input_gain = 0.0f;
    self->smoothed_output_gain = 0.0f;
    self->smoothed_attack = 0.000020f;
    self->smoothed_release = 0.2f;

    self->current_gain_reduction = 0.0f;
    self->current_input_rms = -60.0f; // Default basso
    self->current_output_rms = -60.0f; // Default basso

    // RMS smoothing constant (adjust as needed for meter responsiveness)
    self->rms_alpha = 1.0f - expf(-1.0f / (self->samplerate * 0.05f)); // 50ms time constant for meter

    return (LV2_Handle)self;
}

// Funzione per collegare le porte
static void
connect_port(LV2_Handle instance, uint32_t port, void* data_location) {
    Gua76* self = (Gua76*)instance;

    switch ((GUA76_PortIndex)port) {
        case GUA76_INPUT_GAIN:               self->gain_in_ptr = (float*)data_location; break;
        case GUA76_OUTPUT_GAIN:              self->gain_out_ptr = (float*)data_location; break;
        case GUA76_INPUT_PAD_10DB:           self->input_pad_ptr = (float*)data_location; break;
        case GUA76_BYPASS:                   self->bypass_ptr = (float*)data_location; break;
        case GUA76_NORMALIZE_OUTPUT:         self->normalize_output_ptr = (float*)data_location; break;
        case GUA76_MS_MODE_ACTIVE:           self->ms_mode_active_ptr = (float*)data_location; break;
        case GUA76_EXTERNAL_SC_ACTIVE:       self->external_sc_active_ptr = (float*)data_location; break;
        case GUA76_ATTACK:                   self->attack_ptr = (float*)data_location; break;
        case GUA76_RELEASE:                  self->release_ptr = (float*)data_location; break;
        case GUA76_RATIO:                    self->ratio_ptr = (float*)data_location; break;
        case GUA76_INPUT_CLIP_DRIVE:         self->input_clip_drive_ptr = (float*)data_location; break;
        case GUA76_OUTPUT_CLIP_DRIVE:        self->output_clip_drive_ptr = (float*)data_location; break;
        case GUA76_SC_HPF_FREQ:              self->sc_hpf_freq_ptr = (float*)data_location; break;
        case GUA76_SC_LPF_FREQ:              self->sc_lpf_freq_ptr = (float*)data_location; break;
        case GUA76_SC_HPF_Q:                 self->sc_hpf_q_ptr = (float*)data_location; break; // NEW
        case GUA76_SC_LPF_Q:                 self->sc_lpf_q_ptr = (float*)data_location; break; // NEW
        case GUA76_METER_DISPLAY_MODE:       self->meter_display_mode_ptr = (float*)data_location; break; // NEW
        case GUA76_GAIN_REDUCTION_METER:     self->gain_reduction_meter_ptr = (float*)data_location; break; // NEW (Output)
        case GUA76_INPUT_RMS:                self->input_rms_ptr = (float*)data_location; break; // NEW (Output)
        case GUA76_OUTPUT_RMS:               self->output_rms_ptr = (float*)data_location; break; // NEW (Output)
        case GUA76_AUDIO_IN_L:               self->audio_in_l_ptr = (const float*)data_location; break;
        case GUA76_AUDIO_IN_R:               self->audio_in_r_ptr = (const float*)data_location; break;
        case GUA76_AUDIO_OUT_L:              self->audio_out_l_ptr = (float*)data_location; break;
        case GUA76_AUDIO_OUT_R:              self->audio_out_r_ptr = (float*)data_location; break;
        case GUA76_AUDIO_SIDECHAIN_IN_L:     self->audio_sidechain_in_l_ptr = (const float*)data_location; break;
        case GUA76_AUDIO_SIDECHAIN_IN_R:     self->audio_sidechain_in_r_ptr = (const float*)data_location; break;
    }
}

// Funzione di ripristino (Reset)
static void
activate(LV2_Handle instance) {
    Gua76* self = (Gua76*)instance;

    // Resetta lo stato interno del compressore, detector, filtri, etc.
    // Inizializza i valori smoothed ai valori attuali all'attivazione
    self->smoothed_input_gain = *self->gain_in_ptr;
    self->smoothed_output_gain = *self->gain_out_ptr;
    self->smoothed_attack = *self->attack_ptr;
    self->smoothed_release = *self->release_ptr;

    self->current_gain_reduction = 0.0f;
    self->current_input_rms = -60.0f;
    self->current_output_rms = -60.0f;

    self->rms_sum_sq_in_l = 0.0f;
    self->rms_sum_sq_in_r = 0.0f;
    self->rms_sum_sq_out_l = 0.0f;
    self->rms_sum_sq_out_r = 0.0f;
}

// Funzione principale di elaborazione del segnale
static void
run(LV2_Handle instance, uint32_t sample_count) {
    Gua76* self = (Gua76*)instance;

    // Puntatori ai buffer audio
    const float* in_l = self->audio_in_l_ptr;
    const float* in_r = self->audio_in_r_ptr;
    float* out_l = self->audio_out_l_ptr;
    float* out_r = self->audio_out_r_ptr;

    // Puntatori ai parametri di controllo (letti dal host)
    const float input_gain = *self->gain_in_ptr;
    const float output_gain = *self->gain_out_ptr;
    const float input_pad_10db = *self->input_pad_10db_ptr;
    const float bypass = *self->bypass_ptr;
    const float normalize_output = *self->normalize_output_ptr;
    const float ms_mode_active = *self->ms_mode_active_ptr;
    const float external_sc_active = *self->external_sc_active_ptr;
    const float attack = *self->attack_ptr;
    const float release = *self->release_ptr;
    const float ratio = *self->ratio_ptr;
    const float input_clip_drive = *self->input_clip_drive_ptr;
    const float output_clip_drive = *self->output_clip_drive_ptr;
    const float sc_hpf_freq = *self->sc_hpf_freq_ptr;
    const float sc_lpf_freq = *self->sc_lpf_freq_ptr;
    const float sc_hpf_q = *self->sc_hpf_q_ptr; // NEW
    const float sc_lpf_q = *self->sc_lpf_q_ptr; // NEW
    const float meter_display_mode = *self->meter_display_mode_ptr; // NEW

    // --- Smoothing dei parametri ---
    // Questi valori interpolati vengono usati nell'elaborazione per evitare artefatti
    const float k_smooth = 0.01f; // Valore di smoothing (da regolare)
    self->smoothed_input_gain = (1.0f - k_smooth) * self->smoothed_input_gain + k_smooth * input_gain;
    self->smoothed_output_gain = (1.0f - k_smooth) * self->smoothed_output_gain + k_smooth * output_gain;
    self->smoothed_attack = (1.0f - k_smooth) * self->smoothed_attack + k_smooth * attack;
    self->smoothed_release = (1.0f - k_smooth) * self->smoothed_release + k_smooth * release;
    // Applica smoothing anche agli altri parametri che lo richiedono (es. drive, filtri)

    // --- Implementazione del bypass ---
    if (bypass > 0.5f) { // Se il bypass è attivo
        if (in_l != out_l) { memcpy(out_l, in_l, sizeof(float) * sample_count); }
        if (in_r != out_r) { memcpy(out_r, in_r, sizeof(float) * sample_count); }

        // Manda valori di meter azzerati o di passthrough quando in bypass
        *self->gain_reduction_meter_ptr = 0.0f;
        *self->input_rms_ptr = to_db(calculate_rms_level(in_l, sample_count, self->current_input_rms, self->rms_alpha));
        *self->output_rms_ptr = to_db(calculate_rms_level(out_l, sample_count, self->current_output_rms, self->rms_alpha));
        self->current_input_rms = *self->input_rms_ptr;
        self->current_output_rms = *self->output_rms_ptr;
        return;
    }

    // --- Implementazione del pad in ingresso ---
    float pad_factor = (input_pad_10db > 0.5f) ? powf(10.0f, -10.0f / 20.0f) : 1.0f;

    // --- Ciclo di elaborazione per i campioni audio ---
    for (uint32_t i = 0; i < sample_count; ++i) {
        float in_l_sample = in_l[i] * pad_factor;
        float in_r_sample = in_r[i] * pad_factor;

        // TODO: Qui va la logica COMPLESSA del compressore 1176:
        // 1. Applica Input Gain (smoothed_input_gain)
        // 2. Calcola il livello RMS per il sidechain (anche con i filtri HPF/LPF/Q)
        //    (se external_sc_active, usa i sidechain_in_L/R)
        // 3. Applica la compressione basata su Attack, Release, Ratio.
        //    Questo è il nucleo dell'algoritmo (detector, gain computer)
        // 4. Applica M/S mode se attivo (converti a M/S, processa, riconverti)
        // 5. Applica Input/Output Clip Drive
        // 6. Applica Output Gain (smoothed_output_gain)
        // 7. Applica Normalize Output se attivo (calcola il picco finale e scala)

        // Esempio molto semplice di passthrough con gain (da sostituire)
        float processed_l = in_l_sample * powf(10.0f, self->smoothed_input_gain / 20.0f);
        float processed_r = in_r_sample * powf(10.0f, self->smoothed_input_gain / 20.0f);

        // TODO: Calcola la gain reduction corrente per questo sample
        // self->current_gain_reduction = ... (da derivare dalla logica del compressore)

        processed_l *= powf(10.0f, self->smoothed_output_gain / 20.0f);
        processed_r *= powf(10.0f, self->smoothed_output_gain / 20.0f);


        // Scrivi l'output
        out_l[i] = processed_l;
        out_r[i] = processed_r;
    }

    // --- Aggiornamento dei valori dei meter per la GUI (a fine blocco) ---
    // La gain reduction è un valore che deve essere calcolato all'interno del tuo algoritmo di compressione.
    // Per ora, useremo un valore placeholder.
    // *self->gain_reduction_meter_ptr = self->current_gain_reduction; // Questo verrà aggiornato dalla logica
    *self->gain_reduction_meter_ptr = -fabsf(sinf( (float)glfwGetTime() / 2.0f ) * 15.0f); // Placeholder animato per test

    // Calcolo RMS per input e output per la GUI
    self->current_input_rms = to_db(calculate_rms_level(in_l, sample_count, self->current_input_rms, self->rms_alpha));
    self->current_output_rms = to_db(calculate_rms_level(out_l, sample_count, self->current_output_rms, self->rms_alpha));

    *self->input_rms_ptr = self->current_input_rms;
    *self->output_rms_ptr = self->current_output_rms;

    // Nota: Il meter_display_mode_ptr è letto solo dalla GUI, non dal core qui.
    // Ma se il tuo core dovesse reagire a questa impostazione, lo farebbe qui.
}

// Funzione di pulizia (deallocazione memoria)
static void
cleanup(LV2_Handle instance) {
    free(instance);
}

// Funzione per restituire le descrizioni del plugin
static const LV2_Extension_Data*
extension_data(const char* uri) {
    return NULL;
}

static const LV2_Descriptor descriptor = {
    GUA76_URI,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate, // Manca la funzione deactivate nel codice, ma è nella struct. Aggiungiamo una dummy.
    cleanup,
    extension_data
};

// Funzione dummy per deactivate (potrebbe non essere necessaria per tutti i plugin)
static void deactivate(LV2_Handle instance) {
    // Non fa nulla per ora. Potresti voler salvare stato, resettare buffer, etc.
}


LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    if (index == 0) {
        return &descriptor;
    }
    return NULL;
}
