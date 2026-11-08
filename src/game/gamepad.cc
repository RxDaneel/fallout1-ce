#include "game/gamepad.h"

#include <climits>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#include <algorithm>
#include <vector>

#include "actions.h"
#include "game/actions.h"
#include "game/anim.h"
#include "game/combat.h"
#include "game/config.h"
#include "game/critter.h"
#include "game/display.h"
#include "game/game.h"
#include "game/gconfig.h"
#include "game/gdialog.h"
#include "game/editor.h"
#include "game/gmouse.h"
#include "game/intface.h"
#include "game/loadsave.h"
#include "game/map.h"
#include "game/object.h"
#include "game/party.h"
#include "game/proto.h"
#include "game/protinst.h"
#include "game/skill_defs.h"
#include "game/tile.h"
#include "game/worldmap.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/input.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/svga.h"

namespace fallout {

static const double kPi = 3.14159265358979323846;

// -----------------------------------------------------------------------------
// Impostazioni
// -----------------------------------------------------------------------------

struct MobileSettings {
    bool loaded = false;
    bool touchDirect = true; // tocco: il cursore va dove tocchi
    bool touchSnap = true; // tocco: aggancia gli oggetti vicini
    int cursorSpeed = 100; // velocità cursore del controller, in %
    int deadzone = 20; // zona morta degli stick, in %
    bool cameraFollow = true; // la visuale segue il personaggio
    bool stickMovement = true; // stick sinistro = muovi il personaggio
};

static MobileSettings gMobile;

static void mobileSettingsLoad()
{
    if (gMobile.loaded || !gconfig_is_initialized()) {
        return;
    }

    gMobile.loaded = true;

    int value;
    if (config_get_value(&game_config, "mobile", "touch_mode", &value)) gMobile.touchDirect = value != 0;
    if (config_get_value(&game_config, "mobile", "touch_snap", &value)) gMobile.touchSnap = value != 0;
    if (config_get_value(&game_config, "mobile", "gamepad_cursor_speed", &value)) gMobile.cursorSpeed = std::clamp(value, 25, 400);
    if (config_get_value(&game_config, "mobile", "gamepad_deadzone", &value)) gMobile.deadzone = std::clamp(value, 5, 60);
    if (config_get_value(&game_config, "mobile", "camera_follow", &value)) gMobile.cameraFollow = value != 0;
    if (config_get_value(&game_config, "mobile", "stick_movement", &value)) gMobile.stickMovement = value != 0;
}

bool touchDirectModeEnabled()
{
    mobileSettingsLoad();
    return gMobile.touchDirect;
}

bool touchSnapEnabled()
{
    mobileSettingsLoad();
    return gMobile.touchSnap;
}

// -----------------------------------------------------------------------------
// Stato del controller
// -----------------------------------------------------------------------------

enum Action {
    ACTION_NONE,
    ACTION_KEY, // tasto tenuto finché il pulsante è premuto
    ACTION_KEY_TAP, // pressione + rilascio immediati
    ACTION_MOUSE_LEFT,
    ACTION_MOUSE_RIGHT,
    ACTION_TARGET_PREV,
    ACTION_TARGET_NEXT,
    ACTION_CANCEL_MODE,
    ACTION_DIALOG_PREV,
    ACTION_DIALOG_NEXT,
    ACTION_PAD_USE, // A senza puntatore: usa l'oggetto evidenziato
    ACTION_PAD_LOOK, // X senza puntatore: esamina l'oggetto evidenziato
    ACTION_NAV_UP, // menu: salta al pulsante sopra / sotto / a sinistra / a destra
    ACTION_NAV_DOWN,
    ACTION_NAV_LEFT,
    ACTION_NAV_RIGHT,
    ACTION_WHEEL_UP, // menu: scorri la lista
    ACTION_WHEEL_DOWN,
};

struct Binding {
    Action action = ACTION_NONE;
    int scancode = 0;
};

enum Context {
    CONTEXT_MAP,
    CONTEXT_COMBAT,
    CONTEXT_DIALOG,
    CONTEXT_WINDOW,
};

static SDL_GameController* gController = nullptr;
static SDL_JoystickID gControllerId = -1;
static bool gControllerJustConnected = false;

static Sint16 gAxis[SDL_CONTROLLER_AXIS_MAX];
static Binding gActiveBinding[SDL_CONTROLLER_BUTTON_MAX];

static bool gMouseLeftHeld = false;
static bool gMouseRightHeld = false;
static int gMouseRightPulse = 0;
static int gMouseLeftPulse = 0;

// Spostamento del cursore richiesto da un aggancio (dialogo, bersagli).
static bool gWarpPending = false;
static int gWarpX = 0;
static int gWarpY = 0;

// Azioni di gioco richieste dagli eventi e svolte in gamepadGameTick.
static int gPendingTargetStep = 0;
static bool gPendingCancelMode = false;

// Pulsante Select usato come modificatore.
static bool gSelectHeld = false;

static double gCursorRemainderX = 0.0;
static double gCursorRemainderY = 0.0;
static Uint32 gLastMouseTick = 0;
static Uint32 gLastWheelTick = 0;

static bool gStickWasMoving = false;
static int gStickLastTarget = -1;
static Uint32 gStickLastIssue = 0;
static double gStickLastAngle = 0.0;
static bool gStickRunning = false;

static Object* gTargetCurrent = nullptr;

// Modalità senza puntatore (controller sulla mappa).
static bool gPadMode = false; // ultimo input dal controller
static Uint32 gLastPadInput = 0;
static Object* gFocus = nullptr; // oggetto evidenziato
static bool gFocusManual = false; // scelto con LB/RB: resta finché non ti muovi
static bool gFocusOutlined = false;
static bool gWeHidCursor = false;
static bool gWeHidMapCursor = false;
static Uint32 gFocusNameDue = 0; // quando scrivere il nome dell'oggetto evidenziato // cursore "3D" della mappa (freccia + icona azione)
static bool gUseHolding = false; // A tenuto premuto
static Uint32 gUseHeldSince = 0;
static bool gMenuHold = false; // menu azioni del gioco aperto con A tenuto
static int gMenuStep = 0; // spostamenti richiesti nel menu azioni
static bool gPendingUse = false;
static bool gPendingLook = false;
static double gFacingX = 0.0; // direzione in cui "guarda" il personaggio
static double gFacingY = 1.0;
static Uint32 gLastScrollTick = 0;

// Combattimento: esagono scelto con lo stick.
static int gCombatHex = -1;
static int gCombatHexOrigin = -1;
static Uint32 gCombatHexNext = 0;
static int gCombatHexLastDir = -1;
static bool gCombatStickActive = false;

// Navigazione nei menu.
static int gNavDirX = 0; // direzione tenuta premuta (croce)
static int gNavDirY = 0;
static int gNavPendingX = 0; // salto da fare al prossimo frame
static int gNavPendingY = 0;
static Uint32 gNavNextRepeat = 0;
static int gNavWin = -1; // pulsante selezionato
static int gNavBtn = -1;
static int gWheelPending = 0;
static int gLastContext = -1;
static Uint32 gNavAutoSelectAt = 0;
static int gNavStickDirX = 0;
static int gNavStickDirY = 0;
static Uint32 gNavStickNext = 0;

static Uint32 gLastGameTick = 0;

// Siamo nel ciclo della mappa locale (esplorazione o combattimento)?
// gamepadGameTick viene chiamato a ogni giro di quel ciclo: se non arriva da
// un po' vuol dire che è aperto un menu (inventario, Pip-Boy, menu principale).
static bool inGameLoop()
{
    return gLastGameTick != 0 && SDL_GetTicks() - gLastGameTick < 200;
}

static bool mapIsActive()
{
    return obj_dude != nullptr && gmouse_is_enabled() && !dialog_active() && inGameLoop();
}

static Context currentContext()
{
    if (dialog_active()) {
        return CONTEXT_DIALOG;
    }

    if (mapIsActive()) {
        return isInCombat() ? CONTEXT_COMBAT : CONTEXT_MAP;
    }

    return CONTEXT_WINDOW;
}

static void controllerOpen(int deviceIndex)
{
    if (gController != nullptr) {
        return;
    }

    if (!SDL_IsGameController(deviceIndex)) {
        return;
    }

    gController = SDL_GameControllerOpen(deviceIndex);
    if (gController != nullptr) {
        gControllerId = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gController));
        gControllerJustConnected = true;
        for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++) {
            gAxis[i] = 0;
        }
    }
}

static void releaseAll()
{
    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
        Binding* binding = &(gActiveBinding[i]);
        if (binding->action == ACTION_KEY) {
            inputInjectKey(binding->scancode, false);
        }
        binding->action = ACTION_NONE;
    }

    for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++) {
        gAxis[i] = 0;
    }

    gMouseLeftHeld = false;
    gMouseRightHeld = false;
    gSelectHeld = false;
}

static void controllerClose(SDL_JoystickID id)
{
    if (gController != nullptr && id == gControllerId) {
        releaseAll();
        SDL_GameControllerClose(gController);
        gController = nullptr;
        gControllerId = -1;

        // Se c'è un altro controller collegato usa quello.
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            controllerOpen(i);
            if (gController != nullptr) {
                break;
            }
        }
    }
}

static Binding key(int scancode)
{
    Binding binding;
    binding.action = ACTION_KEY;
    binding.scancode = scancode;
    return binding;
}

