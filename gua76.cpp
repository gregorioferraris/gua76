// ... (parte superiore del file gua76.cpp)

// Funzione di conversione da dB a fattore lineare
static inline float db_to_linear(float db) {
    return powf(10.0f, db / 20.0f);
}

// ... (dentro la struct GUA76)
typedef struct {
    // ... (altre porte di controllo)

    // NUOVI range e default per i gain
    float* input_gain;   // Ora da -20 a +20 dB
    float* output_gain;  // Ora da -20 a +20 dB

    // NUOVO controllo per il pad
    float* input_pad_10db; // 0.0 per OFF, 1.0 per ON

    // ... (resto della struct)

} GUA76;

// ... (dentro la funzione run)
static void run(LV2_Handle instance, uint32_t n_samples) {
    GUA76* plugin = (GUA76*)instance;

    // ... (valori dei parametri esistenti)

    // NUOVI valori dei parametri
    const float input_gain_db = *plugin->input_gain;
    const float output_gain_db = *plugin->output_gain;
    const float input_pad_10db_active = *plugin->input_pad_10db; // Questo sarà 0.0 o 1.0

    // Conversione da dB a lineare
    float current_input_gain_linear = db_to_linear(input_gain_db);
    float current_output_gain_linear = db_to_linear(output_gain_db);

    // Applicazione del pad
    if (input_pad_10db_active > 0.5f) { // Se il pad è "attivo" (valore >= 0.5)
        current_input_gain_linear *= db_to_linear(-10.0f); // Applica -10 dB
    }

    // ... (dentro il loop for)
    for (uint32_t i = 0; i < n_samples; ++i) {
        // ... (resto del loop)

        // --- 2. Stadio Input con Soft Clipping ---
        mid_in *= current_input_gain_linear; // Usa il guadagno calcolato
        side_in *= current_input_gain_linear;
        if (input_clip_drive > 0.001f) {
            mid_in = soft_clip(mid_in, input_clip_drive);
            side_in = soft_clip(side_in, input_clip_drive);
        }

        // ... (logica di compressione)

        // --- 6. Stadio Output con Soft Clipping ---
        mid_in *= current_output_gain_linear; // Usa il guadagno calcolato
        side_in *= current_output_gain_linear;
        if (output_clip_drive > 0.001f) {
            mid_in = soft_clip(mid_in, output_clip_drive);
            side_in = soft_clip(side_in, output_clip_drive);
        }

        // ... (resto del loop)
    }
}
