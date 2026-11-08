# Controlli mobile

Versione di Fallout Community Edition con controller e tocco ripensati per
iPhone e Android (testata pensando a iPhone 14 Pro e Retroid Pocket 5).
Funziona allo stesso modo in Fallout e Fallout 2.

## Controller

Il controller cambia comportamento in base a dove sei: sulla mappa, in
combattimento, in un dialogo o in un menu (inventario, Pip-Boy, scheda
personaggio, mappa del mondo).

### Sulla mappa (esplorazione)

| Comando | Azione |
|---|---|
| Stick sinistro | Muove il personaggio (a fondo corsa corre) |
| Stick destro | Cursore |
| A | Clic intelligente: su un oggetto lo usa, parla, raccoglie; sul pavimento cammina. Tieni premuto per il menu azioni |
| B | Annulla: torna al cursore di movimento |
| X | Cambia cursore (movimento / usa / mira), come il tasto destro |
| Y | Inventario |
| LB / RB | Aggancia l'oggetto interattivo precedente / successivo attorno al personaggio |
| LT | Cambia mano attiva |
| RT | Inizia il combattimento |
| Croce ↑ ↓ ← → | Pip-Boy, Personaggio, Abilità (Skilldex), Automappa |
| L3 | Centra la visuale sul personaggio |
| R3 | Cambia modalità d'attacco dell'arma |
| Start | Menu di gioco |
| Select + A | Salvataggio rapido |
| Select + Y | Caricamento rapido |
| Select + ↑ | Riposa (orologio del Pip-Boy) |

### In combattimento

Come sopra, con queste differenze:

| Comando | Azione |
|---|---|
| Stick sinistro | Cursore (il combattimento è a turni, ci si muove scegliendo l'esagono) |
| LB / RB | Aggancia il nemico precedente / successivo, dal più vicino, con il mirino già pronto |
| A | Primo clic su un nemico: mirino e probabilità di colpire. Secondo clic: attacca |
| RT | Fine turno |
| Select + RT (o Select + RB) | Termina il combattimento |

### Dialoghi

| Comando | Azione |
|---|---|
| Croce ↑ ↓ | Passa da una risposta all'altra |
| Croce ← → | Pagina precedente / successiva del testo |
| A | Sceglie la risposta |
| B | Esci / indietro |

### Menu e finestre

| Comando | Azione |
|---|---|
| Stick sinistro | Cursore |
| Stick destro | Scorre le liste |
| A / X | Clic / clic destro |
| B o Start | Indietro (Esc) |
| Y | Conferma (Invio) |
| Croce | Frecce |
| LB / RB, LT / RT | Pagina su / giù |

## Tocco

Con il tocco "diretto" (predefinito) il cursore va dove tocchi.

| Gesto | Azione |
|---|---|
| Tocco | Clic intelligente: oggetto = usa/parla/raccogli, pavimento = cammina |
| Tocco su un nemico (in combattimento) | Primo tocco: mirino. Secondo tocco: attacca |
| Tieni premuto | Menu azioni (trascina per scegliere); nell'inventario trascina gli oggetti |
| Trascina con un dito | Sposta il cursore senza cliccare (mostra nomi e probabilità di colpire) |
| Tocco con due dita | Cambia cursore (come il tasto destro) |
| Trascina con due dita | Scorre la mappa |

Il tocco "aggancia" gli oggetti vicini: non serve centrare la figura al
pixel. Il tasto Indietro di Android fa Esc.

## Impostazioni

In `fallout.cfg` (Fallout 2: `fallout2.cfg`), sezione `[mobile]`:

```ini
[mobile]
touch_mode=1              ; 1 = diretto, 0 = trackpad (come la versione originale)
touch_snap=1              ; aggancio degli oggetti vicini al tocco
gamepad_cursor_speed=100  ; velocità del cursore, in % (25-400)
gamepad_deadzone=20       ; zona morta degli stick, in % (5-60)
camera_follow=1           ; la visuale segue il personaggio
stick_movement=1          ; 1 = stick sinistro muove il personaggio, 0 = cursore
```