static Binding keyTap(int scancode)
{
    Binding binding;
    binding.action = ACTION_KEY_TAP;
    binding.scancode = scancode;
    return binding;
}

static Binding act(Action action)
{
    Binding binding;
    binding.action = action;
    return binding;
}

static int topMenuWindow()
{
    return windowManagerGetTopVisibleWindow(display_win, interfaceWindow);
}

// Fallout 2: la scheda personaggio scorre caratteristiche e abilità con le
// frecce. Fallout 1 solo sinistra/destra (togli/aggiungi punti): su e giù
// scorrono le righe con il cursore (vedi navTick).
static const bool kEditorVerticalArrows = false;

static bool menuUsesArrowKeys()
{
    int top = topMenuWindow();
    return top != -1 && ((kEditorVerticalArrows && top == characterEditorGetWindow()) || top == loadSaveGetWindow());
}

static bool menuUsesSideArrowKeys()
{
    int top = topMenuWindow();
    return top != -1 && top == characterEditorGetWindow();
}

static bool menuIsWorldmap()
{
    int top = topMenuWindow();
    return top != -1 && top == worldmapGetWindow();
}

// Tabella dei comandi. Vedi CONTROLLI.md per lo schema completo.
static Binding bindingFor(int button, Context context)
{
    // Livello "Select tenuto premuto".
    if (gSelectHeld && (context == CONTEXT_MAP || context == CONTEXT_COMBAT)) {
        switch (button) {
        case SDL_CONTROLLER_BUTTON_A:
            return keyTap(SDL_SCANCODE_F6); // salvataggio rapido
        case SDL_CONTROLLER_BUTTON_Y:
            return keyTap(SDL_SCANCODE_F7); // caricamento rapido
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return keyTap(SDL_SCANCODE_Z); // riposa / orologio Pip-Boy
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            return keyTap(SDL_SCANCODE_RETURN); // termina il combattimento
        }
        return Binding();
    }

    switch (context) {
    case CONTEXT_MAP:
    case CONTEXT_COMBAT:
        switch (button) {
        case SDL_CONTROLLER_BUTTON_A:
            return act(context == CONTEXT_MAP ? ACTION_PAD_USE : ACTION_MOUSE_LEFT);
        case SDL_CONTROLLER_BUTTON_B:
            return act(ACTION_CANCEL_MODE);
        case SDL_CONTROLLER_BUTTON_X:
            return act(context == CONTEXT_MAP ? ACTION_PAD_LOOK : ACTION_MOUSE_RIGHT);
        case SDL_CONTROLLER_BUTTON_Y:
            return keyTap(SDL_SCANCODE_I);
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            return act(ACTION_TARGET_PREV);
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            return act(ACTION_TARGET_NEXT);
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return keyTap(SDL_SCANCODE_P);
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return keyTap(SDL_SCANCODE_C);
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return keyTap(SDL_SCANCODE_S);
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return keyTap(SDL_SCANCODE_TAB);
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:
            return keyTap(SDL_SCANCODE_HOME);
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
            return keyTap(SDL_SCANCODE_N);
        case SDL_CONTROLLER_BUTTON_START:
            return keyTap(SDL_SCANCODE_ESCAPE);
        }
        break;
    case CONTEXT_DIALOG:
    case CONTEXT_WINDOW:
        // Scheda personaggio e salvataggi hanno già una selezione da
        // tastiera: la croce manda le frecce (su/giù scegli, sinistra/destra
        // togli/aggiungi punti). I pulsanti si raggiungono con lo stick.
        if (menuUsesSideArrowKeys() && !menuUsesArrowKeys()) {
            switch (button) {
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                return key(SDL_SCANCODE_LEFT);
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                return key(SDL_SCANCODE_RIGHT);
            }
        }
        if (menuUsesArrowKeys()) {
            switch (button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                return key(SDL_SCANCODE_UP);
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                return key(SDL_SCANCODE_DOWN);
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                return key(SDL_SCANCODE_LEFT);
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                return key(SDL_SCANCODE_RIGHT);
            }
        }
        // Menu, finestre e dialoghi: la croce (e lo stick sinistro) saltano
        // da un pulsante all'altro, A lo preme.
        switch (button) {
        case SDL_CONTROLLER_BUTTON_A:
            return act(ACTION_MOUSE_LEFT);
        case SDL_CONTROLLER_BUTTON_X:
            return act(ACTION_MOUSE_RIGHT);
        case SDL_CONTROLLER_BUTTON_B:
        case SDL_CONTROLLER_BUTTON_START:
            return keyTap(SDL_SCANCODE_ESCAPE);
        case SDL_CONTROLLER_BUTTON_Y:
            return keyTap(SDL_SCANCODE_RETURN);
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return act(ACTION_NAV_UP);
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return act(ACTION_NAV_DOWN);
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return act(ACTION_NAV_LEFT);
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return act(ACTION_NAV_RIGHT);
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            return act(ACTION_WHEEL_UP);
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            return act(ACTION_WHEEL_DOWN);
        }
        break;
    }

    return Binding();
}

// I grilletti sono assi: li trattiamo come pulsanti virtuali.
#define VIRTUAL_BUTTON_LEFT_TRIGGER (SDL_CONTROLLER_BUTTON_MAX - 2)
#define VIRTUAL_BUTTON_RIGHT_TRIGGER (SDL_CONTROLLER_BUTTON_MAX - 1)

static Binding triggerBindingFor(bool right, Context context)
{
    if (context == CONTEXT_MAP || context == CONTEXT_COMBAT) {
        if (!right) {
            return keyTap(SDL_SCANCODE_B); // cambia mano
        }
        // Fuori dal combattimento lo inizia, durante il combattimento
        // termina il turno.
        return keyTap(context == CONTEXT_COMBAT ? SDL_SCANCODE_SPACE : SDL_SCANCODE_A);
    }

    if (context == CONTEXT_WINDOW || context == CONTEXT_DIALOG) {
        return right ? key(SDL_SCANCODE_PAGEDOWN) : key(SDL_SCANCODE_PAGEUP);
    }

    return Binding();
}

static void dialogStep(int step);
static bool mapIsActive();

static void bindingPress(int slot, Binding binding)
{
    gActiveBinding[slot] = binding;

    switch (binding.action) {
    case ACTION_KEY:
        inputInjectKey(binding.scancode, true);
        break;
    case ACTION_KEY_TAP:
        inputInjectKey(binding.scancode, true);
        inputInjectKey(binding.scancode, false);
        break;
    case ACTION_MOUSE_LEFT:
        if (gPadMode && currentContext() == CONTEXT_COMBAT) {
            // Con il controller in combattimento A esegue subito: il
            // bersaglio (LB/RB) o l'esagono (stick) li hai già scelti.
            if (gmouse_3d_get_mode() == GAME_MOUSE_MODE_MOVE) {
                gCombatHex = -1;
            }
        } else if (mapIsActive()) {
            int mouseX;
            int mouseY;
            mouse_get_position(&mouseX, &mouseY);
            if (gWarpPending) {
                mouseX = gWarpX;
                mouseY = gWarpY;
            }
            if (!gamepadSmartClick(mouseX, mouseY)) {
                gActiveBinding[slot].action = ACTION_NONE;
                break;
            }
        }
        gMouseLeftHeld = true;
        gMouseLeftPulse = 2;
        break;
    case ACTION_MOUSE_RIGHT:
        gMouseRightHeld = true;
        gMouseRightPulse = 2;
        break;
    case ACTION_TARGET_PREV:
        gPendingTargetStep = -1;
        break;
    case ACTION_TARGET_NEXT:
        gPendingTargetStep = 1;
        break;
    case ACTION_CANCEL_MODE:
        gPendingCancelMode = true;
        break;
    case ACTION_PAD_USE:
        gUseHolding = true;
        gUseHeldSince = SDL_GetTicks();
        break;
    case ACTION_PAD_LOOK:
        gPendingLook = true;
        break;
    case ACTION_NAV_UP:
    case ACTION_NAV_DOWN:
    case ACTION_NAV_LEFT:
    case ACTION_NAV_RIGHT:
        gNavDirX = binding.action == ACTION_NAV_LEFT ? -1 : (binding.action == ACTION_NAV_RIGHT ? 1 : 0);
        gNavDirY = binding.action == ACTION_NAV_UP ? -1 : (binding.action == ACTION_NAV_DOWN ? 1 : 0);
        gNavPendingX = gNavDirX;
        gNavPendingY = gNavDirY;
        gNavNextRepeat = SDL_GetTicks() + 380;
        break;
    case ACTION_WHEEL_UP:
        gWheelPending += 1;
        inputInjectKey(SDL_SCANCODE_PAGEUP, true);
        inputInjectKey(SDL_SCANCODE_PAGEUP, false);
        break;
    case ACTION_WHEEL_DOWN:
        gWheelPending -= 1;
        inputInjectKey(SDL_SCANCODE_PAGEDOWN, true);
        inputInjectKey(SDL_SCANCODE_PAGEDOWN, false);
        break;
    case ACTION_DIALOG_PREV:
        dialogStep(-1);
        break;
    case ACTION_DIALOG_NEXT:
        dialogStep(1);
        break;
    default:
        break;
    }
}

