#ifndef FALLOUT_GAMEPAD_H_
#define FALLOUT_GAMEPAD_H_

#include <SDL.h>

namespace fallout {

// Controlli mobile: controller (SDL_GameController) e tocco diretto.
//
// Il controller lavora su due livelli:
//  - basso livello (eventi SDL, cursore, tasti simulati): funziona ovunque,
//    menu, dialoghi, mappa del mondo, creazione personaggio;
//  - livello di gioco (movimento diretto del personaggio con lo stick,
//    aggancio dei bersagli, visuale che segue): solo sulla mappa locale,
//    chiamato da gameHandleKey sia in esplorazione sia in combattimento.

void gamepadHandleEvent(SDL_Event* event);

// Contributo del controller al mouse per questo frame. Somma lo spostamento
// del cursore, i pulsanti tenuti premuti e la rotella.
void gamepadGetMouseInput(int* dx, int* dy, int* buttons, int* wheelX, int* wheelY);

// Logica di gioco (mappa locale). Da chiamare una volta per iterazione dei
// cicli principali (esplorazione e combattimento).
void gamepadGameTick(bool isInCombatMode);

// Impostazioni [mobile] di fallout.cfg.
bool touchDirectModeEnabled();
bool touchSnapEnabled();

// Aggancio del tocco: se (x, y) cade vicino a un oggetto interattivo della
// mappa sposta il punto al centro dell'oggetto. Restituisce true se ha
// agganciato.
bool gamepadSnapPoint(int* x, int* y);

// Clic intelligente (vedi gamepad.cc). false = non cliccare, è cambiato solo
// il cursore e serve un secondo tocco per confermare.
bool gamepadSmartClick(int x, int y);

} // namespace fallout

#endif /* FALLOUT_GAMEPAD_H_ */
