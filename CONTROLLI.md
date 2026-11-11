# Controlli mobile

Versione di Fallout Community Edition pensata per giocare con un controller
(Retroid Pocket 5, pad Bluetooth su iPhone) o con il tocco. Funziona allo
stesso modo in Fallout e Fallout 2.

Con il controller non si guida mai un puntatore: sulla mappa il gioco
evidenzia da solo con cosa interagire, nei menu la croce salta da un pulsante
all'altro, in combattimento si scelgono bersagli ed esagoni.

## Sulla mappa

Lo stick sinistro sposta un esagono, un passo alla volta come in
combattimento; A porta il personaggio lì (cammina se è vicino, corre se è
lontano). Se sull'esagono c'è una persona o un oggetto, viene evidenziato
(contorno giallo) e A ci interagisce: parla con chi ha qualcosa da dire,
esamina le persone senza dialogo, e con animali e creature fa partire il
combattimento con loro già nel mirino (poi A spara). Sopra il
pannello una barra mostra i tasti utili in quel momento (es. "A Parla con
Seth · X Esamina · Y Inventario"; in combattimento "A Attacca · RB Bersaglio ·
RT Fine turno").

| Comando | Azione |
|---|---|
| Stick sinistro | Sposta l'esagono di arrivo (più lo spingi, più va veloce); la visuale lo segue |
| Stick destro | Sposta la visuale |
| A | Vai all'esagono, oppure azione sull'oggetto evidenziato: parla, apri, perquisisci, raccogli |
| A tenuto premuto | Menu azioni (guarda, usa, oggetto, abilità...): croce ↑↓ per scegliere, rilascia A per confermare |
| X | Esamina l'oggetto evidenziato |
| LB / RB | Salta all'oggetto precedente / successivo attorno a te |
| B | Annulla: toglie l'esagono (o l'abilità scelta dallo Skilldex) |
| Y | Inventario |
| Croce ↑ ↓ ← → | Pip-Boy, Personaggio, Abilità (Skilldex), Automappa |
| LT | Cambia mano attiva |
| RT | Inizia il combattimento |
| L3 | Centra la visuale sul personaggio |
| R3 | Cambia modalità d'attacco dell'arma |
| Start | Menu di gioco |
| Select + A / Y | Salvataggio / caricamento rapido |
| Select + ↑ | Riposa (orologio del Pip-Boy) |

Abilità: scegli l'abilità dallo Skilldex (croce ←), evidenzia il bersaglio con
LB/RB (puoi scegliere anche te stesso, per il pronto soccorso) e premi A.

## In combattimento

| Comando | Azione |
|---|---|
| LB / RB | Nemico precedente / successivo (dal più vicino), con il mirino e la probabilità di colpire |
| Stick sinistro | Sceglie l'esagono di destinazione, un passo alla volta; mostra i punti azione necessari |
| A | Attacca il nemico scelto, oppure vai all'esagono scelto |
| B | Annulla la scelta |
| X | Cambia cursore (movimento / attacco / usa) |
| R3 | Modalità d'attacco (es. colpo mirato: la parte del corpo si sceglie con la croce) |
| RT (tocco) | Fine turno |
| RT (tenuto premuto) | Termina il combattimento (se non ci sono nemici vicini) |
| Select + RT | Termina il combattimento (alternativa) |
| Stick destro | Sposta la visuale |
| Y | Inventario (per usare oggetti o cambiare arma) |

## Menu, finestre e dialoghi

Inventario, bottino, scambio, Pip-Boy, scheda personaggio, Skilldex, opzioni,
salvataggi, creazione del personaggio, dialoghi, mappa del mondo.

| Comando | Azione |
|---|---|
| Croce o stick sinistro | Salta al pulsante sopra / sotto / a sinistra / a destra |
| A | Premi il pulsante (tieni premuto per trascinare: es. un oggetto dall'inventario alla mano, sposta con la croce e rilascia) |
| B o Start | Indietro / chiudi |
| Y | Conferma (Invio) |
| X | Clic destro |
| LB / RB, LT / RT | Scorri la lista / pagina precedente e successiva |
| Stick destro | Cursore libero, per i rari punti che non sono pulsanti |

Nelle liste (abilità, note del Pip-Boy...) su e giù scorrono le righe.

Casi particolari:

- **Scheda personaggio**: in Fallout 2 la croce sceglie caratteristiche e
  abilità; in Fallout 1 su/giù scorrono le righe e A seleziona. In entrambi
  sinistra/destra tolgono/aggiungono punti. I pulsanti (Fatto, Annulla...) si
  raggiungono con lo stick sinistro; Y conferma, B annulla.
- **Salvataggi**: su/giù scelgono lo slot, Y conferma.
- **Dialoghi**: la selezione parte dalla prima risposta; Barter e Review si
  raggiungono con la croce.
- **Mappa del mondo**: la croce salta tra le città conosciute; lo stick
  sinistro muove un cursore libero per viaggiare verso qualsiasi punto (A).

## Tocco

Se tocchi lo schermo il gioco passa ai comandi touch (e torna al controller
appena usi il pad).

| Gesto | Azione |
|---|---|
| Tocco | Clic intelligente: oggetto = usa/parla/raccogli, pavimento = cammina |
| Tocco su un nemico (in combattimento) | Primo tocco: mirino. Secondo tocco: attacca |
| Tieni premuto | Menu azioni (trascina per scegliere); nell'inventario trascina gli oggetti |
| Trascina con un dito | Sposta il cursore senza cliccare |
| Tocco con due dita | Cambia cursore |
| Trascina con due dita | Scorre la mappa |

Il tasto Indietro di Android fa Esc.

## Impostazioni

In `fallout2.cfg` (Fallout 1: `fallout.cfg`), sezione `[mobile]`:

```ini
[mobile]
touch_mode=1              ; 1 = diretto, 0 = trackpad (come la versione originale)
touch_snap=1              ; aggancio degli oggetti vicini al tocco
gamepad_cursor_speed=100  ; velocità del cursore libero, in % (25-400)
gamepad_deadzone=20       ; zona morta degli stick, in % (5-60)
camera_follow=1           ; la visuale segue il personaggio
stick_movement=1          ; 1 = stick sinistro muove il personaggio, 0 = cursore
hex_movement=1            ; 1 = scegli l'esagono e premi A, 0 = il personaggio segue lo stick
button_hints=1            ; 1 = barra con i tasti da premere sopra il pannello, 0 = nascosta
```

In f1_res.ini / f2_res.ini, sezione `[IFACE]`, `IFACE_BAR_MODE=1` fa continuare la mappa
dietro il pannello: niente zone nere ai lati sugli schermi larghi (l'installer lo attiva da solo).