static void bindingRelease(int slot)
{
    Binding* binding = &(gActiveBinding[slot]);
    switch (binding->action) {
    case ACTION_KEY:
        inputInjectKey(binding->scancode, false);
        break;
    case ACTION_MOUSE_LEFT:
        gMouseLeftHeld = false;
        break;
    case ACTION_MOUSE_RIGHT:
        gMouseRightHeld = false;
        break;
    case ACTION_NAV_UP:
    case ACTION_NAV_DOWN:
    case ACTION_NAV_LEFT:
    case ACTION_NAV_RIGHT:
        gNavDirX = 0;
        gNavDirY = 0;
        break;
    case ACTION_PAD_USE:
        if (gMenuHold) {
            // Rilascio = conferma la voce scelta nel menu azioni.
            gMouseLeftHeld = false;
            gMenuHold = false;
        } else if (gUseHolding) {
            gPendingUse = true;
        }
        gUseHolding = false;
        break;
    default:
        break;
    }
    binding->action = ACTION_NONE;
}

static bool gSelectUsedAsModifier = false;
static bool gTriggerDown[2] = { false, false };

// RT in combattimento: tocco = fine turno, tenuto premuto = fine combattimento.
static Uint32 gEndTurnHeldSince = 0;
static bool gEndCombatSent = false;
static const Uint32 kEndCombatHoldMs = 600;

void gamepadHandleEvent(SDL_Event* event)
{
    switch (event->type) {
    case SDL_CONTROLLERDEVICEADDED:
        controllerOpen(event->cdevice.which);
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        controllerClose(event->cdevice.which);
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        gPadMode = true;
        gLastPadInput = SDL_GetTicks();
        if (gMenuHold && (event->cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP || event->cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)) {
            gMenuStep += event->cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP ? -1 : 1;
            break;
        }
#ifdef FALLOUT_AUTOTEST
        printf("[pad] pulsante %d giù, contesto %d\n", event->cbutton.button, currentContext());
#endif
        if (event->cbutton.which != gControllerId) {
            break;
        }
        if (event->cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
            gSelectHeld = true;
            gSelectUsedAsModifier = false;
            break;
        }
        if (event->cbutton.button < VIRTUAL_BUTTON_LEFT_TRIGGER) {
            Binding binding = bindingFor(event->cbutton.button, currentContext());
            if (gSelectHeld) {
                gSelectUsedAsModifier = true;
            }
            bindingPress(event->cbutton.button, binding);
        }
        break;
    case SDL_CONTROLLERBUTTONUP:
        if (event->cbutton.which != gControllerId) {
            break;
        }
        if (event->cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
            gSelectHeld = false;
            break;
        }
        if (event->cbutton.button < VIRTUAL_BUTTON_LEFT_TRIGGER) {
            bindingRelease(event->cbutton.button);
        }
        break;
    case SDL_CONTROLLERAXISMOTION:
#ifdef FALLOUT_AUTOTEST
        printf("[pad] asse %d = %d (id %d, atteso %d)\n", event->caxis.axis, event->caxis.value, event->caxis.which, gControllerId);
#endif
        if (event->caxis.which != gControllerId) {
            break;
        }
        gAxis[event->caxis.axis] = event->caxis.value;
        if (abs(event->caxis.value) > 12000) {
            gPadMode = true;
            gLastPadInput = SDL_GetTicks();
        }

        if (event->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || event->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
            bool right = event->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
            int slot = right ? VIRTUAL_BUTTON_RIGHT_TRIGGER : VIRTUAL_BUTTON_LEFT_TRIGGER;
            // Isteresi: premuto oltre il 50%, rilasciato sotto il 25%.
            if (!gTriggerDown[right] && event->caxis.value > 16000) {
                gTriggerDown[right] = true;
                Binding binding;
                if (gSelectHeld && right && (currentContext() == CONTEXT_COMBAT)) {
                    binding = keyTap(SDL_SCANCODE_RETURN); // termina combattimento
                    gSelectUsedAsModifier = true;
                } else if (right && currentContext() == CONTEXT_COMBAT) {
                    // Decide al rilascio (o dopo un attimo se resta premuto).
                    gEndTurnHeldSince = SDL_GetTicks();
                    gEndCombatSent = false;
                } else {
                    binding = triggerBindingFor(right, currentContext());
                }
                bindingPress(slot, binding);
            } else if (gTriggerDown[right] && event->caxis.value < 8000) {
                gTriggerDown[right] = false;
                if (right && gEndTurnHeldSince != 0) {
                    if (!gEndCombatSent && currentContext() == CONTEXT_COMBAT) {
                        inputInjectKey(SDL_SCANCODE_SPACE, true);
                        inputInjectKey(SDL_SCANCODE_SPACE, false);
                    }
                    gEndTurnHeldSince = 0;
                    gEndCombatSent = false;
                }
                bindingRelease(slot);
            }
        }
        break;
    }
}

// -----------------------------------------------------------------------------
// Cursore
// -----------------------------------------------------------------------------

static double axisValue(int axis)
{
    double value = gAxis[axis] / 32767.0;
    return std::clamp(value, -1.0, 1.0);
}

// Stick con zona morta radiale e curva di risposta. Restituisce l'intensità
// (0..1) e la direzione normalizzata.
static double stickRead(int axisX, int axisY, double* nx, double* ny)
{
    double x = axisValue(axisX);
    double y = axisValue(axisY);
    double magnitude = sqrt(x * x + y * y);
    double deadzone = gMobile.deadzone / 100.0;

    if (magnitude < deadzone) {
        *nx = 0.0;
        *ny = 0.0;
        return 0.0;
    }

    *nx = x / magnitude;
    *ny = y / magnitude;
    return std::min(1.0, (magnitude - deadzone) / (1.0 - deadzone));
}

static void dialogStep(int step)
{
    int xs[DIALOG_TARGETS_MAX];
    int ys[DIALOG_TARGETS_MAX];
    int count = gameDialogGetOptionTargets(xs, ys, DIALOG_TARGETS_MAX);
    if (count <= 0) {
        // Nessuna risposta visibile: scorri il testo.
        inputInjectKey(step < 0 ? SDL_SCANCODE_UP : SDL_SCANCODE_DOWN, true);
        inputInjectKey(step < 0 ? SDL_SCANCODE_UP : SDL_SCANCODE_DOWN, false);
        return;
    }

    int mouseX;
    int mouseY;
    mouse_get_position(&mouseX, &mouseY);
    if (gWarpPending) {
        mouseY = gWarpY;
    }

    // Risposta su cui si trova ora il cursore (la più vicina in verticale).
    int current = -1;
    int best = INT_MAX;
    for (int i = 0; i < count; i++) {
        int distance = abs(ys[i] - mouseY);
        if (distance < best && mouseX >= xs[i] - 200 && mouseX <= xs[i] + 400) {
            best = distance;
            current = i;
        }
    }

    int next;
    if (current == -1 || best > 30) {
        next = step > 0 ? 0 : count - 1;
    } else {
        next = (current + step + count) % count;
    }

    gWarpPending = true;
    gWarpX = xs[next];
    gWarpY = ys[next];
}

// -----------------------------------------------------------------------------
// Menu: navigazione a salti tra i pulsanti
// -----------------------------------------------------------------------------

static int navFind(const std::vector<NavTarget>& targets, int win, int btn)
{
    for (size_t i = 0; i < targets.size(); i++) {
        if (targets[i].win == win && targets[i].btn == btn) {
            return (int)i;
        }
    }
    return -1;
}

static void navCenter(const NavTarget& target, int* x, int* y)
{
    *x = (target.rect.ulx + target.rect.lrx) / 2;
    *y = (target.rect.uly + target.rect.lry) / 2;
}

static int navNearest(const std::vector<NavTarget>& targets, int x, int y)
{
    int best = -1;
    long long bestDistance = LLONG_MAX;
    for (size_t i = 0; i < targets.size(); i++) {
        const Rect& r = targets[i].rect;
        // Distanza dal rettangolo (0 se il punto è dentro).
        int ddx = x < r.ulx ? r.ulx - x : (x > r.lrx ? x - r.lrx : 0);
        int ddy = y < r.uly ? r.uly - y : (y > r.lry ? y - r.lry : 0);
        long long distance = (long long)ddx * ddx + (long long)ddy * ddy;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = (int)i;
        }
    }
    return best;
}

// Pulsante iniziale: quello sotto il cursore se c'è, altrimenti il primo in
// alto a sinistra (la lettura naturale di un menu).
static int navDefault(const std::vector<NavTarget>& targets, int mouseX, int mouseY)
{
    // Nei dialoghi si parte dalla prima risposta.
    if (dialog_active()) {
        int xs[DIALOG_TARGETS_MAX];
        int ys[DIALOG_TARGETS_MAX];
        if (gameDialogGetOptionTargets(xs, ys, DIALOG_TARGETS_MAX) > 0) {
            for (size_t i = 0; i < targets.size(); i++) {
                const Rect& r = targets[i].rect;
                if (ys[0] >= r.uly && ys[0] <= r.lry && xs[0] >= r.ulx - 60 && xs[0] <= r.lrx) {
                    return (int)i;
                }
            }
        }
    }

    for (size_t i = 0; i < targets.size(); i++) {
        const Rect& r = targets[i].rect;
        if (mouseX >= r.ulx && mouseX <= r.lrx && mouseY >= r.uly && mouseY <= r.lry) {
            return (int)i;
        }
    }

    int best = -1;
    long long bestScore = LLONG_MAX;
    for (size_t i = 0; i < targets.size(); i++) {
        const Rect& r = targets[i].rect;
        long long score = (long long)r.uly * 4 + r.ulx; // prima le righe in alto
        if (score < bestScore) {
            bestScore = score;
            best = (int)i;
        }
    }
    return best;
}

