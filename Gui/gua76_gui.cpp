#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>

// Include GLFW
#include <GLFW/glfw3.h>

// Include GLAD
#include "glad/glad.h"

// Include Dear ImGui
#include "imgui/imgui.h"
#include "imgui/imgui_impl_glfw.h"
#include "imgui/imgui_impl_opengl3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Definizione URI del plugin e della GUI (Devono corrispondere al .ttl)
#define GUA76_GUI_URI    "http://moddevices.com/plugins/mod-devel/gua76_ui"
#define GUA76_PLUGIN_URI "http://moddevices.com/plugins/mod-devel/gua76"

// Enum degli indici delle porte (devono corrispondere a gua76.ttl)
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
    GUA76_SC_HPF_Q = 14,
    GUA76_SC_LPF_Q = 15,
    GUA76_METER_DISPLAY_MODE = 16, // Input: 0=Input, 1=Output, 2=Input-Output
    GUA76_GAIN_REDUCTION_METER = 17, // Output for GUI
    GUA76_INPUT_RMS = 18, // Output for GUI
    GUA76_OUTPUT_RMS = 19, // Output for GUI
    // Le porte audio non sono gestite direttamente dalla GUI, quindi non le elenchiamo qui.
} GUA76_PortIndex;

// Struttura per il nostro stato della GUI
typedef struct {
    LV2_URID_Map* map;
    LV2_UI_Write_Function write_function;
    LV2_UI_Controller controller;

    GLFWwindow* window; // Finestra GLFW
    LV2_UI_Idle_Function idle_interface;

    // Valori attuali dei parametri del plugin (cache) - Ora 20 parametri di controllo/output
    float values[20]; // Aggiornato per riflettere il numero di porte di controllo + metering

    // Nomi dei pulsanti del Ratio
    const char* ratio_labels[5];
    // Nomi per le manopole a scatti (Attack/Release)
    const char* attack_labels[7];
    const float attack_values[7];
    const char* release_labels[6];
    const float release_values[6];
    // Nomi per le modalità del VU meter di sinistra
    const char* meter_mode_labels[3];

} Gua76UI;

// Callback per errori GLFW
static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

// Funzione helper per disegnare un VU Meter verticale
// 'value_db': Il valore in dB da mostrare (es. -20.0f)
// 'min_db': Il minimo del range in dB (es. -30.0f)
// 'max_db': Il massimo del range in dB (es. 0.0f)
// 'size': ImVec2 per la dimensione del meter (larghezza, altezza)
// 'peak_value': Un valore opzionale per un indicatore di picco
static void DrawVUMeter(const char* label, float value_db, float min_db, float max_db, ImVec2 size, float peak_value = -1000.0f) {
    ImGui::BeginGroup();
    ImGui::TextUnformatted(label); // Mostra il nome del meter

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    float height = size.y;
    float width = size.x;

    // Colore di sfondo del meter
    draw_list->AddRectFilled(p, ImVec2(p.x + width, p.y + height), IM_COL32(20, 20, 20, 255));

    // Normalizza il valore dB in un range da 0 a 1 per il disegno
    // clamp a min/max per evitare overflow visuale
    float normalized_value = ImClamp((value_db - min_db) / (max_db - min_db), 0.0f, 1.0f);
    float fill_height = height * normalized_value;

    // Colore della barra (es. verde, giallo, rosso)
    ImU32 bar_color = IM_COL32(0, 200, 0, 255); // Green
    if (value_db > -6.0f) bar_color = IM_COL32(200, 200, 0, 255); // Yellow
    if (value_db > -3.0f) bar_color = IM_COL32(200, 0, 0, 255); // Red

    // Disegna la barra del livello
    draw_list->AddRectFilled(ImVec2(p.x, p.y + height - fill_height), ImVec2(p.x + width, p.y + height), bar_color);

    // Disegna tacche e etichette (esempio)
    for (float db_tick = min_db; db_tick <= max_db; db_tick += 5.0f) { // Tacche ogni 5 dB
        float normalized_tick = ImClamp((db_tick - min_db) / (max_db - min_db), 0.0f, 1.0f);
        float tick_y = p.y + height - (height * normalized_tick);
        draw_list->AddLine(ImVec2(p.x, tick_y), ImVec2(p.x + width * 0.2f, tick_y), IM_COL32(100, 100, 100, 255)); // Small tick

        if (fmodf(db_tick, 10.0f) == 0.0f) { // Etichetta ogni 10 dB
            char buf[16];
            snprintf(buf, sizeof(buf), "%.0f", db_tick);
            draw_list->AddText(ImVec2(p.x + width * 0.3f, tick_y - ImGui::GetTextLineHeight()/2), IM_COL32(150, 150, 150, 255), buf);
        }
    }

    // Disegna indicatore di picco (se fornito)
    if (peak_value > -900.0f) { // Un valore arbitrario per indicare che il picco è valido
        float normalized_peak = ImClamp((peak_value - min_db) / (max_db - min_db), 0.0f, 1.0f);
        float peak_y = p.y + height - (height * normalized_peak);
        draw_list->AddLine(ImVec2(p.x, peak_y), ImVec2(p.x + width, peak_y), IM_COL32(255, 255, 255, 255), 2.0f); // White line for peak
    }

    ImGui::Dummy(size); // Riserva lo spazio per il meter
    ImGui::EndGroup();
}


