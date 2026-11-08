#include "game/gamepad.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>

#include <algorithm>
#include <vector>

#include "game/anim.h"
#include "game/combat.h"
#include "game/config.h"
#include "game/critter.h"
#include "game/display.h"
#include "game/game.h"
#include "game/gconfig.h"
#include "game/gdialog.h"
#include "game/gmouse.h"
#include "game/map.h"
#include "game/object.h"
#include "game/party.h"
#include "game/proto.h"
#include "game/tile.h"
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
            return act(ACTION_MOUSE_LEFT);
        case SDL_CONTROLLER_BUTTON_B:
            return act(ACTION_CANCEL_MODE);
        case SDL_CONTROLLER_BUTTON_X:
            return act(ACTION_MOUSE_RIGHT);
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
        switch (button) {
        case SDL_CONTROLLER_BUTTON_A:
            return act(ACTION_MOUSE_LEFT);
        case SDL_CONTROLLER_BUTTON_X:
            return act(ACTION_MOUSE_RIGHT);
        case SDL_CONTROLLER_BUTTON_B:
        case SDL_CONTROLLER_BUTTON_START:
            return keyTap(SDL_SCANCODE_ESCAPE);
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return act(ACTION_DIALOG_PREV);
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return act(ACTION_DIALOG_NEXT);
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return key(SDL_SCANCODE_UP); // pagina precedente del testo
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return key(SDL_SCANCODE_DOWN); // pagina successiva del testo
        case SDL_CONTROLLER_BUTTON_Y:
            return keyTap(SDL_SCANCODE_RETURN);
        }
        break;
    case CONTEXT_WINDOW:
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
            return key(SDL_SCANCODE_UP);
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return key(SDL_SCANCODE_DOWN);
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return key(SDL_SCANCODE_LEFT);
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return key(SDL_SCANCODE_RIGHT);
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            return key(SDL_SCANCODE_PAGEUP);
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            return key(SDL_SCANCODE_PAGEDOWN);
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

    if (context == CONTEXT_WINDOW) {
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
        if (mapIsActive()) {
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
    default:
        break;
    }
    binding->action = ACTION_NONE;
}

static bool gSelectUsedAsModifier = false;
static bool gTriggerDown[2] = { false, false };

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
        if (event->caxis.which != gControllerId) {
            break;
        }
        gAxis[event->caxis.axis] = event->caxis.value;

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
                } else {
                    binding = triggerBindingFor(right, currentContext());
                }
                bindingPress(slot, binding);
            } else if (gTriggerDown[right] && event->caxis.value < 8000) {
                gTriggerDown[right] = false;
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

    // Stick che muove il cursore: sulla mappa fuori dal combattimento lo
    // stick sinistro muove il personaggio, quindi il cursore è sul destro.
    bool leftMovesDude = context == CONTEXT_MAP && gMobile.stickMovement;

    double cx = 0.0;
    double cy = 0.0;
    double cursorMagnitude = 0.0;

    double nx;
    double ny;
    double left = stickRead(SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &nx, &ny);
    if (!leftMovesDude && left > 0.0) {
        cx = nx;
        cy = ny;
        cursorMagnitude = left;
    }

    double rx;
    double ry;
    double right = stickRead(SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, &rx, &ry);
    if (right > 0.0) {
        if (context == CONTEXT_WINDOW || context == CONTEXT_DIALOG) {
            // Nei menu lo stick destro scorre le liste.
            Uint32 interval = (Uint32)(260 - 200 * right);
            if (now - gLastWheelTick >= interval && fabs(ry) > 0.5) {
                *wheelY = ry < 0 ? 1 : -1;
                gLastWheelTick = now;
            }
        } else if (right > cursorMagnitude) {
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

static bool isInteresting(Object* object, bool combatOnly)
{
    if (object == nullptr || object == obj_dude) {
        return false;
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
        return object->sid != -1; // oggetti con script: computer, interruttori...
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

static void cycleTarget(int step, bool combat)
{
    std::vector<Candidate> candidates;
    collectCandidates(candidates, combat);
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
    gWarpX = candidates[index].x;
    gWarpY = candidates[index].y;

    // Cursore giusto per l'oggetto: mirino sui nemici in combattimento,
    // freccia "usa" sugli oggetti in esplorazione.
    int wanted = combat ? GAME_MOUSE_MODE_CROSSHAIR : GAME_MOUSE_MODE_ARROW;
    if (gmouse_3d_get_mode() == GAME_MOUSE_MODE_MOVE || (combat && gmouse_3d_get_mode() == GAME_MOUSE_MODE_ARROW)) {
        gmouse_3d_set_mode(wanted);
    }
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

    // Prova a 4, 3, 2 esagoni di distanza; la griglia isometrica è
    // schiacciata in verticale, quindi la componente y vale meno.
    static const int distances[] = { 128, 96, 64, 40 };
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

    int width = view.lrx - view.ulx;
    int height = view.lry - view.uly;
    if (x < view.ulx + width / 4 || x > view.lrx - width / 4
        || y < view.uly + height / 4 || y > view.lry - height / 4) {
        tile_scroll_to(obj_dude->tile, 2);
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

    gmouse_3d_set_mode(GAME_MOUSE_MODE_MOVE);

    bool run = magnitude > 0.8;
    double angle = atan2(ny, nx);
    double angleDelta = fabs(angle - gStickLastAngle);
    if (angleDelta > kPi) {
        angleDelta = 2 * kPi - angleDelta;
    }

    bool directionChanged = angleDelta > 0.6; // ~35 gradi
    bool nearTarget = gStickLastTarget == -1 || tile_dist(obj_dude->tile, gStickLastTarget) <= 2;
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
    followCamera();
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
        return;
    }

    if (gPendingCancelMode) {
        gPendingCancelMode = false;
        gTargetCurrent = nullptr;
        gmouse_3d_set_mode(GAME_MOUSE_MODE_MOVE);
    }

    if (gPendingTargetStep != 0) {
        cycleTarget(gPendingTargetStep, isInCombatMode);
        gPendingTargetStep = 0;
    }

    if (!isInCombatMode && gMobile.stickMovement && !game_ui_is_disabled()) {
        stickMovementTick();
    } else {
        gStickWasMoving = false;
        gStickLastTarget = -1;
    }
}

} // namespace fallout