// Il pulsante più vicino nella direzione indicata: conta soprattutto lo
// spostamento lungo la direzione, poco quello di lato.
static int navStep(const std::vector<NavTarget>& targets, int current, int dirX, int dirY)
{
    int cx;
    int cy;
    navCenter(targets[current], &cx, &cy);
    const Rect& cr = targets[current].rect;

    int best = -1;
    double bestScore = 1e18;
    for (size_t i = 0; i < targets.size(); i++) {
        if ((int)i == current) {
            continue;
        }
        const Rect& r = targets[i].rect;
        int x;
        int y;
        navCenter(targets[i], &x, &y);

        double along;
        double side;
        if (dirX != 0) {
            // Deve stare davvero da quella parte (oltre il bordo del pulsante
            // attuale, con un po' di tolleranza).
            if (dirX > 0 && r.ulx <= cr.ulx + 2) continue;
            if (dirX < 0 && r.lrx >= cr.lrx - 2) continue;
            along = fabs((double)(x - cx));
            // Sovrapposizione verticale: stessa riga = nessuna penalità.
            bool overlap = r.uly <= cr.lry && r.lry >= cr.uly;
            side = overlap ? 0.0 : fabs((double)(y - cy));
        } else {
            if (dirY > 0 && r.uly <= cr.uly + 2) continue;
            if (dirY < 0 && r.lry >= cr.lry - 2) continue;
            along = fabs((double)(y - cy));
            bool overlap = r.ulx <= cr.lrx && r.lrx >= cr.ulx;
            side = overlap ? 0.0 : fabs((double)(x - cx));
        }
        double score = along + side * 3.0;
        if (score < bestScore) {
            bestScore = score;
            best = (int)i;
        }
    }
    return best;
}

// Ogni frame nei menu: seleziona un pulsante e sposta lì il cursore.
static void navTick(Context context, Uint32 now, int* dx, int* dy)
{
    bool inMenu = context == CONTEXT_WINDOW || context == CONTEXT_DIALOG;
    if (context != gLastContext) {
        gLastContext = context;
        gNavWin = -1;
        gNavBtn = -1;
        gNavAutoSelectAt = inMenu ? now + 250 : 0;
    }
    if (!inMenu || !gPadMode) {
        return;
    }

    // Croce tenuta premuta: ripete.
    if ((gNavDirX != 0 || gNavDirY != 0) && now >= gNavNextRepeat) {
        gNavPendingX = gNavDirX;
        gNavPendingY = gNavDirY;
        gNavNextRepeat = now + 110;
    }

    // Stick sinistro come la croce (nella mappa del mondo è un cursore
    // libero, per viaggiare verso qualsiasi punto).
    double nx;
    double ny;
    bool worldmap = menuIsWorldmap();
    if (worldmap && stickRead(SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &nx, &ny) > 0.0) {
        gNavWin = -1;
        gNavBtn = -1;
    } else if (!worldmap && stickRead(SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &nx, &ny) > 0.55) {
        int sx = fabs(nx) >= fabs(ny) ? (nx > 0 ? 1 : -1) : 0;
        int sy = fabs(nx) >= fabs(ny) ? 0 : (ny > 0 ? 1 : -1);
        if (sx != gNavStickDirX || sy != gNavStickDirY) {
            gNavPendingX = sx;
            gNavPendingY = sy;
            gNavStickNext = now + 350;
        } else if (now >= gNavStickNext) {
            gNavPendingX = sx;
            gNavPendingY = sy;
            gNavStickNext = now + 130;
        }
        gNavStickDirX = sx;
        gNavStickDirY = sy;
    } else {
        gNavStickDirX = 0;
        gNavStickDirY = 0;
    }

    bool wantJump = gNavPendingX != 0 || gNavPendingY != 0;
    bool autoSelect = gNavAutoSelectAt != 0 && now >= gNavAutoSelectAt;
    // Controllo periodico: il cursore resta agganciato al pulsante scelto
    // (il gioco a volte lo sposta quando apre una finestra), e se il pulsante
    // sparisce se ne sceglie un altro.
    static Uint32 nextCheck = 0;
    bool check = gNavWin != -1 && now >= nextCheck && !gMouseLeftHeld;
    if (!wantJump && !autoSelect && !check) {
        return;
    }
    nextCheck = now + 150;

    std::vector<NavTarget> targets(256);
    int count = windowManagerGetNavTargets(targets.data(), (int)targets.size(), display_win, interfaceWindow);
    targets.resize(count);
    if (count == 0) {
        gNavPendingX = 0;
        gNavPendingY = 0;
        return;
    }

    int mouseX;
    int mouseY;
    mouse_get_position(&mouseX, &mouseY);

    int current = navFind(targets, gNavWin, gNavBtn);
    int next = -1;

    // Aree alte (liste di righe, come le abilità o le note del Pip-Boy): su e
    // giù scorrono le righe dentro l'area, si esce solo dai bordi.
    if (wantJump && current != -1 && gNavPendingY != 0) {
        const Rect& r = targets[current].rect;
        int rowStep = 11 * screenGetHeight() / 480;
        if (screenGetHeight() > 480) {
            rowStep = 11; // l'interfaccia resta a 480 righe anche su schermi più alti
        }
        if (r.lry - r.uly >= 60) {
            int y = mouseY + gNavPendingY * rowStep;
            if (y >= r.uly + 3 && y <= r.lry - 3) {
                *dy += y - mouseY;
                gNavPendingX = 0;
                gNavPendingY = 0;
                return;
            }
        }
    }

    if (wantJump) {
        if (current == -1) {
            next = navDefault(targets, mouseX, mouseY);
        } else {
            next = navStep(targets, current, gNavPendingX, gNavPendingY);
        }
    } else if (current == -1) {
        // Appena aperta una finestra, o il pulsante scelto non c'è più.
        next = navDefault(targets, mouseX, mouseY);
    } else {
        const Rect& r = targets[current].rect;
        if (mouseX < r.ulx || mouseX > r.lrx || mouseY < r.uly || mouseY > r.lry) {
            next = current;
        }
    }
    int lastJumpY = gNavPendingY;
#ifdef FALLOUT_AUTOTEST
    if (wantJump) {
        printf("[nav] salto (%d,%d) da %d a %d su %d pulsanti\n", gNavPendingX, gNavPendingY, current, next, count);
    }
#endif
    gNavPendingX = 0;
    gNavPendingY = 0;
    gNavAutoSelectAt = 0;

    if (next != -1) {
        bool enteringVertically = next != current && current != -1 && lastJumpY != 0;
        gNavWin = targets[next].win;
        gNavBtn = targets[next].btn;
        int x;
        int y;
        navCenter(targets[next], &x, &y);
        const Rect& nr = targets[next].rect;
        if (enteringVertically && nr.lry - nr.uly >= 60) {
            // Si entra in una lista dall'alto o dal basso: prima/ultima riga.
            y = lastJumpY > 0 ? nr.uly + 6 : nr.lry - 6;
        }
        *dx += x - mouseX;
        *dy += y - mouseY;
        gCursorRemainderX = 0.0;
        gCursorRemainderY = 0.0;
    }
}

#ifdef FALLOUT_AUTOTEST
void gamepadDebugDumpNav(const char* name)
{
    std::vector<NavTarget> targets(256);
    int count = windowManagerGetNavTargets(targets.data(), (int)targets.size(), display_win, interfaceWindow);
    const char* dir = getenv("FALLOUT_SHOTS");
    std::string path = std::string(dir != nullptr ? dir : ".") + "/" + name + ".nav";
    FILE* stream = fopen(path.c_str(), "wt");
    for (int i = 0; i < count; i++) {
        fprintf(stream, "%d %d %d %d %d %d %d\n", targets[i].rect.ulx, targets[i].rect.uly, targets[i].rect.lrx, targets[i].rect.lry, targets[i].win, targets[i].btn,
            targets[i].win == gNavWin && targets[i].btn == gNavBtn);
    }
    fclose(stream);
    printf("[pad] %d pulsanti, contesto %d\n", count, currentContext());
}
#endif