// Inizializzazione di GLFW, OpenGL e ImGui
static LV2_UI_Handle
instantiate(const LV2_UI_Descriptor* descriptor,
            const char* plugin_uri,
            const char* bundle_path,
            LV2_UI_Write_Function      write_function,
            LV2_UI_Controller          controller,
            LV2_UI_Widget* widget,
            const LV2_Feature* const* features) {

    if (strcmp(plugin_uri, GUA76_PLUGIN_URI) != 0) {
        fprintf(stderr, "Gua76UI: Plugin URI mismatch.\n");
        return NULL;
    }

    Gua76UI* ui = (Gua76UI*)calloc(1, sizeof(Gua76UI));
    if (!ui) return NULL;

    ui->write_function = write_function;
    ui->controller = controller;

    // Inizializza i valori dei parametri ai loro default (o 0)
    for (int i = 0; i < 20; ++i) { // 20 è il numero di parametri di controllo + metering
        ui->values[i] = 0.0f;
    }
    // Setta i default specifici da gua76.ttl
    ui->values[GUA76_INPUT_GAIN] = 0.0f;
    ui->values[GUA76_OUTPUT_GAIN] = 0.0f;
    ui->values[GUA76_INPUT_PAD_10DB] = 0.0f;
    ui->values[GUA76_BYPASS] = 0.0f;
    ui->values[GUA76_NORMALIZE_OUTPUT] = 0.0f;
    ui->values[GUA76_MS_MODE_ACTIVE] = 0.0f;
    ui->values[GUA76_EXTERNAL_SC_ACTIVE] = 0.0f;
    ui->values[GUA76_ATTACK] = 0.000020f;
    ui->values[GUA76_RELEASE] = 0.2f;
    ui->values[GUA76_RATIO] = 4.0f;
    ui->values[GUA76_INPUT_CLIP_DRIVE] = 1.0f;
    ui->values[GUA76_OUTPUT_CLIP_DRIVE] = 1.0f;
    ui->values[GUA76_SC_HPF_FREQ] = 20.0f;
    ui->values[GUA76_SC_LPF_FREQ] = 20000.0f;
    ui->values[GUA76_SC_HPF_Q] = 0.707f;
    ui->values[GUA76_SC_LPF_Q] = 0.707f;
    ui->values[GUA76_METER_DISPLAY_MODE] = 1.0f; // Default: Output

    // Setup labels for Ratio
    ui->ratio_labels[0] = "4:1";
    ui->ratio_labels[1] = "8:1";
    ui->ratio_labels[2] = "12:1";
    ui->ratio_labels[3] = "20:1";
    ui->ratio_labels[4] = "All"; // Per 25.0

    // Setup labels and values for Attack (7 steps)
    ui->attack_labels[0] = "20us"; ui->attack_values[0] = 0.000020f;
    ui->attack_labels[1] = "40us"; ui->attack_values[1] = 0.000040f;
    ui->attack_labels[2] = "80us"; ui->attack_values[2] = 0.000080f;
    ui->attack_labels[3] = "160us"; ui->attack_values[3] = 0.000160f;
    ui->attack_labels[4] = "320us"; ui->attack_values[4] = 0.000320f;
    ui->attack_labels[5] = "640us"; ui->attack_values[5] = 0.000640f;
    ui->attack_labels[6] = "800us"; ui->attack_values[6] = 0.000800f;

    // Setup labels and values for Release (6 steps)
    ui->release_labels[0] = "50ms"; ui->release_values[0] = 0.05f;
    ui->release_labels[1] = "100ms"; ui->release_values[1] = 0.1f;
    ui->release_labels[2] = "200ms"; ui->release_values[2] = 0.2f;
    ui->release_labels[3] = "400ms"; ui->release_values[3] = 0.4f;
    ui->release_labels[4] = "800ms"; ui->release_values[4] = 0.8f;
    ui->release_labels[5] = "1.1s"; ui->release_values[5] = 1.1f;

    // Setup labels for Meter Display Mode
    ui->meter_mode_labels[0] = "Input";
    ui->meter_mode_labels[1] = "Output";
    ui->meter_mode_labels[2] = "I-O Diff";


    // Cerca le feature necessarie
    for (int i = 0; features[i]; ++i) {
        if (strcmp(features[i]->URI, LV2_URID__map) == 0) {
            ui->map = (LV2_URID_Map*)features[i]->data;
        } else if (strcmp(features[i]->URI, LV2_UI__idle) == 0) {
            ui->idle_interface = (LV2_UI_Idle_Function)features[i]->data;
        }
    }

    if (!ui->map) {
        fprintf(stderr, "Gua76UI: Host does not support urid:map.\n");
        free(ui);
        return NULL;
    }

    // --- Inizializzazione GLFW ---
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        fprintf(stderr, "Gua76UI: Failed to initialize GLFW\n");
        free(ui);
        return NULL;
    }

    // Configura il contesto OpenGL (es. versione 3.3 Core Profile)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE); // Necessario per macOS
