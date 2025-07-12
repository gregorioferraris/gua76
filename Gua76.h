#ifndef GUA76_H
#define GUA76_H

#define GUA76_URI "http://your-plugin.com/plugins/gua76" // URI univoco per il tuo plugin

typedef enum {
    GUA76_AUDIO_IN_L    = 0,
    GUA76_AUDIO_IN_R    = 1,
    GUA76_AUDIO_OUT_L   = 2,
    GUA76_AUDIO_OUT_R   = 3,

    // Ingressi Sidechain esterni
    GUA76_SIDECHAIN_IN_L = 4,
    GUA76_SIDECHAIN_IN_R = 5,

    // Controlli principali (simili al 1176)
    GUA76_INPUT         = 6,  // Livello di input
    GUA76_OUTPUT        = 7,  // Livello di output
    GUA76_ATTACK        = 8,  // Tempo di attacco (0.0 = veloce, 1.0 = lento)
    GUA76_RELEASE       = 9,  // Tempo di rilascio (0.0 = veloce, 1.0 = lento)
    GUA76_RATIO         = 10,  // Selezione del rapporto di compressione (0=4:1, 1=8:1, 2=12:1, 3=20:1, 4=All-Button)
    GUA76_METER_MODE    = 11,  // Selezione del meter (0=GR, 1=Input, 2=Output)
    GUA76_BYPASS        = 12, // Bypass On/Off
    GUA76_DRIVE_SATURATION = 13, // Nuovo controllo per saturazione/drive aggiuntivo

    // Controlli aggiuntivi (moderni)
    GUA76_OVERSAMPLING  = 14, // Oversampling On/Off
    GUA76_SIDECHAIN_HPF_ON  = 15, // Sidechain HPF On/Off
    GUA76_SIDECHAIN_HPF_FREQ= 16, // Sidechain HPF Frequenza
    GUA77_SIDECHAIN_HPF_Q   = 17, // Nuovo: Q per i filtri sidechain HPF/LPF
    GUA76_SIDECHAIN_LPF_ON  = 18, // Sidechain LPF On/Off
    GUA76_SIDECHAIN_LPF_FREQ= 19, // Sidechain LPF Frequenza
    GUA76_SIDECHAIN_LISTEN  = 20, // Ascolta il segnale sidechain processato

    GUA76_MIDSIDE_MODE  = 21, // Nuovo: Mid-Side On/Off
    GUA76_MIDSIDE_LINK  = 22, // Nuovo: Mid-Side Linking (0=indipendente, 1=linkato)

    GUA76_PAD_10DB      = 23, // Nuovo: -10dB Pad On/Off

    // Porte per i valori dei meter (Output dal plugin alla GUI)
    GUA76_PEAK_GR       = 24, // Valore del meter Gain Reduction (dB)
    GUA76_PEAK_IN_L     = 25, // Valore di picco Input Left (dB)
    GUA76_PEAK_IN_R     = 26, // Valore di picco Input Right (dB)
    GUA76_PEAK_OUT_L    = 27, // Valore di picco Output Left (dB)
    GUA76_PEAK_OUT_R    = 28  // Valore di picco Output Right (dB)

} Gua76PortIndex;

#endif // GUA76_H