void gamepadGetMouseInput(int* dx, int* dy, int* buttons, int* wheelX, int* wheelY)
{
    *dx = 0;
    *dy = 0;
    *buttons = 0;
    *wheelX = 0;
    *wheelY = 0;

    mobileSettingsLoad();

    Uint32 now = SDL_GetTicks();
    double elapsed = gLastMouseTick == 0 ? 0.016 : (now - gLastMouseTick) / 1000.0;
    elapsed = std::clamp(elapsed, 0.0, 0.05);
    gLastMouseTick = now;

    if (gWarpPending) {
        int mouseX;
        int mouseY;
        mouse_get_position(&mouseX, &mouseY);
        *dx += gWarpX - mouseX;
        *dy += gWarpY - mouseY;
        gWarpPending = false;
        gCursorRemainderX = 0.0;
        gCursorRemainderY = 0.0;
    }

    if (gController == nullptr) {
        return;
    }

    Context context = currentContext();
    bool pointerless = context == CONTEXT_MAP && gPadMode;

    navTick(context, now, dx, dy);

    // Senza puntatore il cursore resta nascosto sulla mappa; ricompare nei
    // menu, in combattimento e quando si apre il menu azioni.
    bool wantHidden = pointerless && !gMenuHold;
    if (wantHidden != gWeHidCursor) {
        mouseSetCursorInvisible(wantHidden);
        gWeHidCursor = wantHidden;
    }
    // Nasconde subito anche il cursore "3D" della mappa: altrimenti il gioco
    // farebbe in tempo a scrivere il nome di quello che c'era sotto.
    if (wantHidden && gmouse_3d_is_on()) {
        gmouse_3d_off();
        gWeHidMapCursor = true;
    }

    // A tenuto premuto su un oggetto: apre il menu azioni del gioco.
    if (gUseHolding && !gMenuHold && pointerless && gFocus != nullptr && now - gUseHeldSince >= 350) {
        gMenuHold = true;
        gMenuStep = 0;
        if (gWeHidMapCursor) {
            gmouse_3d_on();
            gWeHidMapCursor = false;
        }
        mouseSetCursorInvisible(false);
        gWeHidCursor = false;
        gMouseLeftHeld = true;
    }

    if (gMenuHold) {
        // Nel menu azioni: croce o stick su/giù scorrono le voci (il gioco
        // cambia voce quando il cursore si sposta di più di 10 punti).
        static Uint32 lastStickStep = 0;
        double sx;
        double sy;
        if (stickRead(SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &sx, &sy) > 0.5 && fabs(sy) > 0.7 && now - lastStickStep > 220) {
            gMenuStep += sy > 0 ? 1 : -1;
            lastStickStep = now;
        }
        if (gMenuStep != 0) {
            *dy += gMenuStep > 0 ? 14 : -14;
            gMenuStep += gMenuStep > 0 ? -1 : 1;
        }
        *buttons |= MOUSE_STATE_LEFT_BUTTON_DOWN;
        return;
    }

    if (pointerless) {
        // Niente cursore da muovere: gli stick muovono personaggio e visuale.
        if (gMouseLeftHeld || gMouseLeftPulse > 0) {
            *buttons |= MOUSE_STATE_LEFT_BUTTON_DOWN;
        }
        if (gMouseLeftPulse > 0) gMouseLeftPulse--;
        return;
    }

    // Stick che muove il cursore: sulla mappa fuori dal combattimento lo
    // stick sinistro muove il personaggio, quindi il cursore è sul destro.
    bool leftMovesDude = context == CONTEXT_MAP && gMobile.stickMovement;

    double cx = 0.0;
    double cy = 0.0;
    double cursorMagnitude = 0.0;

    if (gWheelPending != 0) {
        *wheelY = gWheelPending > 0 ? 1 : -1;
        gWheelPending += gWheelPending > 0 ? -1 : 1;
    }

    // Stick sinistro: personaggio (mappa), esagoni (combattimento), salti tra
    // i pulsanti (menu). Muove il cursore solo se il movimento con lo stick è
    // disattivato nelle impostazioni.
    double nx;
    double ny;
    double left = stickRead(SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &nx, &ny);
    bool worldmap = context == CONTEXT_WINDOW && menuIsWorldmap();
    bool leftIsCursor = (context == CONTEXT_MAP && !gMobile.stickMovement) || worldmap;
    (void)leftMovesDude;
    if (leftIsCursor && left > 0.0) {
        cx = nx;
        cy = ny;
        cursorMagnitude = left;
    }

    double rx;
    double ry;
    double right = stickRead(SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, &rx, &ry);
    if (right > 0.0) {
        if (context == CONTEXT_COMBAT && gPadMode) {
            // In combattimento lo stick destro sposta la visuale.
        } else if (right > cursorMagnitude) {
            // Nei menu lo stick destro è un cursore libero di riserva, per i
            // punti che non sono pulsanti (es. la mappa del mondo).
            if (context == CONTEXT_WINDOW || context == CONTEXT_DIALOG) {
                gNavWin = -1;
                gNavBtn = -1;
            }
            cx = rx;
            cy = ry;
            cursorMagnitude = right;
        }
    }

    if (cursorMagnitude > 0.0) {
        // Curva quadratica: preciso con poca spinta, veloce a fondo corsa.
        // La velocità è riferita a uno schermo alto 480 punti.
        double scale = screenGetHeight() / 480.0;
        double speed = 900.0 * scale * (gMobile.cursorSpeed / 100.0) * cursorMagnitude * cursorMagnitude;
        speed = std::max(speed, 40.0 * scale);
        gCursorRemainderX += cx * speed * elapsed;
        gCursorRemainderY += cy * speed * elapsed;
        int moveX = (int)gCursorRemainderX;
        int moveY = (int)gCursorRemainderY;
        gCursorRemainderX -= moveX;
        gCursorRemainderY -= moveY;
        *dx += moveX;
        *dy += moveY;
    }

    // Un clic deve durare almeno un frame anche se il pulsante viene
    // rilasciato prima che il gioco legga l'input.
    if (gMouseLeftHeld || gMouseLeftPulse > 0) {
        *buttons |= MOUSE_STATE_LEFT_BUTTON_DOWN;
    }
    if (gMouseRightHeld || gMouseRightPulse > 0) {
        *buttons |= MOUSE_STATE_RIGHT_BUTTON_DOWN;
    }
    if (gMouseLeftPulse > 0) gMouseLeftPulse--;
    if (gMouseRightPulse > 0) gMouseRightPulse--;
}

// -----------------------------------------------------------------------------
// Oggetti interattivi (aggancio bersagli e tocco)
// -----------------------------------------------------------------------------

struct Candidate {
    Object* object;
    int x;
    int y;
    Rect rect;
};

static bool gIncludeDude = false; // abilità su se stessi (pronto soccorso)

static bool isInteresting(Object* object, bool combatOnly)
{
    if (object == nullptr) {
        return false;
    }

    if (object == obj_dude) {
        return gIncludeDude && !combatOnly;
    }

    if ((object->flags & OBJECT_HIDDEN) != 0) {
        return false;
    }

    int type = PID_TYPE(object->pid);

    if (type == OBJ_TYPE_CRITTER) {
        if (combatOnly) {
            return !critter_is_dead(object) && !isPartyMember(object);
        }
        // Anche i corpi: si possono perquisire.
        return true;
    }

    if (combatOnly) {
        return false;
    }

    if (type == OBJ_TYPE_ITEM) {
        return true;
    }

    if (type == OBJ_TYPE_SCENERY) {
        Proto* proto;
        if (proto_ptr(object->pid, &proto) == -1) {
            return false;
        }
        if (proto->scenery.type != SCENERY_TYPE_GENERIC) {
            return true; // porte, scale, ascensori
        }
        // Solo gli oggetti che si possono usare (computer, interruttori...):
        // non i muri o i mobili decorativi che hanno uno script.
        return proto_action_can_use((object)->pid);
    }

    return false;
}

static bool isoWindowRect(Rect* rect)
{
    if (win_get_rect(display_win, rect) == -1) {
        return false;
    }
    return true;
}

static void collectCandidates(std::vector<Candidate>& out, bool combatOnly)
{
    out.clear();

    Rect view;
    if (!isoWindowRect(&view)) {
        return;
    }

    for (Object* object = obj_find_first_at(map_elevation); object != nullptr; object = obj_find_next_at()) {
        if (!isInteresting(object, combatOnly)) {
            continue;
        }

        Rect rect;
        obj_bound(object, &rect);
        if (rect.lrx <= rect.ulx || rect.lry <= rect.uly) {
            continue;
        }

        Candidate candidate;
        candidate.object = object;
        candidate.rect = rect;
        candidate.x = (rect.ulx + rect.lrx) / 2;
        // Un po' sotto il centro: le figure sono alte e il punto "pieno"
        // dello sprite è sul busto.
        candidate.y = rect.uly + (rect.lry - rect.uly) * 3 / 5;

        if (candidate.x < view.ulx + 4 || candidate.x > view.lrx - 4
            || candidate.y < view.uly + 4 || candidate.y > view.lry - 4) {
            continue;
        }

        out.push_back(candidate);
    }
}

bool gamepadSnapPoint(int* x, int* y)
{
    if (!mapIsActive()) {
        return false;
    }

    int mode = gmouse_3d_get_mode();
    if (mode == GAME_MOUSE_MODE_MOVE) {
        return false;
    }

    std::vector<Candidate> candidates;
    collectCandidates(candidates, mode == GAME_MOUSE_MODE_CROSSHAIR);

    const int margin = 14 * screenGetHeight() / 480;
    int best = INT_MAX;
    const Candidate* found = nullptr;
    for (const Candidate& candidate : candidates) {
        if (*x < candidate.rect.ulx - margin || *x > candidate.rect.lrx + margin
            || *y < candidate.rect.uly - margin || *y > candidate.rect.lry + margin) {
            continue;
        }
        int ddx = *x - candidate.x;
        int ddy = *y - candidate.y;
        int distance = ddx * ddx + ddy * ddy;
        if (distance < best) {
            best = distance;
            found = &candidate;
        }
    }

    if (found == nullptr) {
        return false;
    }

    *x = found->x;
    *y = found->y;
    return true;
}

