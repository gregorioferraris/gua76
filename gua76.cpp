#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/atom.h> // Incluso due volte? No, solo una volta.
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>

// Standard C++ per X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <cairo.h>       // Per disegnare
#include <cairo-xlib.h>  // Integrazione Cairo con X11

#include <stdio.h> // Per debug
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Definizione URI del plugin e della GUI (Devono corrispondere al .ttl)
#define GUA76_GUI_URI   "http://moddevices.com/plugins/mod-devel/gua76_ui"
#define GUA76_PLUGIN_URI "http://moddevices.com/plugins/mod-devel/gua76"

// Enum degli indici delle porte (devono corrispondere a gua76.ttl)
// È cruciale che questi siano ESATTAMENTE gli stessi del file gua76.ttl
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
    // Le porte audio non sono gestite direttamente dalla GUI, quindi non le includiamo qui
    // ma le posizioni degli indici DEVONO COMUNQUE ALLINEARSI CON IL CORE DEL PLUGIN
} GUA76_PortIndex;


// Struct per il nostro stato della GUI
typedef struct {
    LV2_URID_Map* map;
    LV2_URID_Unmap* unmap;
    LV2_Atom_Forge   forge;
    LV2_UI_Write_Function write_function; // Funzione per scrivere valori al plugin
    LV2_UI_Controller    controller;

    // Connessioni X11 per il drawing con Cairo
    Display* display;
    Window         window;
    cairo_surface_t* surface;
    cairo_t* cr;
    Atom           atom_delete_window; // Per chiudere la finestra

    // Valori attuali dei parametri del plugin (cache)
    float values[14]; // Ci sono 14 parametri di controllo (da 0 a 13)

    // Per la gestione dei controlli: potresti avere strutture per manopole, pulsanti, ecc.
    // Ogni controllo avrebbe la sua posizione, dimensione, stato, e l'indice della porta LV2 associata.
    // Esempio:
    struct {
        float x, y, radius;
        float angle; // Per manopole
        int port_index;
        bool is_dragging;
        double drag_start_y; // Per gestire il drag verticale
    } knobs[10]; // Esempio: un array di manopole
    
    struct {
        float x, y, width, height;
        bool is_on;
        int port_index;
    } buttons[10]; // Esempio: un array di pulsanti per i toggle e ratio

    // Variabile per tenere traccia della manopola/pulsante attivo per il drag/click
    int active_control_idx; // Indice del controllo che l'utente sta interagendo con
    int active_control_type; // 0=none, 1=knob, 2=button

    // Per il rendering del testo
    cairo_font_face_t* font_face;

} Gua76UI;

// Funzioni di utilità per disegnare (es. una manopola, un pulsante)
// Questo è dove il tuo talento artistico e le conoscenze di Cairo entrano in gioco.

static void draw_knob(cairo_t* cr, float x, float y, float radius, float value, float min_val, float max_val, const char* label) {
    cairo_save(cr);
    cairo_translate(cr, x, y);

    // Disegna lo sfondo della manopola
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2); // Grigio scuro
    cairo_arc(cr, 0, 0, radius, 0, 2 * M_PI);
    cairo_fill(cr);

    // Disegna il "pizzico" o indicatore
    cairo_set_source_rgb(cr, 0.8, 0.8, 0.8); // Grigio chiaro
    float angle = (value - min_val) / (max_val - min_val) * (0.75 * 2 * M_PI) + (0.625 * 2 * M_PI); // Mappa il valore a un angolo
    cairo_set_line_width(cr, 2);
    cairo_arc(cr, 0, 0, radius * 0.8, angle - 0.1, angle + 0.1);
    cairo_line_to(cr, radius * 0.5 * cos(angle), radius * 0.5 * sin(angle));
    cairo_close_path(cr);
    cairo_fill(cr);
    
    // Disegna il label (sotto la manopola)
    cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    cairo_text_extents_t te;
    cairo_text_extents(cr, label, &te);
    cairo_move_to(cr, -te.width / 2, radius + te.height + 5);
    cairo_show_text(cr, label);

    cairo_restore(cr);
}

