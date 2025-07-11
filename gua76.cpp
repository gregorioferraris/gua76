#include <cmath> // Per funzioni matematiche come atan, tanh, fabsf, expf, powf
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/urid/urid.h>
#include <lv2/midi/midi.h>
#include <lv2/log/log.h>
#include <lv2/log/logger.h>
#include <lv2/time/time.h>
#include <lv2/units/units.h>

// Definizione dell'URI del plugin (assicurati che sia unico!)
#define GUA76_URI "http://moddevices.com/plugins/mod-devel/gua76"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- Classe BiquadFilter (necessaria per la sidechain filtrata) ---
// (Identica a quella fornita in precedenza)
class BiquadFilter {
public:
    BiquadFilter() : b0(0), b1(0), b2(0), a1(0), a2(0), z1(0), z2(0) {}

    void set_coefficients(float new_b0, float new_b1, float new_b2, float new_a1, float new_a2) {
        b0 = new_b0; b1 = new_b1; b2 = new_b2; a1 = new_a1; a2 = new_a2;
    }

    void set_lowpass(float freq_hz, float Q, float sample_rate) {
        float omega = 2.0f * M_PI * freq_hz / sample_rate;
        float alpha = sinf(omega) / (2.0f * Q);
        float cos_omega = cosf(omega);

        float a0 = 1.0f + alpha;
        b0 = (1.0f - cos_omega) / 2.0f;
        b1 = 1.0f - cos_omega;
        b2 = (1.0f - cos_omega) / 2.0f;
        a1 = -2.0f * cos_omega;
        a2 = 1.0f - alpha;

        // Normalizzazione
        b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    }
    
    void set_highpass(float freq_hz, float Q, float sample_rate) {
        float omega = 2.0f * M_PI * freq_hz / sample_rate;
        float alpha = sinf(omega) / (2.0f * Q);
        float cos_omega = cosf(omega);

        float a0 = 1.0f + alpha;
        b0 = (1.0f + cos_omega) / 2.0f;
        b1 = -(1.0f + cos_omega);
        b2 = (1.0f + cos_omega) / 2.0f;
        a1 = -2.0f * cos_omega;
        a2 = 1.0f - alpha;

        // Normalizzazione
        b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    }

    float process_sample(float input_sample) {
        float output_sample = b0 * input_sample + b1 * z1 + b2 * z2 - a1 * z1 - a2 * z2;
        z2 = z1;
        z1 = input_sample;
        return output_sample;
    }

    void reset() { z1 = 0.0f; z2 = 0.0f; }

private:
    float b0, b1, b2, a1, a2;
    float z1, z2;
};
// --- Fine Classe BiquadFilter ---


// --- Funzione di Soft Clipping ---
static inline float soft_clip(float input, float drive_factor) {
    if (drive_factor < 0.001f) return input; // Evita atan(0) o effetti indesiderati con drive zero
    return std::atan(input * drive_factor) / std::atan(drive_factor);
}

// Funzione di trasferimento J-FET (rimane invariata per ora)
// Nota: Questo modello FET è per un singolo transistore.
// Nell'1176 la compressione è più complessa, con diversi FET in serie/parallelo.
// Per una replica più fedele dell'1176, potremmo aver bisogno di un modello FET più specifico
// o di una catena di guadagno più dettagliata per catturare il "knee" implicito e la saturazione.
static inline float jfet_transfer(float input, float Vp, float Idss) {
    float Vgs = input;
    if (Vgs >= 0.0f) {
        return Idss * (Vgs / (Idss + fabsf(Vgs)));
    } else if (Vgs < Vp) {
        return 0.0f;
    } else {
        float term = (1.0f - (Vgs / Vp));
        return Idss * (term * term);
    }
}

// Struttura per i dati di UNA singola catena di compressione (Mid o Side)
typedef struct {
    float envelope;
    float gain_reduction; // Potrebbe non essere strettamente necessario separarlo se si usa lo stesso algoritmo

    // Filtri sidechain specifici per questa catena
    BiquadFilter hpf_filter;
    BiquadFilter lpf_filter;

    // Parametri J-FET (anche se saranno uguali per Mid e Side, utile per modularità futura)
    float jfet_vp;
    float jfet_idss;

    // Per il compressore interno
    float attack_coeff;
    float release_coeff;
    float actual_ratio;

} CompressorChain;


