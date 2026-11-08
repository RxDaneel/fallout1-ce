// Banco di prova automatico (solo build di sviluppo, -DFALLOUT_AUTOTEST=ON).
//
// Legge uno script dal file indicato in FALLOUT_AUTOTEST e lo esegue mentre
// il gioco gira (anche senza schermo, SDL_VIDEODRIVER=offscreen):
//
//   wait MS                 aspetta
//   key NOME                preme e rilascia un tasto (nome scancode SDL)
//   keydown NOME / keyup NOME
//   mouse X Y               sposta il cursore
//   click X Y [right]       clic
//   pad PULSANTE [down|up]  pulsante del controller virtuale (a, b, x, y,
//                           leftshoulder, dpup, start, back, ...); senza
//                           down/up = premi e rilascia
//   axis ASSE VALORE        asse del controller (leftx, lefty, rightx,
//                           righty, lefttrigger, righttrigger), -32768..32767
//   shot NOME               salva NOME.bmp nella cartella FALLOUT_SHOTS
//   log TESTO               scrive su stdout
//   quit                    esce
//
// Righe vuote e righe che iniziano con # sono ignorate.

#ifdef FALLOUT_AUTOTEST

#include "game/autotest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "plib/gnw/input.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/svga.h"

namespace fallout {

static std::vector<std::string> gLines;
static size_t gLine = 0;
static bool gLoaded = false;
static Uint32 gWaitUntil = 0;

static SDL_Joystick* gVirtualJoystick = nullptr;
static int gVirtualIndex = -1;

static bool gMousePending = false;
static int gMouseX = 0;
static int gMouseY = 0;
static int gClickFrames = 0;
static int gClickButtons = 0;
static int gReleaseFrames = 0;
static int gPadTapButton = -1;
static int gPadTapFrames = 0;

static void attachVirtualController()
{
    // Senza schermo la finestra non ha mai il focus: SDL ignorerebbe il
    // controller.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    SDL_VirtualJoystickDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.vendor_id = 0x045e; // aspetto di un pad Xbox
    desc.product_id = 0x028e;
    desc.name = "Controller di prova";

    gVirtualIndex = SDL_JoystickAttachVirtualEx(&desc);
    if (gVirtualIndex < 0) {
        printf("[autotest] controller virtuale non disponibile: %s\n", SDL_GetError());
        return;
    }

    gVirtualJoystick = SDL_JoystickOpen(gVirtualIndex);
    printf("[autotest] controller virtuale collegato (indice %d, gamecontroller=%d)\n",
        gVirtualIndex, SDL_IsGameController(gVirtualIndex));
}

static void load()
{
    gLoaded = true;
    const char* path = getenv("FALLOUT_AUTOTEST");
    if (path == nullptr) {
        return;
    }

    FILE* stream = fopen(path, "rt");
    if (stream == nullptr) {
        printf("[autotest] script non trovato: %s\n", path);
        return;
    }

    char buffer[512];
    while (fgets(buffer, sizeof(buffer), stream) != nullptr) {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        gLines.push_back(line);
    }
    fclose(stream);

    printf("[autotest] %zu comandi da %s\n", gLines.size(), path);
    attachVirtualController();
}

static void screenshot(const char* name)
{
    const char* dir = getenv("FALLOUT_SHOTS");
    std::string path = std::string(dir != nullptr ? dir : ".") + "/" + name + ".bmp";
    if (gSdlSurface != nullptr && SDL_SaveBMP(gSdlSurface, path.c_str()) == 0) {
        printf("[autotest] screenshot %s\n", path.c_str());
    } else {
        printf("[autotest] screenshot fallito: %s\n", SDL_GetError());
    }
    fflush(stdout);
}

static void key(const char* name, bool down)
{
    SDL_Scancode scancode = SDL_GetScancodeFromName(name);
    if (scancode == SDL_SCANCODE_UNKNOWN) {
        printf("[autotest] tasto sconosciuto: %s\n", name);
        return;
    }
    inputInjectKey(scancode, down);
}

static void padButton(const char* name, int state)
{
    if (gVirtualJoystick == nullptr) {
        return;
    }
    SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(name);
    if (button == SDL_CONTROLLER_BUTTON_INVALID) {
        printf("[autotest] pulsante sconosciuto: %s\n", name);
        return;
    }
    if (state == 2) {
        SDL_JoystickSetVirtualButton(gVirtualJoystick, button, 1);
        gPadTapButton = button;
        gPadTapFrames = 3;
    } else {
        SDL_JoystickSetVirtualButton(gVirtualJoystick, button, state);
    }
}

static void padAxis(const char* name, int value)
{
    if (gVirtualJoystick == nullptr) {
        return;
    }
    SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(name);
    if (axis == SDL_CONTROLLER_AXIS_INVALID) {
        printf("[autotest] asse sconosciuto: %s\n", name);
        return;
    }
    SDL_JoystickSetVirtualAxis(gVirtualJoystick, axis, (Sint16)value);
}

void autotestTick()
{
    if (!gLoaded) {
        load();
    }

    if (gPadTapFrames > 0 && --gPadTapFrames == 0 && gVirtualJoystick != nullptr) {
        SDL_JoystickSetVirtualButton(gVirtualJoystick, gPadTapButton, 0);
    }

    // Un comando che coinvolge il mouse alla volta.
    if (gClickFrames > 0 || gReleaseFrames > 0) {
        return;
    }

    while (gLine < gLines.size() && SDL_GetTicks() >= gWaitUntil) {
        const std::string& line = gLines[gLine++];
        char command[32] = { 0 };
        char arg1[128] = { 0 };
        char arg2[128] = { 0 };
        char arg3[128] = { 0 };
        sscanf(line.c_str(), "%31s %127s %127s %127s", command, arg1, arg2, arg3);

        if (strcmp(command, "wait") == 0) {
            gWaitUntil = SDL_GetTicks() + atoi(arg1);
            return;
        } else if (strcmp(command, "key") == 0) {
            key(arg1, true);
            key(arg1, false);
        } else if (strcmp(command, "keydown") == 0) {
            key(arg1, true);
        } else if (strcmp(command, "keyup") == 0) {
            key(arg1, false);
        } else if (strcmp(command, "mouse") == 0) {
            gMousePending = true;
            gMouseX = atoi(arg1);
            gMouseY = atoi(arg2);
            return;
        } else if (strcmp(command, "click") == 0) {
            gMousePending = true;
            gMouseX = atoi(arg1);
            gMouseY = atoi(arg2);
            gClickButtons = strcmp(arg3, "right") == 0 ? MOUSE_STATE_RIGHT_BUTTON_DOWN : MOUSE_STATE_LEFT_BUTTON_DOWN;
            gClickFrames = 3;
            return;
        } else if (strcmp(command, "pad") == 0) {
            int state = 2;
            if (strcmp(arg2, "down") == 0) state = 1;
            if (strcmp(arg2, "up") == 0) state = 0;
            padButton(arg1, state);
            if (state == 2) {
                return; // lascia passare qualche frame
            }
        } else if (strcmp(command, "axis") == 0) {
            padAxis(arg1, atoi(arg2));
        } else if (strcmp(command, "shot") == 0) {
            screenshot(arg1);
        } else if (strcmp(command, "log") == 0) {
            printf("[autotest] %s\n", line.c_str() + 4);
            fflush(stdout);
        } else if (strcmp(command, "quit") == 0) {
            printf("[autotest] fine\n");
            fflush(stdout);
            exit(0);
        } else {
            printf("[autotest] comando sconosciuto: %s\n", line.c_str());
        }
    }
}

void autotestGetMouseInput(int* dx, int* dy, int* buttons)
{
    *dx = 0;
    *dy = 0;
    *buttons = 0;

    if (gMousePending) {
        int x;
        int y;
        mouse_get_position(&x, &y);
        *dx = gMouseX - x;
        *dy = gMouseY - y;
        gMousePending = false;
    }

    if (gClickFrames > 0) {
        gClickFrames--;
        *buttons = gClickButtons;
        if (gClickFrames == 0) {
            gReleaseFrames = 3;
        }
    } else if (gReleaseFrames > 0) {
        gReleaseFrames--;
    }
}

} // namespace fallout

#endif
