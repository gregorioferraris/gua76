#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/log/logger.h>
#include <lv2/uri/uri.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h> // Per printf se necessario per debug

// Definisci l'URI del tuo plugin (deve corrispondere a gua76.ttl)
#define GUA76_URI "http://moddevices.com/plugins/mod-devel/gua76"

// Macro per definire costanti matematiche con precisione float
#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923f
#endif

// Enum per gli indici delle porte (migliora la leggibilità e riduce errori)
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
    GUA76_AUDIO_IN_L = 14,
    GUA76_AUDIO_IN_R = 15,
    GUA76_AUDIO_OUT_L = 16,
    GUA76_AUDIO_OUT_R = 17,
    GUA76_AUDIO_SIDECHAIN_IN_L = 18,
    GUA76_AUDIO_SIDECHAIN_IN_R = 19
} GUA76_PortIndex;

// Funzione di conversione da dB a fattore lineare
static inline float db_to_linear(float db) {
    return powf(10.0f, db / 20.0f);
}

// Funzione di soft clipping (puoi personalizzarla)
static inline float soft_clip(float in, float drive) {
    if (drive < 0.001f) return in; // Evita divisione per zero e calcoli inutili

    // Una semplice funzione di soft-clipping basata su tanh o arctan
    // in = in * drive; // Applica il drive prima del clipping
    // return atan(in) / atan(drive); // Rescaling per mantenere il range
    // Oppure
    float threshold = 0.5f / drive; // Punti in cui inizia la compressione
    if (in > threshold) {
        return threshold + (in - threshold) / (1.0f + fabsf(in - threshold) * 2.0f);
    } else if (in < -threshold) {
        return -threshold + (in + threshold) / (1.0f + fabsf(in + threshold) * 2.0f);
    }
    return in;
}

// Implementazione di un filtro Biquad (HPF/LPF per sidechain)
// Source: https://www.earlevel.com/main/2012/11/26/biquad-c-code/ (Adattato)
typedef struct {
    float a0, a1, a2, b1, b2;
    float z1, z2;
} BiquadFilter;

static void biquad_set(BiquadFilter* filter, int type, float freq, float Q, float sample_rate) {
    float norm;
    float K = tanf(M_PI * freq / sample_rate);

    switch (type) {
        case 0: // LPF
            norm = 1.0f / (1.0f + K / Q + K * K);
            filter->a0 = K * K * norm;
            filter->a1 = 2.0f * filter->a0;
            filter->a2 = filter->a0;
            filter->b1 = 2.0f * (K * K - 1.0f) * norm;
            filter->b2 = (1.0f - K / Q + K * K) * norm;
            break;
        case 1: // HPF
            norm = 1.0f / (1.0f + K / Q + K * K);
            filter->a0 = 1.0f * norm;
            filter->a1 = -2.0f * filter->a0;
            filter->a2 = filter->a0;
            filter->b1 = 2.0f * (K * K - 1.0f) * norm;
            filter->b2 = (1.0f - K / Q + K * K) * norm;
            break;
        // Altri tipi di filtro possono essere aggiunti qui
    }
    filter->z1 = filter->z2 = 0.0f; // Reset stato
}

static inline float biquad_process(BiquadFilter* filter, float in) {
    float out = in * filter->a0 + filter->z1;
    filter->z1 = in * filter->a1 + filter->z2 - filter->b1 * out;
    filter->z2 = in * filter->a2 - filter->b2 * out;
    return out;
}