// Struttura per i dati del plugin (rinominata a GUA76)
typedef struct {
    // Porte di controllo
    float* input_gain;
    float* output_gain;
    float* attack;
    float* release;
    float* ratio;

    // Controlli Soft Clipping
    float* input_clip_drive;
    float* output_clip_drive;

    // Controlli Sidechain (applicati a entrambi i filtri M e S)
    float* sc_hpf_freq;
    float* sc_lpf_freq;

    // Porte audio (stereo in/out)
    const float* audio_in_L;  // Input Left
    const float* audio_in_R;  // Input Right
    float* audio_out_L;       // Output Left
    float* audio_out_R;       // Output Right
    const float* audio_sidechain_in_L; // Sidechain Input Left
    const float* audio_sidechain_in_R; // Sidechain Input Right


    // Dati interni dello stato
    float sample_rate;

    // Due istanze della catena di compressione: una per Mid, una per Side
    CompressorChain mid_chain;
    CompressorChain side_chain;
    
    // Parametri J-FET (li teniamo qui come parametri globali/fissi per l'1176)
    float internal_jfet_vp;    // Ad esempio: -2.0f
    float internal_jfet_idss;  // Ad esempio: 0.005f


    LV2_Log_Logger logger;

} GUA76;


// Funzione 'instantiate'
static LV2_Handle instantiate(const LV2_Descriptor* descriptor,
                              double rate,
                              const char* bundle_path,
                              const LV2_Feature* const* features) {
    GUA76* plugin = (GUA76*)calloc(1, sizeof(GUA76));
    if (!plugin) return NULL;

    plugin->sample_rate = rate;
    lv2_log_logger_init(&plugin->logger, features);

    // Inizializza le catene di compressione
    plugin->mid_chain.envelope = 0.0f;
    plugin->mid_chain.gain_reduction = 1.0f;
    plugin->side_chain.envelope = 0.0f;
    plugin->side_chain.gain_reduction = 1.0f;

    // Imposta i parametri J-FET interni (questi non saranno esposti come controlli)
    plugin->internal_jfet_vp = -2.0f;   // Valore tipico per un J-FET
    plugin->internal_jfet_idss = 0.005f; // Valore tipico per un J-FET

    // Inizializza i filtri della sidechain per entrambe le catene
    plugin->mid_chain.hpf_filter.set_highpass(20.0f, 0.707f, plugin->sample_rate);
    plugin->mid_chain.lpf_filter.set_lowpass(20000.0f, 0.707f, plugin->sample_rate);
    plugin->side_chain.hpf_filter.set_highpass(20.0f, 0.707f, plugin->sample_rate);
    plugin->side_chain.lpf_filter.set_lowpass(20000.0f, 0.707f, plugin->sample_rate);

    return (LV2_Handle)plugin;
}

// Funzione 'connect_port'
static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    GUA76* plugin = (GUA76*)instance;

    // Assicurati che gli indici corrispondano a quelli nel .ttl e nella GUI!
    switch (port) {
        case 0:  plugin->input_gain = (float*)data; break;
        case 1:  plugin->output_gain = (float*)data; break;
        case 2:  plugin->attack = (float*)data; break;
        case 3:  plugin->release = (float*)data; break;
        case 4:  plugin->ratio = (float*)data; break;
        case 5:  plugin->input_clip_drive = (float*)data; break;
        case 6:  plugin->output_clip_drive = (float*)data; break;
        case 7:  plugin->sc_hpf_freq = (float*)data; break;
        case 8:  plugin->sc_lpf_freq = (float*)data; break;

        // Porte Audio Stereo
        // Nota: Le porte audio sono consecutive dopo le porte di controllo
        case 9:  plugin->audio_in_L = (const float*)data; break;
        case 10: plugin->audio_in_R = (const float*)data; break;
        case 11: plugin->audio_out_L = (float*)data; break;
        case 12: plugin->audio_out_R = (float*)data; break;
        case 13: plugin->audio_sidechain_in_L = (const float*)data; break;
        case 14: plugin->audio_sidechain_in_R = (const float*)data; break;
    }
}