#endif

    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); // Rende la finestra nascosta
    ui->window = glfwCreateWindow(800, 500, "Gua76 GUI", NULL, NULL); // Dimensioni iniziali adatte
    if (!ui->window) {
        fprintf(stderr, "Gua76UI: Failed to create GLFW window\n");
        glfwTerminate();
        free(ui);
        return NULL;
    }

    glfwMakeContextCurrent(ui->window);
    glfwSwapInterval(1); // Enable vsync

    // --- Carica tutte le funzioni OpenGL con GLAD ---
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        fprintf(stderr, "Gua76UI: Failed to initialize GLAD\n");
        glfwDestroyWindow(ui->window);
        glfwTerminate();
        free(ui);
        return NULL;
    }

    // --- Inizializza Dear ImGui ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    ImGui::StyleColorsDark();

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    ImGui_ImplGlfw_InitForOpenGL(ui->window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // --- Collega la finestra GLFW al widget LV2 ---
#ifdef __linux__
    *widget = (LV2_UI_Widget)glfwGetX11Window(ui->window);
#elif __APPLE__
    // Per macOS, questo è un placeholder. Richiede un wrapper NSView/Cocoa.
    // Per ora, non funzionerà correttamente come UI embeddata.
    fprintf(stderr, "Gua76UI: macOS integration for GLFW window as LV2_UI_Widget not fully implemented (requires Cocoa wrapper).\n");
    // Potresti voler visualizzare una finestra standalone per debug su macOS se non hai un wrapper completo.
    // glfwSetWindowAttrib(ui->window, GLFW_VISIBLE, GLFW_TRUE); // Rende visibile per test
    *widget = NULL; // O un puntatore dummy per evitare crash se l'host non lo usa.
#elif _WIN32
    // Per Windows, questo è un placeholder. Richiede un wrapper HWND/WinAPI.
    fprintf(stderr, "Gua76UI: Windows integration for GLFW window as LV2_UI_Widget not fully implemented (requires WinAPI wrapper).\n");
    *widget = NULL;
#endif

    return (LV2_UI_Handle)ui;
}