// Funzione di trasferimento per il modello J-FET (semplificato per envelope)
// Un modello più accurato sarebbe iterativo o basato su tabelle lookup.
// Questo è un approccio semplificato per la rilevazione del gain reduction.
static inline float jfet_transfer(float input_level, float vp, float idss) {
    // Semplificazione: vp e idss determinano la "curva"
    // Qui, input_level è l'envelope filtrata.
    // Il rapporto tra input_level e Vp (pinch-off voltage) determina la non-linearità.
    // L'1176 usa il FET come un resistore a controllo di tensione.
    // Per una compressione, un segnale più alto riduce la "resistenza" del FET,
    // o in questo caso, aumenta l'attenuazione.

    // Questo è un modello base. L'1176 è molto più complesso.
    // Una possibile interpretazione della gain reduction guidata da FET è che
    // all'aumentare del segnale di controllo (envelope), il FET diventa più conduttivo,
    // e questo "spinge" il segnale verso il basso.
    // Per la compressione, vogliamo che un segnale di controllo più grande produca
    // una riduzione di guadagno più grande.
    if (input_level < 0.0f) input_level = 0.0f; // L'envelope è sempre positiva

    // Esempio molto semplificato di relazione non-lineare per gain reduction
    // gain_factor = 1.0f - (input_level * K) dove K dipende da ratio, ecc.
    // L'1176 è feed-back, il che significa che il segnale compresso influenza la sidechain.
    // Per ora, useremo un approccio feed-forward per semplicità.

    // L'uscita del FET è inversamente proporzionale all'ingresso per la GR.
    // Un valore più alto di input_level (envelope) dovrebbe dare un fattore di guadagno più piccolo.
    // Qui è dove l'algoritmo dell'1176 è la sua magia.
    // Senza un modello completo, possiamo simulare una compressione con curva logaritmica.

    // Questo sarà il punto in cui la nostra envelope influenzerà la gain reduction
    // Non restituisce un fattore di guadagno, ma l'input_level che useremo per calcolare la GR.
    return input_level;
}


// Struttura per un singolo canale di compressione
typedef struct {
    float envelope;
    float target_gain;
    BiquadFilter hpf_filter;
    BiquadFilter lpf_filter;
} CompressorChain;

// Funzione di elaborazione del compressore per un singolo canale
static float compressor_process_sample(CompressorChain* chain, float in_sample, float sc_sample,
                                      float attack_time, float release_time, float ratio,
                                      float sc_hpf_freq, float sc_lpf_freq, float sample_rate) {

    // Aggiorna filtri sidechain
    biquad_set(&chain->hpf_filter, 1, sc_hpf_freq, 0.707f, sample_rate); // HPF, Q=0.707 per Butterworth
    biquad_set(&chain->lpf_filter, 0, sc_lpf_freq, 0.707f, sample_rate); // LPF, Q=0.707 per Butterworth

    // Sidechain processing: applica filtri e rettifica (RMS o Peak)
    float filtered_sc = biquad_process(&chain->hpf_filter, sc_sample);
    filtered_sc = biquad_process(&chain->lpf_filter, filtered_sc);

    // Rilevazione envelope (qui una semplice rettifica e smoothing)
    float current_level = fabsf(filtered_sc); // Rettifica a onda intera (Full-wave rectification)

    // Smoothing dell'envelope (costante di tempo basata su attacco/rilascio)
    // Velocità di attacco/rilascio per la variazione dell'envelope
    float attack_coef = expf(-1.0f / (attack_time * sample_rate));
    float release_coef = expf(-1.0f / (release_time * sample_rate));

    if (current_level > chain->envelope) {
        chain->envelope = current_level + attack_coef * (chain->envelope - current_level);
    } else {
        chain->envelope = current_level + release_coef * (chain->envelope - current_level);
    }
    // Clamping per evitare NaN o valori strani
    if (chain->envelope < 1e-10f) chain->envelope = 1e-10f; // Evita log(0)

    // Calcolo della Gain Reduction basato sul modello 1176 (o semplificato)
    // L'1176 non ha una threshold esplicita, ma una "fixed threshold" e l'input gain la spinge.
    // La riduzione di guadagno non è lineare con l'envelope come in un compressore VCA.
    // È la caratteristica non lineare del JFET che causa la compressione.
    // Qui useremo una threshold implicita e un calcolo di GR basato su un modello logaritmico.

    // Simuliamo una "threshold" interna che è fissa (es. 0.1) e il ratio.
    // L'input_gain nel TTL è la manopola "Input" dell'1176.

    float gain_reduction_db;
    // La ratio dell'1176 è più complessa. Per i valori 4, 8, 12, 20:1.
    // La modalità "All Button" è un comportamento unico, non solo 25:1.
    // Qui, usiamo 'ratio' direttamente.
    float threshold_level = 0.1f; // Una threshold fissa interna per la compressione

    if (chain->envelope > threshold_level) {
        // La gain reduction è logaritmica.
        // GR_dB = (threshold_dB - current_level_dB) * (1 - 1/ratio)
        // Per l'1176, la curva è più complessa, il "ratio" è approssimato.
        gain_reduction_db = (log10f(threshold_level) - log10f(chain->envelope)) * (1.0f - (1.0f / ratio));
        if (gain_reduction_db > 0.0f) gain_reduction_db = 0.0f; // Solo riduzione di guadagno
    } else {
        gain_reduction_db = 0.0f;
    }

    // Converti la gain reduction da dB a fattore lineare
    float gain_reduction_linear = db_to_linear(gain_reduction_db);

    // Applicare il gain reduction
    return in_sample * gain_reduction_linear;
}