// Funzione 'run'
static void run(LV2_Handle instance, uint32_t n_samples) {
    GUA76* plugin = (GUA76*)instance;

    // Valori dei parametri (condivisi per Mid e Side)
    const float in_gain = *plugin->input_gain;
    const float out_gain = *plugin->output_gain;
    const float attack_ms = *plugin->attack;
    const float release_ms = *plugin->release;
    const float ratio_value = *plugin->ratio;

    const float input_clip_drive = *plugin->input_clip_drive;
    const float output_clip_drive = *plugin->output_clip_drive;

    const float sc_hpf_freq = *plugin->sc_hpf_freq;
    const float sc_lpf_freq = *plugin->sc_lpf_freq;

    // Aggiorna i coefficienti dei filtri Sidechain per ENTRAMBE le catene
    plugin->mid_chain.hpf_filter.set_highpass(sc_hpf_freq, 0.707f, plugin->sample_rate);
    plugin->mid_chain.lpf_filter.set_lowpass(sc_lpf_freq, 0.707f, plugin->sample_rate);
    plugin->side_chain.hpf_filter.set_highpass(sc_hpf_freq, 0.707f, plugin->sample_rate);
    plugin->side_chain.lpf_filter.set_lowpass(sc_lpf_freq, 0.707f, plugin->sample_rate);

    // Calcola i coefficienti di attacco e rilascio (condivisi)
    float attack_coeff = 0.0f;
    if (attack_ms > 0.0f) {
        attack_coeff = expf(-1.0f / (attack_ms * 0.001f * plugin->sample_rate));
    }
    float release_coeff = 0.0f;
    if (release_ms > 0.0f) {
        release_coeff = expf(-1.0f / (release_ms * 0.001f * plugin->sample_rate));
    }

    // Mappatura Ratio (condivisa)
    float actual_ratio = 1.0f;
    if (ratio_value == 4.0f) {
        actual_ratio = 4.0f;
    } else if (ratio_value == 8.0f) {
        actual_ratio = 8.0f;
    } else if (ratio_value == 12.0f) {
        actual_ratio = 12.0f;
    } else if (ratio_value == 20.0f) {
        actual_ratio = 20.0f;
    } else if (ratio_value == 25.0f) { // Valore speciale per All-Button mode (es. 25.0f)
        actual_ratio = 20.0f; // Il ratio è 20:1, ma con comportamento distorto
        // TODO: Qui implementare gli effetti di distorsione/non linearità aggiuntivi dell'All-Button mode.
    }

    // Assegna i coefficienti alle catene di compressione
    plugin->mid_chain.attack_coeff = attack_coeff;
    plugin->mid_chain.release_coeff = release_coeff;
    plugin->mid_chain.actual_ratio = actual_ratio;
    plugin->side_chain.attack_coeff = attack_coeff;
    plugin->side_chain.release_coeff = release_coeff;
    plugin->side_chain.actual_ratio = actual_ratio;

    // Assegna i parametri J-FET interni alle catene
    plugin->mid_chain.jfet_vp = plugin->internal_jfet_vp;
    plugin->mid_chain.jfet_idss = plugin->internal_jfet_idss;
    plugin->side_chain.jfet_vp = plugin->internal_jfet_vp;
    plugin->side_chain.jfet_idss = plugin->internal_jfet_idss;


    for (uint32_t i = 0; i < n_samples; ++i) {
        float in_L = plugin->audio_in_L[i];
        float in_R = plugin->audio_in_R[i];

        // --- 1. Codifica M/S in Input ---
        float mid_in = (in_L + in_R) * 0.5f; // Mid = (L+R)/2
        float side_in = (in_L - in_R) * 0.5f; // Side = (L-R)/2

        // --- 2. Stadio Input con Soft Clipping (applicato a Mid e Side separatamente) ---
        mid_in *= in_gain;
        side_in *= in_gain;
        if (input_clip_drive > 0.001f) {
            mid_in = soft_clip(mid_in, input_clip_drive);
            side_in = soft_clip(side_in, input_clip_drive);
        }

        // --- 3. Gestione Sidechain Filtrata (separata per Mid e Side) ---
        float sc_mid_sample, sc_side_sample;

        // Se sidechain esterna è collegata, usala. Altrimenti usa il segnale M/S interno.
        if (plugin->audio_sidechain_in_L && plugin->audio_sidechain_in_R) {
            float sc_L = plugin->audio_sidechain_in_L[i];
            float sc_R = plugin->audio_sidechain_in_R[i];
            sc_mid_sample = (sc_L + sc_R) * 0.5f;
            sc_side_sample = (sc_L - sc_R) * 0.5f;
        } else {
            sc_mid_sample = mid_in;  // Sidechain interna Mid
            sc_side_sample = side_in; // Sidechain interna Side
        }

        // Applica i filtri alla Sidechain Mid
        sc_mid_sample = plugin->mid_chain.hpf_filter.process_sample(sc_mid_sample);
        sc_mid_sample = plugin->mid_chain.lpf_filter.process_sample(sc_mid_sample);

        // Applica i filtri alla Sidechain Side
        sc_side_sample = plugin->side_chain.hpf_filter.process_sample(sc_side_sample);
        sc_side_sample = plugin->side_chain.lpf_filter.process_sample(sc_side_sample);


        // --- 4. Calcolo Gain Reduction (separato per Mid e Side) ---

        // MID Chain
        float mid_detection = fabsf(sc_mid_sample);
        if (mid_detection > plugin->mid_chain.envelope) {
            plugin->mid_chain.envelope = (plugin->mid_chain.attack_coeff * plugin->mid_chain.envelope) + ((1.0f - plugin->mid_chain.attack_coeff) * mid_detection);
        } else {
            plugin->mid_chain.envelope = (plugin->mid_chain.release_coeff * plugin->mid_chain.envelope) + ((1.0f - plugin->mid_chain.release_coeff) * mid_detection);
        }

        float mid_target_gain = 1.0f;
        if (plugin->mid_chain.envelope > 0.0f) { // Evita divisione per zero
            float mid_overshoot = plugin->mid_chain.envelope / 1.0f; // Soglia implicita
            if (mid_overshoot > 1.0f) {
                mid_target_gain = powf(1.0f / mid_overshoot, (plugin->mid_chain.actual_ratio - 1.0f) / plugin->mid_chain.actual_ratio);
            }
        }
        // Applicazione del J-FET model per la riduzione del guadagno
        // Questo è il punto dove il modello J-FET può influenzare il gain_reduction
        // O può essere applicato direttamente al segnale audio, in base a come vuoi il modello 1176.
        // Per l'1176, il FET è l'elemento di riduzione del guadagno stesso.
        // La simulazione del J-FET è un modello della corrente di drain, non direttamente un guadagno.
        // Dobbiamo calcolare un gain reduction basato sulla "conduttanza" del FET simulato.
        // Per semplificare, useremo la target_gain calcolata sopra.
        // Se vogliamo una simulazione più profonda del FET, dovremmo integrare jfet_transfer nel calcolo della target_gain.
        // Per ora, lo lasciamo come modello di "output saturation" se mai lo si volesse riutilizzare.


        // SIDE Chain (stessa logica della MID chain)
        float side_detection = fabsf(sc_side_sample);
        if (side_detection > plugin->side_chain.envelope) {
            plugin->side_chain.envelope = (plugin->side_chain.attack_coeff * plugin->side_chain.envelope) + ((1.0f - plugin->side_chain.attack_coeff) * side_detection);
        } else {
            plugin->side_chain.envelope = (plugin->side_chain.release_coeff * plugin->side_chain.envelope) + ((1.0f - plugin->side_chain.release_coeff) * side_detection);
        }

        float side_target_gain = 1.0f;
        if (plugin->side_chain.envelope > 0.0f) {
            float side_overshoot = plugin->side_chain.envelope / 1.0f;
            if (side_overshoot > 1.0f) {
                side_target_gain = powf(1.0f / side_overshoot, (plugin->side_chain.actual_ratio - 1.0f) / plugin->side_chain.actual_ratio);
            }
        }
        
        // --- 5. Applica Gain Reduction ai segnali Mid e Side ---
        mid_in *= mid_target_gain;
        side_in *= side_target_gain;

        // --- 6. Stadio Output con Soft Clipping (applicato a Mid e Side) ---
        mid_in *= out_gain;
        side_in *= out_gain;
        if (output_clip_drive > 0.001f) {
            mid_in = soft_clip(mid_in, output_clip_drive);
            side_in = soft_clip(side_in, output_clip_drive);
        }

        // --- 7. Decodifica M/S in Output ---
        plugin->audio_out_L[i] = mid_in + side_in; // Left = Mid + Side
        plugin->audio_out_R[i] = mid_in - side_in; // Right = Mid - Side
    }
}

// Funzioni 'deactivate', 'cleanup', 'extension_data', 'work' (rimangono invariate)
static void deactivate(LV2_Handle instance) {}
static void cleanup(LV2_Handle instance) {
    free(instance);
}
static LV2_Worker_Status work(LV2_Handle instance,
                              LV2_Worker_Respond_Function respond,
                              LV2_Worker_Request_Function request,
                              void* feature_data,
                              uint32_t size,
                              const void* data) {
    return LV2_WORKER_SUCCESS;
}
static const void* extension_data(const char* uri) { return NULL; }

// Descrittore del plugin
static const LV2_Descriptor descriptor = {
    GUA76_URI,
    instantiate,
    connect_port,
    run,
    deactivate,
    cleanup,
    extension_data
};

// Funzione di ingresso del plugin
LV2_Descriptor const* lv2_descriptor(uint32_t index) {
    switch (index) {
        case 0:  return &descriptor;
        default: return NULL;
    }
}