// Oggetto interattivo sotto il punto. Il rettangolo dello sprite è più largo
// della figura, quindi si usa una versione ristretta.
static Object* objectAtPoint(int x, int y, bool combatOnly)
{
    std::vector<Candidate> candidates;
    collectCandidates(candidates, combatOnly);

    Object* found = nullptr;
    int best = INT_MAX;
    for (const Candidate& candidate : candidates) {
        const Rect& rect = candidate.rect;
        int width = rect.lrx - rect.ulx;
        int height = rect.lry - rect.uly;
        if (x < rect.ulx + width * 15 / 100 || x > rect.lrx - width * 15 / 100
            || y < rect.uly + height / 10 || y > rect.lry) {
            continue;
        }
        int ddx = x - candidate.x;
        int ddy = y - candidate.y;
        if (ddx * ddx + ddy * ddy < best) {
            best = ddx * ddx + ddy * ddy;
            found = candidate.object;
        }
    }
    return found;
}

// Clic intelligente: sceglie da solo il cursore giusto.
//  - Esplorazione: su un oggetto usa/parla/raccogli, sul pavimento cammina.
//  - Combattimento: il primo tocco su un nemico mette il mirino (e mostra la
//    probabilità di colpire), il secondo attacca. Lo stesso per tornare a
//    muoversi: niente punti azione sprecati per errore.
// Restituisce false se il clic va trattenuto (serve una conferma).
bool gamepadSmartClick(int x, int y)
{
    if (!mapIsActive()) {
        return true;
    }

    Rect view;
    if (!isoWindowRect(&view) || x < view.ulx || x > view.lrx || y < view.uly || y > view.lry) {
        return true; // barra dell'interfaccia
    }

    int mode = gmouse_3d_get_mode();

    if (!isInCombat()) {
        if (mode == GAME_MOUSE_MODE_MOVE) {
            if (objectAtPoint(x, y, false) != nullptr) {
                gmouse_3d_set_mode(GAME_MOUSE_MODE_ARROW);
            }
        } else if (mode == GAME_MOUSE_MODE_ARROW) {
            if (objectAtPoint(x, y, false) == nullptr) {
                gmouse_3d_set_mode(GAME_MOUSE_MODE_MOVE);
            }
        }
        return true;
    }

    if (mode == GAME_MOUSE_MODE_MOVE && objectAtPoint(x, y, true) != nullptr) {
        gmouse_3d_set_mode(GAME_MOUSE_MODE_CROSSHAIR);
        return false;
    }

    if (mode == GAME_MOUSE_MODE_CROSSHAIR && objectAtPoint(x, y, true) == nullptr) {
        gmouse_3d_set_mode(GAME_MOUSE_MODE_MOVE);
        return false;
    }

    return true;
}

// Punto dell'oggetto su cui mettere il cursore: un pixel pieno dello sprite
// vicino al centro (il gioco riconosce l'oggetto solo sui pixel pieni).
static void objectAimPoint(Object* object, int* outX, int* outY)
{
    Rect rect;
    obj_bound(object, &rect);
    int x = (rect.ulx + rect.lrx) / 2;
    int y = rect.uly + (rect.lry - rect.uly) * 3 / 5;

    if (obj_intersects_with(object, x, y) == 0) {
        int bestDistance = INT_MAX;
        int bestX = x;
        int bestY = y;
        for (int sy = rect.uly; sy <= rect.lry; sy += 2) {
            for (int sx = rect.ulx; sx <= rect.lrx; sx += 2) {
                int ddx = sx - x;
                int ddy = sy - y;
                int distance = ddx * ddx + ddy * ddy;
                if (distance < bestDistance && obj_intersects_with(object, sx, sy) != 0) {
                    bestDistance = distance;
                    bestX = sx;
                    bestY = sy;
                }
            }
        }
        x = bestX;
        y = bestY;
    }

    *outX = x;
    *outY = y;
}

static void cycleTarget(int step, bool combat)
{
    std::vector<Candidate> candidates;
    collectCandidates(candidates, combat);
#ifdef FALLOUT_AUTOTEST
    printf("[pad] bersagli: %zu\n", candidates.size());
#endif
    if (candidates.empty()) {
        gTargetCurrent = nullptr;
        return;
    }

    int dudeX = 0;
    int dudeY = 0;
    tile_coord(obj_dude->tile, &dudeX, &dudeY, obj_dude->elevation);
    dudeX += 16;
    dudeY += 8;

    if (combat) {
        // In combattimento dal più vicino al più lontano.
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return tile_dist(obj_dude->tile, a.object->tile) < tile_dist(obj_dude->tile, b.object->tile);
        });
    } else {
        // In esplorazione in senso orario attorno al personaggio, così LB/RB
        // si comportano come "a sinistra / a destra".
        std::sort(candidates.begin(), candidates.end(), [dudeX, dudeY](const Candidate& a, const Candidate& b) {
            double angleA = atan2((double)(a.y - dudeY), (double)(a.x - dudeX));
            double angleB = atan2((double)(b.y - dudeY), (double)(b.x - dudeX));
            return angleA < angleB;
        });
    }

    int index = -1;
    for (size_t i = 0; i < candidates.size(); i++) {
        if (candidates[i].object == gTargetCurrent) {
            index = (int)i;
            break;
        }
    }

    if (index == -1) {
        if (combat) {
            index = 0;
        } else {
            // Primo aggancio: l'oggetto più vicino al cursore.
            int mouseX;
            int mouseY;
            mouse_get_position(&mouseX, &mouseY);
            int best = INT_MAX;
            for (size_t i = 0; i < candidates.size(); i++) {
                int ddx = candidates[i].x - mouseX;
                int ddy = candidates[i].y - mouseY;
                if (ddx * ddx + ddy * ddy < best) {
                    best = ddx * ddx + ddy * ddy;
                    index = (int)i;
                }
            }
        }
    } else {
        index = (index + step + (int)candidates.size()) % (int)candidates.size();
    }

    gTargetCurrent = candidates[index].object;
    gWarpPending = true;
    objectAimPoint(gTargetCurrent, &gWarpX, &gWarpY);

    // Cursore giusto per l'oggetto: mirino sui nemici in combattimento,
    // freccia "usa" sugli oggetti in esplorazione.
    int wanted = combat ? GAME_MOUSE_MODE_CROSSHAIR : GAME_MOUSE_MODE_ARROW;
    if (gmouse_3d_get_mode() == GAME_MOUSE_MODE_MOVE || (combat && gmouse_3d_get_mode() == GAME_MOUSE_MODE_ARROW)) {
        gmouse_3d_set_mode(wanted);
    }
}

// -----------------------------------------------------------------------------
// Modalità senza puntatore: oggetto evidenziato
// -----------------------------------------------------------------------------

static bool isSkillMode(int mode)
{
    return mode >= FIRST_GAME_MOUSE_MODE_SKILL && mode < GAME_MOUSE_MODE_COUNT;
}

static int skillForMode(int mode)
{
    switch (mode) {
    case GAME_MOUSE_MODE_USE_FIRST_AID:
        return SKILL_FIRST_AID;
    case GAME_MOUSE_MODE_USE_DOCTOR:
        return SKILL_DOCTOR;
    case GAME_MOUSE_MODE_USE_LOCKPICK:
        return SKILL_LOCKPICK;
    case GAME_MOUSE_MODE_USE_STEAL:
        return SKILL_STEAL;
    case GAME_MOUSE_MODE_USE_TRAPS:
        return SKILL_TRAPS;
    case GAME_MOUSE_MODE_USE_SCIENCE:
        return SKILL_SCIENCE;
    case GAME_MOUSE_MODE_USE_REPAIR:
        return SKILL_REPAIR;
    }
    return -1;
}

static void focusSetOutline(Object* object, bool on)
{
    if (object == nullptr) {
        return;
    }

    Rect rect;
    int rc = on ? obj_outline_object(object, OUTLINE_TYPE_ITEM, &rect) : obj_remove_outline(object, &rect);
    if (rc == 0) {
        tile_refresh_rect(&rect, object->elevation);
    }
}

// Toglie l'evidenziazione. Se l'oggetto non è più sulla mappa (raccolto,
// distrutto, cambio mappa) non lo tocca: il puntatore potrebbe non valere più.
static void focusClear(bool objectStillValid)
{
    if (gFocus != nullptr && gFocusOutlined && objectStillValid) {
        focusSetOutline(gFocus, false);
    }
    gFocus = nullptr;
    gFocusOutlined = false;
    gFocusManual = false;
    gFocusNameDue = 0;
    gTargetCurrent = nullptr;
}

static void focusSet(Object* object)
{
    if (object == gFocus) {
        return;
    }
#ifdef FALLOUT_AUTOTEST
    if (object != nullptr && PID_TYPE(object->pid) == OBJ_TYPE_SCENERY) {
        Proto* proto;
        proto_ptr(object->pid, &proto);
        printf("[pad] focus -> scenery pid %x tipo %d sid %d uso %d\n", object->pid, proto->scenery.type, object->sid, proto_action_can_use((object)->pid));
    } else {
        printf("[pad] focus -> %p (dude %p) pid %x\n", (void*)object, (void*)obj_dude, object ? object->pid : 0);
    }
#endif
    focusClear(true);
    gFocus = object;
    if (gFocus != nullptr && gFocus != obj_dude) {
        focusSetOutline(gFocus, true);
        gFocusOutlined = true;
        gFocusNameDue = SDL_GetTicks() + 400;
    }
}