// Struttura principale del plugin
typedef struct {
    // Porte di controllo (mappate dal .ttl)
    float* input_gain;
    float* output_gain;
    float* input_pad_10db;
    float* bypass;
    float* normalize_output;
    float* ms_mode_active;
    float* external_sc_active;
    float* attack;
    float* release;
    float* ratio;
    float* input_clip_drive;
    float* output_clip_drive;
    float* sc_hpf_freq;
    float* sc_lpf_freq;

    // Porte audio
    const float* audio_in_L;
    const float* audio_in_R;
    float* audio_out_L;
    float* audio_out_R;
    const float* audio_sidechain_in_L; // Input sidechain esterno
    const float* audio_sidechain_in_R;

    // Dati interni dello stato
    float sample_rate;

    // Due istanze della catena di compressione: una per Mid/Left, una per Side/Right
    CompressorChain mid_or_left_chain;
    CompressorChain side_or_right_chain;

    LV2_Log_Logger logger;

} GUA76;

// Inizializzazione della catena del compressore
static void compressor_chain_init(CompressorChain* chain) {
    chain->envelope = 0.0f;
    chain->target_gain = 1.0f;
    biquad_set(&chain->hpf_filter, 1, 20.0f, 0.707f, 44100.0f); // Default HPF
    biquad_set(&chain->lpf_filter, 0, 20000.0f, 0.707f, 44100.0f); // Default LPF
}


// Funzione di inizializzazione del plugin
static LV2_Handle
instantiate(const LV2_Descriptor* descriptor,
            double                    sample_rate,
            LV2_URID_Map* map,
            const LV2_Feature* const* features) {

    GUA76* plugin = (GUA76*)calloc(1, sizeof(GUA76));
    if (!plugin) return NULL;

    plugin->sample_rate = (float)sample_rate;

    // Inizializza le catene del compressore
    compressor_chain_init(&plugin->mid_or_left_chain);
    compressor_chain_init(&plugin->side_or_right_chain);

    // Inizializza il logger (opzionale, per debug)
    lv2_log_logger_init(&plugin->logger, map, features);

    return (LV2_Handle)plugin;
}

// Funzione per connettere le porte
static void
connect_port(LV2_Handle instance, uint32_t port, void* data) {
    GUA76* plugin = (GUA76*)instance;

    switch ((GUA76_PortIndex)port) {
        case GUA76_INPUT_GAIN:           plugin->input_gain = (float*)data; break;
        case GUA76_OUTPUT_GAIN:          plugin->output_gain = (float*)data; break;
        case GUA76_INPUT_PAD_10DB:       plugin->input_pad_10db = (float*)data; break;
        case GUA76_BYPASS:               plugin->bypass = (float*)data; break;
        case GUA76_NORMALIZE_OUTPUT:     plugin->normalize_output = (float*)data; break;
        case GUA76_MS_MODE_ACTIVE:       plugin->ms_mode_active = (float*)data; break;
        case GUA76_EXTERNAL_SC_ACTIVE:   plugin->external_sc_active = (float*)data; break;
        case GUA76_ATTACK:               plugin->attack = (float*)data; break;
        case GUA76_RELEASE:              plugin->release = (float*)data; break;
        case GUA76_RATIO:                plugin->ratio = (float*)data; break;
        case GUA76_INPUT_CLIP_DRIVE:     plugin->input_clip_drive = (float*)data; break;
        case GUA76_OUTPUT_CLIP_DRIVE:    plugin->output_clip_drive = (float*)data; break;
        case GUA76_SC_HPF_FREQ:          plugin->sc_hpf_freq = (float*)data; break;
        case GUA76_SC_LPF_FREQ:          plugin->sc_lpf_freq = (float*)data; break;
        case GUA76_AUDIO_IN_L:           plugin->audio_in_L = (const float*)data; break;
        case GUA76_AUDIO_IN_R:           plugin->audio_in_R = (const float*)data; break;
        case GUA76_AUDIO_OUT_L:          plugin->audio_out_L = (float*)data; break;
        case GUA76_AUDIO_OUT_R:          plugin->audio_out_R = (float*)data; break;
        case GUA76_AUDIO_SIDECHAIN_IN_L: plugin->audio_sidechain_in_L = (const float*)data; break;
        case GUA76_AUDIO_SIDECHAIN_IN_R: plugin->audio_sidechain_in_R = (const float*)data; break;
    }
}