// Funzione per gestire gli eventi del plugin (aggiornamenti dei parametri dal core)
static void
port_event(LV2_UI_Handle handle,
           uint32_t      port_index,
           uint32_t      buffer_size,
           uint32_t      format,
           const void* buffer) {

    Gua76UI* ui = (Gua76UI*)handle;

    // Assicurati che sia un valore float e che l'indice sia valido
    if (format == ui->map->map(ui->map->handle, LV2_ATOM_URI "#Float") &&
        port_index < sizeof(ui->values) / sizeof(ui->values[0])) {
        ui->values[port_index] = *(const float*)buffer;
        // La GUI verrà ridisegnata nel loop di idle
    }
}

// Funzione per trovare il valore più vicino in un array discreto
static int get_nearest_discrete_value_idx(float value, const float* arr, int size) {
    int nearest_idx = 0;
    float min_diff = fabsf(value - arr[0]);
    for (int i = 1; i < size; ++i) {
        float diff = fabsf(value - arr[i]);
        if (diff < min_diff) {
            min_diff = diff;
            nearest_idx = i;
        }
    }
    return nearest_idx;
}

// Funzione per il ciclo di esecuzione della GUI (rendering)
static int
ui_idle(LV2_UI_Handle handle) {
    Gua76UI* ui = (Gua76UI*)handle;
    if (!ui || !ui->window) return 0;

    // Inizia un nuovo frame ImGui
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Imposta le dimensioni e la posizione della finestra ImGui per riempire la finestra GLFW
    int display_w, display_h;
    glfwGetFramebufferSize(ui->window, &display_w, &display_h);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(display_w, display_h));

    // Inizia la finestra principale del plugin
    ImGui::Begin("Gua76 Compressor", NULL,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // --- Layout principale: Bypass, Pad ---
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f); // Pulsanti più arrotondati
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 5.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));

    if (ImGui::Button((ui->values[GUA76_BYPASS] > 0.5f) ? "BYPASS ON" : "BYPASS OFF", ImVec2(100, 30))) {
        ui->values[GUA76_BYPASS] = (ui->values[GUA76_BYPASS] > 0.5f) ? 0.0f : 1.0f;
        ui->write_function(ui->controller, GUA76_BYPASS, sizeof(float), 0, &ui->values[GUA76_BYPASS]);
    }
    ImGui::SameLine(0.0f, 20.0f); // Spazio tra i pulsanti

    if (ImGui::Button((ui->values[GUA76_INPUT_PAD_10DB] > 0.5f) ? "PAD -10dB ON" : "PAD -10dB OFF", ImVec2(120, 30))) {
        ui->values[GUA76_INPUT_PAD_10DB] = (ui->values[GUA76_INPUT_PAD_10DB] > 0.5f) ? 0.0f : 1.0f;
        ui->write_function(ui->controller, GUA76_INPUT_PAD_10DB, sizeof(float), 0, &ui->values[GUA76_INPUT_PAD_10DB]);
    }

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);

    ImGui::Separator();
    ImGui::Spacing();


    // --- Contenuto della Tab Bar ---
    static int current_tab = 0; // 0 for Main, 1 for Sidechain

    if (ImGui::BeginTabBar("MyTabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("Main Tab")) {
            current_tab = 0;
            // --- MAIN TAB CONTENT ---
            ImGui::PushItemWidth(150); // Larghezza per le manopole

            // Row 1: Meters and Big Knobs
            // VU Meter di Sinistra (Selezionabile)
            ImGui::BeginGroup();
            ImGui::Text("Meter Display:");
            for (int i = 0; i < IM_ARRAYSIZE(ui->meter_mode_labels); ++i) {
                if (ImGui::RadioButton(ui->meter_mode_labels[i], (int)ui->values[GUA76_METER_DISPLAY_MODE] == i)) {
                    ui->values[GUA76_METER_DISPLAY_MODE] = (float)i;
                    ui->write_function(ui->controller, GUA76_METER_DISPLAY_MODE, sizeof(float), 0, &ui->values[GUA76_METER_DISPLAY_MODE]);
                }
            }

            float meter_value = 0.0f;
            if ((int)ui->values[GUA76_METER_DISPLAY_MODE] == 0) { // Input
                meter_value = ui->values[GUA76_INPUT_RMS];
            } else if ((int)ui->values[GUA76_METER_DISPLAY_MODE] == 1) { // Output
                meter_value = ui->values[GUA76_OUTPUT_RMS];
            } else { // I-O Diff
                meter_value = ui->values[GUA76_OUTPUT_RMS] - ui->values[GUA76_INPUT_RMS];
            }
            DrawVUMeter("Lvl", meter_value, -30.0f, 0.0f, ImVec2(50, 150));
            ImGui::EndGroup();


            ImGui::SameLine(); ImGui::Dummy(ImVec2(20,0)); ImGui::SameLine(); // Spazio


            // Input Gain
            ImGui::BeginGroup();
            ImGui::Text("Input Gain");
            ImGui::VSliderFloat("##InputGain", ImVec2(70, 150), &ui->values[GUA76_INPUT_GAIN], -20.0f, 20.0f, "%.1f dB");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                ui->write_function(ui->controller, GUA76_INPUT_GAIN, sizeof(float), 0, &ui->values[GUA76_INPUT_GAIN]);
            }
            ImGui::EndGroup();


            ImGui::SameLine(); ImGui::Dummy(ImVec2(20,0)); ImGui::SameLine(); // Spazio


            // Output Gain
            ImGui::BeginGroup();
            ImGui::Text("Output Gain");
            ImGui::VSliderFloat("##OutputGain", ImVec2(70, 150), &ui->values[GUA76_OUTPUT_GAIN], -20.0f, 20.0f, "%.1f dB");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                ui->write_function(ui->controller, GUA76_OUTPUT_GAIN, sizeof(float), 0, &ui->values[GUA76_OUTPUT_GAIN]);
            }
            ImGui::EndGroup();

            ImGui::SameLine(); ImGui::Dummy(ImVec2(20,0)); ImGui::SameLine(); // Spazio

            // Attack (Manopola a Scatti)
            ImGui::BeginGroup();
            ImGui::Text("Attack");
            // Trova l'indice corrente più vicino al valore del plugin
            int current_attack_idx = get_nearest_discrete_value_idx(ui->values[GUA76_ATTACK], ui->attack_values, IM_ARRAYSIZE(ui->attack_values));
            ImGui::PushItemWidth(100);
            if (ImGui::SliderInt("##Attack", &current_attack_idx, 0, IM_ARRAYSIZE(ui->attack_values) - 1, ui->attack_labels[current_attack_idx])) {
                ui->values[GUA76_ATTACK] = ui->attack_values[current_attack_idx];
                ui->write_function(ui->controller, GUA76_ATTACK, sizeof(float), 0, &ui->values[GUA76_ATTACK]);
            }
            ImGui::PopItemWidth();
            ImGui::EndGroup();

            ImGui::SameLine(); ImGui::Dummy(ImVec2(20,0)); ImGui::SameLine(); // Spazio

            // Ratio Buttons
            ImGui::BeginGroup();
            ImGui::Text("Ratio:");
            const float ratio_values[] = {4.0f, 8.0f, 12.0f, 20.0f, 25.0f}; // 25.0 per All-Button
            for (int n = 0; n < IM_ARRAYSIZE(ratio_values); n++) {
                if (ImGui::RadioButton(ui->ratio_labels[n], ui->values[GUA76_RATIO] == ratio_values[n])) {
                    ui->values[GUA76_RATIO] = ratio_values[n];
                    ui->write_function(ui->controller, GUA76_RATIO, sizeof(float), 0, &ui->values[GUA76_RATIO]);
                }
            }
            ImGui::EndGroup();

            ImGui::SameLine(); ImGui::Dummy(ImVec2(20,0)); ImGui::SameLine(); // Spazio

            // Gain Reduction VU Meter (Destra)
            DrawVUMeter("GR", ui->values[GUA76_GAIN_REDUCTION_METER], -30.0f, 0.0f, ImVec2(50, 150));


            // Second Row (under Attack/Release/Ratio)
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();


            // Release (Manopola a Scatti)
            ImGui::BeginGroup();
            ImGui::Text("Release");
            int current_release_idx = get_nearest_discrete_value_idx(ui->values[GUA76_RELEASE], ui->release_values, IM_ARRAYSIZE(ui->release_values));
            ImGui::PushItemWidth(100);
            if (ImGui::SliderInt("##Release", &current_release_idx, 0, IM_ARRAYSIZE(ui->release_values) - 1, ui->release_labels[current_release_idx])) {
                ui->values[GUA76_RELEASE] = ui->release_values[current_release_idx];
                ui->write_function(ui->controller, GUA76_RELEASE, sizeof(float), 0, &ui->values[GUA76_RELEASE]);
            }
            ImGui::PopItemWidth();
            ImGui::EndGroup();

            ImGui::SameLine(); ImGui::Dummy(ImVec2(20,0)); ImGui::SameLine();

            // Clip Drive
            ImGui::PushItemWidth(150);
            ImGui::SliderFloat("Input Clip Drive", &ui->values[GUA76_INPUT_CLIP_DRIVE], 0.0f, 10.0f, "%.1f");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                ui->write_function(ui->controller, GUA76_INPUT_CLIP_DRIVE, sizeof(float), 0, &ui->values[GUA76_INPUT_CLIP_DRIVE]);
            }
            ImGui::SameLine();
            ImGui::SliderFloat("Output Clip Drive", &ui->values[GUA76_OUTPUT_CLIP_DRIVE], 0.0f, 10.0f, "%.1f");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                ui->write_function(ui->controller, GUA76_OUTPUT_CLIP_DRIVE, sizeof(float), 0, &ui->values[GUA76_OUTPUT_CLIP_DRIVE]);
            }
            ImGui::PopItemWidth();


            // Normalize Output Button
            ImGui::SameLine(); ImGui::Dummy(ImVec2(20,0)); ImGui::SameLine();
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 5.0f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
            if (ImGui::Button((ui->values[GUA76_NORMALIZE_OUTPUT] > 0.5f) ? "NORMALIZE ON" : "NORMALIZE OFF", ImVec2(140, 30))) {
                ui->values[GUA76_NORMALIZE_OUTPUT] = (ui->values[GUA76_NORMALIZE_OUTPUT] > 0.5f) ? 0.0f : 1.0f;
                ui->write_function(ui->controller, GUA76_NORMALIZE_OUTPUT, sizeof(float), 0, &ui->values[GUA76_NORMALIZE_OUTPUT]);
            }
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);


            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Sidechain Tab")) {
            current_tab = 1;
            // --- SIDECHAIN TAB CONTENT ---
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 5.0f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));

            if (ImGui::Button((ui->values[GUA76_EXTERNAL_SC_ACTIVE] > 0.5f) ? "EXTERNAL SC ON" : "EXTERNAL SC OFF", ImVec2(150, 30))) {
                ui->values[GUA76_EXTERNAL_SC_ACTIVE] = (ui->values[GUA76_EXTERNAL_SC_ACTIVE] > 0.5f) ? 0.0f : 1.0f;
                ui->write_function(ui->controller, GUA76_EXTERNAL_SC_ACTIVE, sizeof(float), 0, &ui->values[GUA76_EXTERNAL_SC_ACTIVE]);
            }
            ImGui::SameLine(0.0f, 20.0f);

            if (ImGui::Button((ui->values[GUA76_MS_MODE_ACTIVE] > 0.5f) ? "M/S MODE ON" : "M/S MODE OFF", ImVec2(150, 30))) {
                ui->values[GUA76_MS_MODE_ACTIVE] = (ui->values[GUA76_MS_MODE_ACTIVE] > 0.5f) ? 0.0f : 1.0f;
                ui->write_function(ui->controller, GUA76_MS_MODE_ACTIVE, sizeof(float), 0, &ui->values[GUA76_MS_MODE_ACTIVE]);
            }

            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Sidechain Filters
            ImGui::PushItemWidth(150);
            ImGui::SliderFloat("SC HPF Freq (Hz)", &ui->values[GUA76_SC_HPF_FREQ], 20.0f, 20000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                ui->write_function(ui->controller, GUA76_SC_HPF_FREQ, sizeof(float), 0, &ui->values[GUA76_SC_HPF_FREQ]);
            }
            ImGui::SameLine();
            ImGui::SliderFloat("SC HPF Q", &ui->values[GUA76_SC_HPF_Q], 0.1f, 10.0f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                ui->write_function(ui->controller, GUA76_SC_HPF_Q, sizeof(float), 0, &ui->values[GUA76_SC_HPF_Q]);
            }
            ImGui::Spacing();

            ImGui::SliderFloat("SC LPF Freq (Hz)", &ui->values[GUA76_SC_LPF_FREQ], 20.0f, 20000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                ui->write_function(ui->controller, GUA76_SC_LPF_FREQ, sizeof(float), 0, &ui->values[GUA76_SC_LPF_FREQ]);
            }
            ImGui::SameLine();
            ImGui::SliderFloat("SC LPF Q", &ui->values[GUA76_SC_LPF_Q], 0.1f, 10.0f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                ui->write_function(ui->controller, GUA76_SC_LPF_Q, sizeof(float), 0, &ui->values[GUA76_SC_LPF_Q]);
            }
            ImGui::PopItemWidth();

            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }


    // --- Fine della GUI ImGui ---
    ImGui::End();

    // Rendering ImGui
    ImGui::Render();
    int framebuffer_width, framebuffer_height;
    glfwGetFramebufferSize(ui->window, &framebuffer_width, &framebuffer_height);
    glViewport(0, 0, framebuffer_width, framebuffer_height);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f); // Colore di sfondo del plugin
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Se stiamo usando viewports (finestre ImGui native separate)
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* backup_current_context = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup_current_context);
    }

    glfwSwapBuffers(ui->window);

    // Controlla se la finestra GLFW è stata chiusa dall'utente
    if (glfwWindowShouldClose(ui->window)) {
        return 1; // Segnala all'host LV2 di chiudere la GUI
    }

    // Processa gli eventi GLFW
    glfwPollEvents();

    return 0; // La GUI deve rimanere aperta
}

// Funzione di pulizia della GUI
static void
cleanup(LV2_UI_Handle handle) {
    Gua76UI* ui = (Gua76UI*)handle;

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(ui->window);
    glfwTerminate();

    free(ui);
}

// Descrizione della GUI per LV2
static const LV2_UI_Descriptor ui_descriptor = {
    GUA76_GUI_URI,
    instantiate,
    cleanup,
    port_event,
    ui_idle, // Utilizza ui_idle per il rendering continuo
    NULL // no extension_data
};

LV2_SYMBOL_EXPORT
const LV2_UI_Descriptor*
lv2_ui_descriptor(uint32_t index) {
    if (index == 0) {
        return &ui_descriptor;
    }
    return NULL;
}