static double angleBetween(double ax, double ay, double bx, double by)
{
    double la = sqrt(ax * ax + ay * ay);
    double lb = sqrt(bx * bx + by * by);
    if (la < 0.001 || lb < 0.001) {
        return 0.0;
    }
    double c = (ax * bx + ay * by) / (la * lb);
    return acos(std::clamp(c, -1.0, 1.0));
}

// Sceglie l'oggetto da evidenziare: vicino e nella direzione in cui ti muovi.
static void focusUpdate()
{
    gIncludeDude = isSkillMode(gmouse_3d_get_mode());

    std::vector<Candidate> candidates;
    collectCandidates(candidates, false);

    bool currentValid = false;
    for (const Candidate& candidate : candidates) {
        if (candidate.object == gFocus) {
            currentValid = true;
            break;
        }
    }

    if (gFocus != nullptr && !currentValid) {
        // È sparito dalla mappa: niente da ripulire.
        gFocusOutlined = false;
        focusClear(false);
    }

    if (gFocusManual && currentValid) {
        return;
    }

    int dudeX;
    int dudeY;
    tile_coord(obj_dude->tile, &dudeX, &dudeY, obj_dude->elevation);
    dudeX += 16;
    dudeY += 8;

    Object* best = nullptr;
    double bestScore = 1e9;
    double currentScore = 1e9;
    for (const Candidate& candidate : candidates) {
        int distance = candidate.object == obj_dude ? 0 : tile_dist(obj_dude->tile, candidate.object->tile);
        if (distance > 8) {
            continue;
        }
        double angle = candidate.object == obj_dude ? 0.0 : angleBetween(gFacingX, gFacingY, candidate.x - dudeX, candidate.y - dudeY);
        double score = distance + angle * 1.5;
        if (PID_TYPE(candidate.object->pid) == OBJ_TYPE_CRITTER && !critter_is_dead(candidate.object)) {
            score -= 2.0; // le persone sono la cosa con cui interagisci più spesso
        }
        if (candidate.object == obj_dude) {
            score = 3.0; // in modalità abilità si può scegliere anche se stessi
        }
        if (candidate.object == gFocus) {
            currentScore = score;
        }
        if (score < bestScore) {
            bestScore = score;
            best = candidate.object;
        }
    }

    // Isteresi: non saltare avanti e indietro tra due oggetti vicini.
    if (gFocus != nullptr && currentScore < 1e8 && bestScore > currentScore - 1.5) {
        return;
    }

    focusSet(best);
}

// Tiene il cursore (nascosto) sull'oggetto evidenziato, così le funzioni del
// gioco che guardano "cosa c'è sotto il cursore" vedono l'oggetto giusto.
static void focusParkCursor()
{
    int x = screenGetWidth() / 2;
    int y = screenGetHeight() - 20; // sulla barra dell'interfaccia
    if (gFocus != nullptr) {
        objectAimPoint(gFocus, &x, &y);
    }

    int mouseX;
    int mouseY;
    mouse_get_position(&mouseX, &mouseY);
    if (abs(mouseX - x) > 2 || abs(mouseY - y) > 2) {
        gWarpPending = true;
        gWarpX = x;
        gWarpY = y;
    }
}

// A: l'azione principale sull'oggetto evidenziato, come un clic con il
// cursore "usa" del gioco originale.
static void focusUse()
{
    Object* target = gFocus;
    if (target == nullptr) {
        return;
    }

    int mode = gmouse_3d_get_mode();
    if (isSkillMode(mode)) {
        int skill = skillForMode(mode);
        focusClear(true);
        if (action_use_skill_on(obj_dude, target, skill) != -1) {
            gmouse_set_cursor(MOUSE_CURSOR_NONE);
            gmouse_3d_set_mode(GAME_MOUSE_MODE_ARROW);
        }
        return;
    }

    if (mode == GAME_MOUSE_MODE_USE_CROSSHAIR) {
        Object* item;
        focusClear(true);
        if (intface_get_current_item(&item) != -1) {
            action_use_an_item_on_object(obj_dude, target, item);
        }
        gmouse_set_cursor(MOUSE_CURSOR_NONE);
        gmouse_3d_set_mode(GAME_MOUSE_MODE_ARROW);
        return;
    }

    switch (PID_TYPE(target->pid)) {
    case OBJ_TYPE_ITEM:
        focusClear(true);
        action_get_an_object(obj_dude, target);
        break;
    case OBJ_TYPE_CRITTER:
        if (target == obj_dude) {
            break;
        }
        if (obj_action_can_talk_to(target)) {
            action_talk_to(obj_dude, target);
        } else {
            action_loot_container(obj_dude, target);
        }
        break;
    case OBJ_TYPE_SCENERY:
        if (proto_action_can_use((target)->pid)) {
            action_use_an_object(obj_dude, target);
        } else if (obj_examine(obj_dude, target) == -1) {
            obj_look_at(obj_dude, target);
        }
        break;
    }
}

static void focusLook()
{
    if (gFocus != nullptr && obj_examine(obj_dude, gFocus) == -1) {
        obj_look_at(obj_dude, gFocus);
    }
}

// Stick destro: sposta la visuale.
static void cameraStickTick()
{
    double nx;
    double ny;
    double magnitude = stickRead(SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, &nx, &ny);
    if (magnitude <= 0.0) {
        return;
    }
    Uint32 now = SDL_GetTicks();
    Uint32 interval = (Uint32)(140 - 100 * magnitude);
    if (now - gLastScrollTick < interval) {
        return;
    }
    gLastScrollTick = now;
    int dx = fabs(nx) > 0.38 ? (nx > 0 ? 1 : -1) : 0;
    int dy = fabs(ny) > 0.38 ? (ny > 0 ? 1 : -1) : 0;
    map_scroll(dx, dy);
}

// -----------------------------------------------------------------------------
// Combattimento: esagono di destinazione scelto con lo stick
// -----------------------------------------------------------------------------

static void hexCenter(int tile, int* x, int* y)
{
    tile_coord(tile, x, y, obj_dude->elevation);
    *x += 16;
    *y += 8;
}

static void combatStickTick()
{
    double nx;
    double ny;
    double magnitude = stickRead(SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &nx, &ny);
    Uint32 now = SDL_GetTicks();

    if (magnitude < 0.5) {
        gCombatStickActive = false;
        return;
    }

    if (gCombatStickActive && now < gCombatHexNext) {
        return;
    }
    gCombatHexNext = now + (gCombatStickActive ? 170 : 300);
    gCombatStickActive = true;

    // Si parte dal personaggio se si è mosso o se non c'è una scelta.
    if (gCombatHex == -1 || gCombatHexOrigin != obj_dude->tile) {
        gCombatHex = obj_dude->tile;
        gCombatHexOrigin = obj_dude->tile;
    }

    // Tra i 6 lati dell'esagono quello più vicino alla direzione dello stick
    // (calcolato sullo schermo, così vale per qualsiasi orientamento).
    int cx;
    int cy;
    hexCenter(gCombatHex, &cx, &cy);
    int bestRotation = -1;
    double bestDot = -2.0;
    double secondDot = -2.0;
    int secondRotation = -1;
    for (int rotation = 0; rotation < 6; rotation++) {
        int neighbor = tile_num_in_direction(gCombatHex, rotation, 1);
        if (neighbor == -1) {
            continue;
        }
        int x;
        int y;
        hexCenter(neighbor, &x, &y);
        double vx = x - cx;
        double vy = y - cy;
        double length = sqrt(vx * vx + vy * vy);
        if (length < 1.0) {
            continue;
        }
        double dot = (vx * nx + vy * ny) / length;
        if (dot > bestDot) {
            secondDot = bestDot;
            secondRotation = bestRotation;
            bestDot = dot;
            bestRotation = rotation;
        } else if (dot > secondDot) {
            secondDot = dot;
            secondRotation = rotation;
        }
    }
    if (bestRotation == -1) {
        return;
    }
    // Su e giù cadono a metà tra due lati: si alterna per andare dritti.
    if (secondRotation != -1 && bestDot - secondDot < 0.08 && bestRotation == gCombatHexLastDir) {
        bestRotation = secondRotation;
    }
    gCombatHexLastDir = bestRotation;

    int next = tile_num_in_direction(gCombatHex, bestRotation, 1);
    if (next == -1) {
        return;
    }
    gCombatHex = next;

    gmouse_3d_set_mode(GAME_MOUSE_MODE_MOVE);
    gWarpPending = true;
    hexCenter(gCombatHex, &gWarpX, &gWarpY);
}

// -----------------------------------------------------------------------------
// Movimento diretto con lo stick
// -----------------------------------------------------------------------------

static int stickTargetTile(double nx, double ny)
{
    int dudeX;
    int dudeY;
    if (tile_coord(obj_dude->tile, &dudeX, &dudeY, obj_dude->elevation) == -1) {
        return -1;
    }
    dudeX += 16;
    dudeY += 8;

    // Prova a 6, 5, 4, 3, 2 esagoni di distanza (tratti lunghi = meno ripartenze); la griglia isometrica è
    // schiacciata in verticale, quindi la componente y vale meno.
    static const int distances[] = { 192, 160, 128, 96, 64, 40 };
    for (int distance : distances) {
        int x = dudeX + (int)(nx * distance);
        int y = dudeY + (int)(ny * distance * 0.75);
        int tile = tile_num(x, y, obj_dude->elevation);
        if (tile == -1 || tile == obj_dude->tile) {
            continue;
        }
        if (obj_blocking_at(obj_dude, tile, obj_dude->elevation) != nullptr) {
            continue;
        }
        return tile;
    }
    return -1;
}