// Funzione di attivazione (chiamata quando il plugin è attivo)
static void
activate(LV2_Handle instance) {
    // Reset dello stato del compressore all'attivazione
    GUA76* plugin = (GUA76*)instance;
    compressor_chain_init(&plugin->mid_or_left_chain);
    compressor_chain_init(&plugin->side_or_right_chain);
}

// Funzione di elaborazione (il cuore del plugin)
static void
run(LV2_Handle instance, uint32_t n_samples) {
    GUA76* plugin = (GUA76*)instance;

    const float bypass_active = *plugin->bypass;
    const float ms_mode_active = *plugin->ms_mode_active;
    const float external_sc_active = *plugin->external_sc_active;

    // --- 1. Gestione Bypass ---
    if (bypass_active > 0.5f) {
        for (uint32_t i = 0; i < n_samples; ++i) {
            plugin->audio_out_L[i] = plugin->audio_in_L[i];
            plugin->audio_out_R[i] = plugin->audio_in_R[i];
        }
        return; // Termina la funzione run, saltando l'elaborazione del compressore
    }

    // --- Valori dei Parametri ---
    const float input_gain_db = *plugin->input_gain;
    const float output_gain_db = *plugin->output_gain;
    const float input_pad_10db_active = *plugin->input_pad_10db;
    const float normalize_output_active = *plugin->normalize_output;
    const float attack_time = *plugin->attack;
    const float release_time = *plugin->release;
    const float ratio_val = *plugin->ratio;
    const float input_clip_drive = *plugin->input_clip_drive;
    const float output_clip_drive = *plugin->output_clip_drive;
    const float sc_hpf_freq = *plugin->sc_hpf_freq;
    const float sc_lpf_freq = *plugin->sc_lpf_freq;

    // Conversione Gain da dB a lineare
    float current_input_gain_linear = db_to_linear(input_gain_db);
    float current_output_gain_linear = db_to_linear(output_gain_db);

    // Applicazione del pad
    if (input_pad_10db_active > 0.5f) {
        current_input_gain_linear *= db_to_linear(-10.0f); // Applica -10 dB
    }

    // Attenuazione per la normalizzazione di output (se attiva)
    const float normalize_attenuation_db = -6.0f; // Esempio: attenua di 6 dB
    if (normalize_output_active > 0.5f) {
        current_output_gain_linear *= db_to_linear(normalize_attenuation_db);
    }

    // --- Loop di Elaborazione per Campione ---
    for (uint32_t i = 0; i < n_samples; ++i) {
        float in_L = plugin->audio_in_L[i];
        float in_R = plugin->audio_in_R[i];
        float out_L, out_R;

        // --- Gestione del Sidechain Source (interna o esterna) ---
        float sc_L_source, sc_R_source;
        // Controlla se external_sc_active è ON E se le porte esterne sono connesse (non NULL)
        if (external_sc_active > 0.5f && plugin->audio_sidechain_in_L && plugin->audio_sidechain_in_R) {
            sc_L_source = plugin->audio_sidechain_in_L[i];
            sc_R_source = plugin->audio_sidechain_in_R[i];
        } else {
            // Usa sidechain interna (dal segnale principale)
            sc_L_source = in_L;
            sc_R_source = in_R;
        }

        // --- APPLICA INPUT GAIN E PAD (iniziale) ---
        in_L *= current_input_gain_linear;
        in_R *= current_input_gain_linear;

        // --- APPLICA SOFT CLIP ALL'INPUT (se drive > 0) ---
        if (input_clip_drive > 0.001f) {
            in_L = soft_clip(in_L, input_clip_drive);
            in_R = soft_clip(in_R, input_clip_drive);
        }

        // --- Logica di Elaborazione (M/S vs L/R) ---
        if (ms_mode_active > 0.5f) {
            // --- M/S Encoding ---
            float mid_in = (in_L + in_R) * 0.5f;
            float side_in = (in_L - in_R) * 0.5f;

            // --- M/S Sidechain Source (tipicamente Mid per compressori M/S) ---
            float sc_mono_mid = (sc_L_source + sc_R_source) * 0.5f; // Sidechain derivata dal Mid del segnale SC

            // --- Processing Mid Chain ---
            float processed_mid = compressor_process_sample(&plugin->mid_or_left_chain, mid_in, sc_mono_mid,
                                                            attack_time, release_time, ratio_val,
                                                            sc_hpf_freq, sc_lpf_freq, plugin->sample_rate);

            // --- Processing Side Chain ---
            float processed_side = compressor_process_sample(&plugin->side_or_right_chain, side_in, sc_mono_mid, // Side chain usa lo stesso sc_mono_mid
                                                             attack_time, release_time, ratio_val,
                                                             sc_hpf_freq, sc_lpf_freq, plugin->sample_rate);

            // --- M/S Decoding ---
            out_L = processed_mid + processed_side;
            out_R = processed_mid - processed_side;
        } else {
            // --- L/R Processing (senza M/S, compressione stereo linked) ---
            // Il sidechain per la compressione L/R linked
            float sc_mono_lr = (sc_L_source + sc_R_source) * 0.5f;

            // Applica la compressione a entrambi i canali usando la stessa sidechain
            out_L = compressor_process_sample(&plugin->mid_or_left_chain, in_L, sc_mono_lr,
                                              attack_time, release_time, ratio_val,
                                              sc_hpf_freq, sc_lpf_freq, plugin->sample_rate);
            out_R = compressor_process_sample(&plugin->side_or_right_chain, in_R, sc_mono_lr,
                                              attack_time, release_time, ratio_val,
                                              sc_hpf_freq, sc_lpf_freq, plugin->sample_rate);
            // NOTA: Se volessi unlinked L/R, dovresti passare sc_L_source a mid_or_left_chain
            // e sc_R_source a side_or_right_chain. Ma per un 1176, linked stereo è più comune.
        }

        // --- APPLICA OUTPUT GAIN (e normalizzazione già inclusa in current_output_gain_linear) ---
        out_L *= current_output_gain_linear;
        out_R *= current_output_gain_linear;

        // --- APPLICA SOFT CLIP ALL'OUTPUT (se drive > 0) ---
        if (output_clip_drive > 0.001f) {
            out_L = soft_clip(out_L, output_clip_drive);
            out_R = soft_clip(out_R, output_clip_drive);
        }

        // --- Scrivi l'output ---
        plugin->audio_out_L[i] = out_L;
        plugin->audio_out_R[i] = out_R;
    }
}

// Funzione di disattivazione
static void
deactivate(LV2_Handle instance) {
    // Non è necessario fare nulla qui a meno che non ci siano risorse da rilasciare
}

// Funzione di pulizia (rilascia la memoria)
static void
cleanup(LV2_Handle instance) {
    GUA76* plugin = (GUA76*)instance;
    free(plugin);
}

// Funzione per estendere il comportamento del plugin (es. per l'UI)
static const void*
extension_data(const char* uri) {
    return NULL;
}

// Descrizione del plugin per LV2
static const LV2_Descriptor descriptor = {
    GUA76_URI,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    extension_data
};

// Punto di ingresso del modulo LV2
LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor(uint32_t index) {
    if (index == 0) {
        return &descriptor;
    }
    return NULL;
}
