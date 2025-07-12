# Makefile per il plugin LV2 Gua76 Compressor

# Nome del plugin e della directory del bundle LV2
BUNDLE_NAME = gua76.lv2
PLUGIN_URI = http://your-plugin.com/plugins/gua76 # Assicurati che questo corrisponda a gua76.h e manifest.ttl

# Directory di destinazione per l'installazione
# Per gli utenti: ~/.lv2
# Per il sistema: /usr/local/lib/lv2 o /usr/lib/lv2
# Puoi cambiare questa variabile o sovrascriverla da riga di comando (es. make install LV2_PATH=/usr/local/lib/lv2)
LV2_PATH ?= $(HOME)/.lv2

# Compilatore C++ (puoi usare g++ o clang++)
CXX = g++
# C Compiler (for potential C files, not strictly needed for this example)
CC = gcc

# Flag di compilazione
# -Wall: Abilita tutti gli avvisi
# -Wextra: Abilita avvisi extra
# -fPIC: Genera codice indipendente dalla posizione (necessario per librerie condivise)
# -O2: Ottimizzazione di livello 2
# -std=c++11: Standard C++11 (o c++14/c++17 a seconda delle tue esigenze)
# -D_POSIX_C_SOURCE=200112L: Per alcune definizioni POSIX (es. per math.h)
CXXFLAGS = -Wall -Wextra -fPIC -O2 -std=c++11 -D_POSIX_C_SOURCE=200112L
CFLAGS = $(CXXFLAGS) # Stessi flag per C

# Flag di linking
# -shared: Crea una libreria condivisa
# -lm: Linka la libreria matematica
# $(shell pkg-config --libs lv2) : Linka le librerie LV2 tramite pkg-config
# -lX11 -lcairo: Linka le librerie X11 e Cairo per la GUI (se usi X11+Cairo)
LDFLAGS = -shared -lm $(shell pkg-config --libs lv2)

# Include directories
# $(shell pkg-config --cflags lv2) : Include le directory di LV2 tramite pkg-config
# -I. : Include la directory corrente per i file .h
# -I/usr/include/cairo -I/usr/include/X11: Per X11 e Cairo (se usi X11+Cairo)
CXXFLAGS += $(shell pkg-config --cflags lv2) -I.
CFLAGS += $(shell pkg-config --cflags lv2) -I.

# Sorgenti del plugin audio
AUDIO_SRC = gua76.cpp
# Oggetti del plugin audio
AUDIO_OBJ = $(AUDIO_SRC:.cpp=.o)
# Libreria condivisa del plugin audio
AUDIO_LIB = gua76.so

# Sorgenti della GUI (Assumendo una GUI X11 in C++)
GUI_SRC = gua76_gui.cpp
# Oggetti della GUI
GUI_OBJ = $(GUI_SRC:.cpp=.o)
# Libreria condivisa della GUI
GUI_LIB = gua76_gui.so

# Dipendenze della GUI (se usi X11/Cairo)
GUI_LDFLAGS = $(LDFLAGS) -lX11 -lcairo
GUI_CXXFLAGS = $(CXXFLAGS) $(shell pkg-config --cflags cairo xcb) # Aggiungi cflags per Cairo/XCB se necessarie

# Tutti i target
.PHONY: all clean install uninstall

all: $(AUDIO_LIB) $(GUI_LIB)

# Regola per compilare il plugin audio
$(AUDIO_LIB): $(AUDIO_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $(AUDIO_OBJ)

# Regola per compilare i file .cpp in .o per il plugin audio
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Regola per compilare la GUI
$(GUI_LIB): $(GUI_OBJ)
	$(CXX) $(GUI_LDFLAGS) -o $@ $(GUI_OBJ)

# Regola per compilare i file .cpp in .o per la GUI
%.o: %.cpp
	$(CXX) $(GUI_CXXFLAGS) -c $< -o $@

# Installazione del plugin
install: all
	@echo "Installing $(BUNDLE_NAME) to $(LV2_PATH)..."
	mkdir -p $(LV2_PATH)/$(BUNDLE_NAME)
	cp $(AUDIO_LIB) $(LV2_PATH)/$(BUNDLE_NAME)
	cp $(GUI_LIB) $(LV2_PATH)/$(BUNDLE_NAME)
	cp manifest.ttl $(LV2_PATH)/$(BUNDLE_NAME)/gua76.ttl # Copia il manifest
	@echo "Installation complete."

# Disinstallazione del plugin
uninstall:
	@echo "Uninstalling $(BUNDLE_NAME) from $(LV2_PATH)..."
	rm -rf $(LV2_PATH)/$(BUNDLE_NAME)
	@echo "Uninstallation complete."

# Pulizia dei file generati
clean:
	@echo "Cleaning up..."
	rm -f $(AUDIO_OBJ) $(AUDIO_LIB) $(GUI_OBJ) $(GUI_LIB)
	@echo "Clean complete."