static void issueMove(int tile, bool run)
{
#ifdef FALLOUT_AUTOTEST
    printf("[pad] muovi verso %d da %d (corsa %d)\n", tile, obj_dude->tile, run);
#endif
    register_clear(obj_dude);
    if (register_begin(ANIMATION_REQUEST_RESERVED) == -1) {
        return;
    }
    if (run) {
        register_object_run_to_tile(obj_dude, tile, obj_dude->elevation, -1, 0);
    } else {
        register_object_move_to_tile(obj_dude, tile, obj_dude->elevation, -1, 0);
    }
    register_end();
}

// Visuale che segue il personaggio con passi piccoli e continui (come lo
// scorrimento ai bordi del gioco) invece di un salto di un quarto di schermo:
// niente scatti quando si passa da una zona all'altra della mappa.
static Uint32 gFollowUntil = 0;

static void followCamera()
{
    if (!gMobile.cameraFollow) {
        return;
    }

    Rect view;
    if (!isoWindowRect(&view)) {
        return;
    }

    int x;
    int y;
    if (tile_coord(obj_dude->tile, &x, &y, obj_dude->elevation) == -1) {
        return;
    }
    x += 16;
    y += 8;

    int width = view.lrx - view.ulx;
    int height = view.lry - view.uly;
    int dx = x - (view.ulx + width / 2);
    int dy = y - (view.uly + height / 2);
    int deadX = width / 7;
    int deadY = height / 7;

    int stepX = dx > deadX ? 1 : (dx < -deadX ? -1 : 0);
    int stepY = dy > deadY ? 1 : (dy < -deadY ? -1 : 0);
    if (stepX == 0 && stepY == 0) {
        return;
    }

    // Personaggio uscito del tutto dalla vista (cambio piano, teletrasporto):
    // lì conviene ricentrare subito.
    if (abs(dx) > width / 2 || abs(dy) > height / 2) {
        tile_scroll_to(obj_dude->tile, 2);
        return;
    }

    bool wasVisible = gmouse_3d_is_on();
    map_scroll(stepX, stepY);
    if (wasVisible && !gmouse_3d_is_on()) {
        gmouse_3d_on();
    }
}

static void stickMovementTick()
{
    double nx;
    double ny;
    double magnitude = stickRead(SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &nx, &ny);
    Uint32 now = SDL_GetTicks();

    if (magnitude <= 0.0) {
        if (gStickWasMoving) {
            gStickWasMoving = false;
            gStickLastTarget = -1;
            // Fermati sull'esagono successivo, senza scatti.
            if (anim_busy(obj_dude)) {
                int next = tile_num_in_direction(obj_dude->tile, obj_dude->rotation, 1);
                if (next != -1 && obj_blocking_at(obj_dude, next, obj_dude->elevation) == nullptr) {
                    issueMove(next, false);
                } else {
                    register_clear(obj_dude);
                }
            }
        }
        return;
    }

    if (!gPadMode) {
        gmouse_3d_set_mode(GAME_MOUSE_MODE_MOVE);
    }
    gFacingX = nx;
    gFacingY = ny;
    gFocusManual = false;

    bool run = magnitude > 0.8;
    double angle = atan2(ny, nx);
    double angleDelta = fabs(angle - gStickLastAngle);
    if (angleDelta > kPi) {
        angleDelta = 2 * kPi - angleDelta;
    }

    bool directionChanged = angleDelta > 0.6; // ~35 gradi
    bool nearTarget = gStickLastTarget == -1 || tile_dist(obj_dude->tile, gStickLastTarget) <= 3;
    bool idle = !anim_busy(obj_dude);
    bool gaitChanged = run != gStickRunning;

    if ((nearTarget || directionChanged || idle || gaitChanged) && now - gStickLastIssue >= 120) {
        int tile = stickTargetTile(nx, ny);
        if (tile != -1 && (tile != gStickLastTarget || idle)) {
            issueMove(tile, run);
            gStickLastTarget = tile;
            gStickLastIssue = now;
            gStickLastAngle = angle;
            gStickRunning = run;
        }
    }

    gStickWasMoving = true;
    gFollowUntil = now + 1500;
}

// Toglie l'evidenziazione controllando prima che l'oggetto sia ancora sulla
// mappa.
static void focusUpdateValidityAndClear()
{
    std::vector<Candidate> candidates;
    gIncludeDude = true;
    collectCandidates(candidates, false);
    bool valid = false;
    for (const Candidate& candidate : candidates) {
        if (candidate.object == gFocus) {
            valid = true;
            break;
        }
    }
    focusClear(valid);
}

void gamepadPointerUsed()
{
    if (gPadMode) {
        gPadMode = false;
    }
}

void gamepadGameTick(bool isInCombatMode)
{
    mobileSettingsLoad();
    gLastGameTick = SDL_GetTicks();

    if (gControllerJustConnected && obj_dude != nullptr) {
        gControllerJustConnected = false;
        char message[] = "Controller collegato.";
        display_print(message);
    }

    if (gController == nullptr || !mapIsActive()) {
        gPendingTargetStep = 0;
        gPendingCancelMode = false;
        gPendingUse = false;
        gPendingLook = false;
        return;
    }

    if (gEndTurnHeldSince != 0 && !gEndCombatSent && SDL_GetTicks() - gEndTurnHeldSince >= kEndCombatHoldMs) {
        gEndCombatSent = true;
        if (isInCombatMode) {
            inputInjectKey(SDL_SCANCODE_RETURN, true);
            inputInjectKey(SDL_SCANCODE_RETURN, false);
        }
    }

    // Fine combattimento: si torna all'esplorazione col controller, senza
    // esagono rosso né mirino rimasti appesi.
    static bool wasInCombat = false;
    if (wasInCombat && !isInCombatMode) {
        gCombatHex = -1;
        gCombatStickActive = false;
        gTargetCurrent = nullptr;
        gWarpPending = false;
        if (gLastPadInput != 0 && SDL_GetTicks() - gLastPadInput < 60000) {
            gPadMode = true;
        }
    }
    wasInCombat = isInCombatMode;

    bool pointerless = gPadMode && !isInCombatMode;

    if (gPendingCancelMode) {
        gPendingCancelMode = false;
        gTargetCurrent = nullptr;
        gmouse_set_cursor(MOUSE_CURSOR_NONE);
        gmouse_3d_set_mode(pointerless ? GAME_MOUSE_MODE_ARROW : GAME_MOUSE_MODE_MOVE);
        gCombatHex = -1;
    }

    if (!pointerless) {
        // In combattimento il gioco gestisce i contorni dei personaggi.
        if (gFocus != nullptr) {
            focusUpdateValidityAndClear();
        }
        if (gWeHidMapCursor) {
            gmouse_3d_on();
            gWeHidMapCursor = false;
        }
    } else {
        if (!gMenuHold && gmouse_3d_is_on()) {
            gmouse_3d_off();
            gWeHidMapCursor = true;
        }
        // Senza puntatore niente cursore "movimento" con l'esagono rosso.
        if (gmouse_3d_get_mode() == GAME_MOUSE_MODE_MOVE || gmouse_3d_get_mode() == GAME_MOUSE_MODE_CROSSHAIR) {
            gmouse_3d_set_mode(GAME_MOUSE_MODE_ARROW);
        }
        if (!gMenuHold) {
            focusUpdate();
            focusParkCursor();
        }
        // Nome nella console ("You see: ..."), solo quando l'evidenziazione
        // resta ferma un attimo: niente elenco di nomi mentre cammini.
        if (gFocusNameDue != 0 && SDL_GetTicks() >= gFocusNameDue) {
            gFocusNameDue = 0;
            if (gFocus != nullptr && gFocus != obj_dude) {
                obj_look_at(obj_dude, gFocus);
            }
        }
        cameraStickTick();
    }

    if (gPendingTargetStep != 0) {
        if (pointerless) {
            gTargetCurrent = gFocus;
        }
        cycleTarget(gPendingTargetStep, isInCombatMode);
        if (pointerless) {
            focusSet(gTargetCurrent);
            gFocusManual = gFocus != nullptr;
        }
        gPendingTargetStep = 0;
    }

    if (gPendingUse) {
        gPendingUse = false;
        if (pointerless) {
            focusUse();
        }
    }

    if (gPendingLook) {
        gPendingLook = false;
        if (pointerless) {
            focusLook();
        }
    }

    if (isInCombatMode && gPadMode) {
        combatStickTick();
        cameraStickTick();
    }

    if (!isInCombatMode && gMobile.stickMovement && !game_ui_is_disabled()) {
        stickMovementTick();
        if (SDL_GetTicks() < gFollowUntil) {
            if (anim_busy(obj_dude)) {
                gFollowUntil = SDL_GetTicks() + 300;
            }
            followCamera();
        }
    } else {
        gStickWasMoving = false;
        gStickLastTarget = -1;
    }
}

} // namespace fallout