static void draw_toggle_button(cairo_t* cr, float x, float y, float w, float h, bool is_on, const char* label) {
    cairo_save(cr);
    cairo_translate(cr, x, y);

    // Disegna lo sfondo del pulsante
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3); // Grigio scuro
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);

    // Disegna il "toggle" o indicatore dello stato
    if (is_on) {
        cairo_set_source_rgb(cr, 0.0, 0.8, 0.0); // Verde accesso
    } else {
        cairo_set_source_rgb(cr, 0.8, 0.0, 0.0); // Rosso spento
    }
    cairo_rectangle(cr, w * 0.1, h * 0.1, w * 0.8, h * 0.8);
    cairo_fill(cr);

    // Disegna il label
    cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    cairo_text_extents_t te;
    cairo_text_extents(cr, label, &te);
    cairo_move_to(cr, (w - te.width) / 2, h + te.height + 5);
    cairo_show_text(cr, label);

    cairo_restore(cr);
}


// Funzione di disegno principale della GUI
static void draw(Gua76UI* ui) {
    cairo_set_source_rgb(ui->cr, 0.1, 0.1, 0.1); // Colore di sfondo della GUI
    cairo_paint(ui->cr);

    // Esempio di disegno di una manopola per Input Gain
    // Indice 0 in values[] corrisponde a GUA76_INPUT_GAIN
    draw_knob(ui->cr, 50, 50, 30, ui->values[GUA76_INPUT_GAIN], -20.0f, 20.0f, "Input Gain");

    // Esempio di disegno di un pulsante per Bypass
    // Indice 3 in values[] corrisponde a GUA76_BYPASS
    draw_toggle_button(ui->cr, 150, 50, 40, 20, ui->values[GUA76_BYPASS] > 0.5f, "Bypass");

    // TODO: Disegna TUTTI gli altri controlli (Output Gain, Pad, Normalize, MS Mode, External SC, Attack, Release, Ratio, Clip Drive, HPF, LPF)
    // Usando le posizioni e le dimensioni appropriate.
    // Ricorda di accedere ai valori attuali tramite ui->values[GUA76_PORT_INDEX].

    // Forza il refresh della finestra
    cairo_surface_flush(ui->surface);
    XFlush(ui->display);
}

// Funzione per inviare un valore al plugin core
static void send_value(Gua76UI* ui, uint32_t port_index, float value) {
    ui->write_function(ui->controller, port_index, sizeof(float), 0, &value);
}

