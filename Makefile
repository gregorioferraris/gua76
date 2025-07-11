# Makefile per il plugin LV2 gua76

# --- CONFIGURAZIONE ---

# Nome del plugin. Deve corrispondere al nome della cartella LV2 e al prefisso dei file .cpp, .ttl, .so
PLUGIN_NAME = gua76

# Directory di installazione dei plugin LV2
# Per installazioni personali: $(HOME)/.lv2/
# Per installazioni a livello di sistema: /usr/local/lib/lv2/ (o /usr/lib/lv2/)
INSTALL_DIR = $(HOME)/.lv2/

# Flag del compilatore (ottimizzazioni, avvisi, C++11)
CXXFLAGS = -O2 -Wall -fPIC -std=c++11

# Flag del linker (librerie LV2, math, X11 per la GUI)
LDFLAGS = -lm -ldl $(shell pkg-config --libs lv2) $(shell pkg-config --libs xcb xcb-util xcb-image xcb-render xcb-shm xcb-randr)

# Headers e librerie per LV2 e X11 (necessari per la GUI)
# Usa pkg-config per trovare i percorsi corretti
CXXFLAGS += $(shell pkg-config --cflags lv2) $(shell pkg-config --cflags xcb xcb-util xcb-image xcb-render xcb-shm xcb-randr)

# Sorgenti del plugin core
PLUGIN_SRC = $(PLUGIN_NAME).cpp

# Sorgenti della GUI
GUI_SRC = gui/$(PLUGIN_NAME)_gui.cpp

# Oggetti compilati
PLUGIN_OBJ = $(PLUGIN_SRC:.cpp=.o)
GUI_OBJ = $(GUI_SRC:.cpp=.o)

# Nomi dei file di output
PLUGIN_SO = $(PLUGIN_NAME).so
GUI_SO = gui/$(PLUGIN_NAME)_gui.so

# Cartella di destinazione del plugin
DEST_DIR = $(INSTALL_DIR)/$(PLUGIN_NAME).lv2

# ------------------------------------
# REGOLE DI COMPILAZIONE
# ------------------------------------

.PHONY: all clean install uninstall

all: $(DEST_DIR) $(DEST_DIR)/$(PLUGIN_SO) $(DEST_DIR)/$(PLUGIN_NAME).ttl $(DEST_DIR)/$(GUI_SO)

# Crea la directory di destinazione se non esiste
$(DEST_DIR):
	@mkdir -p $(DEST_DIR)/gui

# Compila il core del plugin
$(DEST_DIR)/$(PLUGIN_SO): $(PLUGIN_OBJ)
	@echo "Linking $(PLUGIN_SO)..."
	$(CXX) $(CXXFLAGS) -shared -o $@ $(PLUGIN_OBJ) $(LDFLAGS)

# Copia il file TTL (descrizione del plugin)
$(DEST_DIR)/$(PLUGIN_NAME).ttl: $(PLUGIN_NAME).ttl
	@echo "Copying $(PLUGIN_NAME).ttl..."
	@cp $< $@

# Compila la GUI
$(DEST_DIR)/$(GUI_SO): $(GUI_OBJ)
	@echo "Linking $(GUI_SO)..."
	$(CXX) $(CXXFLAGS) -shared -o $@ $(GUI_OBJ) $(LDFLAGS)

# Regole per la compilazione dei file .cpp in .o
%.o: %.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) -c $< -o $@

gui/%.o: gui/%.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Regola per pulire i file compilati e la directory di installazione
clean:
	@echo "Cleaning up..."
	@rm -f $(PLUGIN_OBJ) $(GUI_OBJ) $(PLUGIN_SO) $(GUI_SO)
	@rm -rf $(DEST_DIR)

# Regola per installare il plugin
install: all
	@echo "Plugin $(PLUGIN_NAME) installed to $(DEST_DIR)"

# Regola per disinstallare il plugin
uninstall:
	@echo "Uninstalling $(PLUGIN_NAME)..."
	@rm -rf $(DEST_DIR)
	@echo "Plugin $(PLUGIN_NAME) uninstalled from $(INSTALL_DIR)"