// Handler degli eventi X11 (mouse, tastiera)
static int handle_event(Gua76UI* ui, XEvent* event) {
    switch (event->type) {
        case Expose:
            // Ridisegna l'intera finestra quando è esposta
            draw(ui);
            break;
        case ButtonPress: {
            // Gestione click del mouse
            XButtonEvent* bev = (XButtonEvent*)event;
            // TODO: Controlla quale manopola/pulsante è stato cliccato in base alle coordinate bev->x, bev->y
            // Se una manopola, imposta ui->active_control_idx e ui->active_control_type = 1
            // Se un pulsante, imposta ui->active_control_idx e ui->active_control_type = 2
            // Esempio per il Bypass button:
            if (bev->x >= 150 && bev->x <= 150+40 && bev->y >= 50 && bev->y <= 50+20) {
                ui->values[GUA76_BYPASS] = (ui->values[GUA76_BYPASS] > 0.5f) ? 0.0f : 1.0f; // Toggle stato
                send_value(ui, GUA76_BYPASS, ui->values[GUA76_BYPASS]);
                draw(ui); // Ridisegna per mostrare il cambio di stato
            }
            // Esempio per Input Gain knob (molto semplificato, senza drag)
            float knob_x = 50, knob_y = 50, knob_r = 30;
            if (pow(bev->x - knob_x, 2) + pow(bev->y - knob_y, 2) <= pow(knob_r, 2)) {
                 ui->active_control_idx = GUA76_INPUT_GAIN;
                 ui->active_control_type = 1; // Knob
                 ui->knobs[0].is_dragging = true; // Assumi che knobs[0] sia l'input gain
                 ui->knobs[0].drag_start_y = bev->y;
            }

            break;
        }
        case ButtonRelease: {
            // Rilascia il controllo attivo
            // Resetta ui->active_control_idx e ui->active_control_type = 0
            if (ui->knobs[0].is_dragging) {
                ui->knobs[0].is_dragging = false;
            }
            ui->active_control_idx = -1;
            ui->active_control_type = 0;
            break;
        }
        case MotionNotify: {
            // Gestione del movimento del mouse (per i drag delle manopole)
            XPointerEvent* pev = (XPointerEvent*)event;
            if (ui->active_control_type == 1 && ui->knobs[0].is_dragging) { // Se stiamo trascinando una manopola (es. input gain)
                float delta_y = pev->y - ui->knobs[0].drag_start_y;
                float sensitivity = 0.1f; // Regola la sensibilità del drag

                // Aggiorna il valore in base al movimento verticale
                // Range per Input Gain è -20 a +20
                float current_val = ui->values[ui->active_control_idx];
                float new_val = current_val - (delta_y * sensitivity); // Spostamento Y influisce sul valore

                // Clampa il valore nel range [-20, 20]
                if (new_val < -20.0f) new_val = -20.0f;
                if (new_val > 20.0f) new_val = 20.0f;
                
                ui->values[ui->active_control_idx] = new_val;
                send_value(ui, ui->active_control_idx, new_val);
                
                ui->knobs[0].drag_start_y = pev->y; // Aggiorna la posizione di partenza
                draw(ui); // Ridisegna per mostrare il nuovo valore
            }
            break;
        }
        case ClientMessage:
            // Gestione della chiusura della finestra
            if (event->xclient.data.l[0] == ui->atom_delete_window) {
                return 1; // Indica che la GUI deve chiudersi
            }
            break;
    }
    return 0; // Indica che la GUI deve rimanere aperta
}

// Funzione di creazione della GUI
static LV2_UI_Handle
instantiate(const LV2_UI_Descriptor* descriptor,
            const char* plugin_uri,
            const char* bundle_path,
            LV2_UI_Write_Function      write_function,
            LV2_UI_Controller          controller,
            LV2_UI_Widget* widget,
            const LV2_Feature* const* features) {

    // Controlla che l'URI del plugin corrisponda
    if (strcmp(plugin_uri, GUA76_PLUGIN_URI) != 0) {
        fprintf(stderr, "Gua76UI: Plugin URI mismatch.\n");
        return NULL;
    }

    Gua76UI* ui = (Gua76UI*)calloc(1, sizeof(Gua76UI));
    if (!ui) return NULL;

    ui->write_function = write_function;
    ui->controller = controller;

    // Cerca le feature necessarie
    for (int i = 0; features[i]; ++i) {
        if (strcmp(features[i]->URI, LV2_URID__map) == 0) {
            ui->map = (LV2_URID_Map*)features[i]->data;
        } else if (strcmp(features[i]->URI, LV2_URID__unmap) == 0) {
            ui->unmap = (LV2_URID_Unmap*)features[i]->data;
        }
    }

    if (!ui->map) {
        fprintf(stderr, "Gua76UI: Host does not support urid:map.\n");
        free(ui);
        return NULL;
    }

    // Inizializza i valori dei parametri ai loro default (o 0)
    for (int i = 0; i < 14; ++i) { // 14 è il numero di parametri di controllo nel .ttl
        ui->values[i] = 0.0f; 
    }
    // Puoi impostare i default qui o lasciarli 0 e poi aggiornarli via port_event
    ui->values[GUA76_INPUT_GAIN] = 0.0f;
    ui->values[GUA76_OUTPUT_GAIN] = 0.0f;
    ui->values[GUA76_INPUT_PAD_10DB] = 0.0f;
    ui->values[GUA76_BYPASS] = 0.0f;
    ui->values[GUA76_NORMALIZE_OUTPUT] = 0.0f;
    ui->values[GUA76_MS_MODE_ACTIVE] = 1.0f; // Default ON
    ui->values[GUA76_EXTERNAL_SC_ACTIVE] = 0.0f;
    ui->values[GUA76_ATTACK] = 0.000020f;
    ui->values[GUA76_RELEASE] = 0.2f;
    ui->values[GUA76_RATIO] = 4.0f;
    ui->values[GUA76_INPUT_CLIP_DRIVE] = 1.0f;
    ui->values[GUA76_OUTPUT_CLIP_DRIVE] = 1.0f;
    ui->values[GUA76_SC_HPF_FREQ] = 20.0f;
    ui->values[GUA76_SC_LPF_FREQ] = 20000.0f;


    // --- Inizializzazione X11 e Cairo ---
    ui->display = XOpenDisplay(NULL);
    if (!ui->display) {
        fprintf(stderr, "Gua76UI: Cannot open X display.\n");
        free(ui);
        return NULL;
    }

    int screen = DefaultScreen(ui->display);
    Window root_window = RootWindow(ui->display, screen);

    // Creare la finestra (dimensioni fisse per ora)
    int width = 400; // Esempio
    int height = 300; // Esempio
    ui->window = XCreateSimpleWindow(
        ui->display, root_window,
        0, 0, width, height,
        0, BlackPixel(ui->display, screen), WhitePixel(ui->display, screen)
    );

    // Selezione degli eventi che vogliamo ricevere
    XSelectInput(ui->display, ui->window,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask);

    // Gestione della chiusura della finestra con il tasto X
    ui->atom_delete_window = XInternAtom(ui->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(ui->display, ui->window, &ui->atom_delete_window, 1);

    XMapWindow(ui->display, ui->window); // Mostra la finestra

    // Inizializza Cairo
    ui->surface = cairo_xlib_surface_create(ui->display, ui->window,
                                            DefaultVisual(ui->display, screen), width, height);
    ui->cr = cairo_create(ui->surface);

    // Associa il widget LV2 con la nostra finestra X11
    *widget = (LV2_UI_Widget)ui->window;

    ui->active_control_idx = -1; // Nessun controllo attivo inizialmente
    ui->active_control_type = 0;

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
        draw(ui); // Ridisegna la GUI per riflettere il nuovo valore
    }
}

// Funzione per il ciclo di esecuzione della GUI (se non c'è un main loop X11)
static int
ui_idle(LV2_UI_Handle handle) {
    Gua76UI* ui = (Gua76UI*)handle;
    XEvent event;

    // Processa tutti gli eventi X11 in coda
    while (XPending(ui->display)) {
        XNextEvent(ui->display, &event);
        if (handle_event(ui, &event)) {
            return 1; // Chiede la chiusura della GUI
        }
    }
    return 0; // La GUI deve rimanere aperta
}

// Funzione di pulizia della GUI
static void
cleanup(LV2_UI_Handle handle) {
    Gua76UI* ui = (Gua76UI*)handle;

    cairo_destroy(ui->cr);
    cairo_surface_destroy(ui->surface);
    XDestroyWindow(ui->display, ui->window);
    XCloseDisplay(ui->display);
    free(ui);
}

// Descrizione della GUI per LV2
static const LV2_UI_Descriptor ui_descriptor = {
    GUA76_GUI_URI,
    instantiate,
    cleanup,
    port_event,
    ui_idle, // O ui_idle, a seconda del ciclo di eventi preferito
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
