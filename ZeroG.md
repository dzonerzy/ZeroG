# ZeroG — Documentazione tecnica

Oct 28, 2026 · @Daniele

## Panoramica

L'obiettivo è un motore web basato su WebKit, open source, con una API C piccola e stabile, distribuibile come **singolo eseguibile** su Linux, Windows e macOS. In pratica: un "Ultralight open source", con rendering offscreen e presentazione controllata dall'applicazione.

Il progetto si chiama **ZeroG**, e l'API pubblica usa il prefisso `zg_`. Il nome del framework Zig costruito sopra è ancora da decidere (proposta: Capsule).

Il nome non contiene "WebKit", che è un [marchio registrato di Apple](https://en.wikipedia.org/wiki/WebKit). Prima della pubblicazione va fatta una ricerca sui marchi anche in EUIPO: negli USA "ZERO-G" risulta già registrato in classe 9, per prodotti diversi dal software, e il pacchetto npm `zero-g` è occupato.

### Obiettivi

- Motore web completo (HTML, CSS, JavaScript via JavaScriptCore), compatibile con i frontend moderni (React, Vue, Svelte, Vite).
- Comportamento identico su tutte le piattaforme: stesso motore, stessa versione, nessuna dipendenza dalla webview di sistema.
- Distribuzione come singolo binario, con linking statico dove la licenza lo consente.
- Rendering offscreen: il motore produce frame, l'applicazione decide dove presentarli (finestra, texture di un gioco, overlay).
- API C stabile, utilizzabile da qualsiasi linguaggio; Zig come primo cliente.
- Single-process e binding JS native: motore e applicazione nello stesso processo, con funzioni dell'host esposte direttamente come oggetti JavaScript, senza IPC.

### Non obiettivi

- Non è un browser: niente tab, estensioni, sincronizzazione, UI di navigazione.
- Non è pensato per navigare il web aperto in modo sicuro come un browser moderno; il caso d'uso principale è contenuto dell'applicazione (vedi Sicurezza).
- Nessun supporto mobile nella prima fase.
- Nessun fork di WebCore e JavaScriptCore: il motore aggiunge un port e un livello di embedding propri, e le modifiche al codice upstream restano patch minime.

### Casi d'uso target

- Framework desktop alla Wails o Tauri che vogliono un motore unico invece delle webview di sistema.
- UI di giochi e overlay renderizzati in una texture.
- Tool desktop che devono funzionare su Linux senza dipendere dalla versione di WebKitGTK della distro.
- Applicazioni kiosk ed embedded che oggi usano WPE ma vogliono un'integrazione più semplice.

## Contesto

Nessuna alternativa attuale offre insieme motore web completo, licenza open source, comportamento identico su tre OS e distribuzione come singolo binario. È questo lo spazio del progetto.

| Alternativa | Motore | JS | Distribuzione | Licenza | Limite principale |
| --- | --- | --- | --- | --- | --- |
| [Ultralight](https://github.com/ultralight-ux/Ultralight) | Fork di WebCore + renderer proprio | Sì (JavaScriptCore) | DLL; link statico non consentito con la licenza gratuita | Core proprietario, WebCore LGPL/BSD | Licenza commerciale per uso a pagamento |
| [WPE WebKit](https://wpewebkit.org/) | WebKit upstream | Sì | Librerie dinamiche, multi-processo | LGPL/BSD | Solo Linux |
| CEF | Chromium | Sì | Librerie dinamiche, centinaia di MB | BSD | Dimensioni e complessità |
| [Servo](https://servo.org/blog/2026/04/13/servo-0.1.0-release/) | Servo (Rust) | Sì | Crate Rust, v0.1.0 da aprile 2026 | MPL 2.0 | Compatibilità web ancora inferiore a WebKit e Chromium |
| Webview di sistema (WebView2, WKWebView, WebKitGTK) | Diversi per OS | Sì | Dipendenza dal sistema | Varia | Comportamento diverso per OS, WebKitGTK frammentato su Linux |
| [Blitz](https://github.com/DioxusLabs/blitz) | Motore HTML/CSS modulare | No | Crate Rust | MIT/Apache | Pre-alpha, nessun JavaScript per scelta |

Il vantaggio competitivo del motore è la combinazione di tre cose: compatibilità WebKit, API C per qualsiasi linguaggio e distribuzione a file singolo. Servo è il concorrente open source più diretto; il confronto va rifatto a ogni sua release LTS.

## Architettura

L'applicazione vede solo l'API C; sotto ci sono il layer di embedding e il layer di piattaforma scritti dal progetto, con in mezzo WebKit upstream. Tutto vive in un solo processo.

&#91;embedded content: architettura · un processo, sei livelli\]

Le chiamate scendono dall'applicazione fino alla presentazione. Le binding JS fanno il percorso inverso senza IPC: JavaScriptCore chiama direttamente il codice nativo registrato dal framework, sullo stesso thread principale.

## Port del motore

Il motore è un **port WebKit proprio e multipiattaforma**, costruito direttamente su WTF, JavaScriptCore e WebCore, senza il livello WebKit2 multi-processo e senza partire da WPE o dal port Win. È lo stesso approccio di Ultralight.

Upstream ha già rimosso il percorso single-process per i port non-Apple: il [WebKit1 di WinCairo è stato eliminato](https://lists.webkit.org/pipermail/webkit-unassigned/2023-March/1093883.html), e con esso il motivo di mantenere la vecchia implementazione curl di `ResourceHandle`. Il livello che collega WebCore all'applicazione va quindi scritto dal progetto.

### Livelli

| Livello | Origine | Contenuto |
| --- | --- | --- |
| WTF | Upstream | Strutture dati, threading, `RunLoopGeneric` (già usato dai port JSCOnly e PlayStation), quindi niente GLib |
| JavaScriptCore | Upstream | Motore JS, JIT, API C (`JSContextRef`, `JSObjectRef`) |
| WebCore | Upstream con patch minime | DOM, CSS, layout, rendering con Skia, rete con le classi curl di `platform/network/curl` |
| Layer di piattaforma | Progetto | Contesti GPU per Skia, font manager, clipboard, cursore, IME, timer |
| Layer di embedding | Progetto | `Page`, `LocalFrame` e i client di WebCore (chrome, editor, frame loader, context menu), loader delle risorse |
| API C | Progetto | Interfaccia pubblica `zg_*` |

I nomi delle classi client di WebCore cambiano spesso tra revisioni (per esempio `FrameLoaderClient` è diventato `LocalFrameLoaderClient` con il lavoro sulla site isolation). Vanno verificati sulla revisione scelta.

### Riferimenti da studiare

- **[WebCore di Ultralight](https://github.com/ultralight-ux/WebCore)**: il fork LGPL pubblico di Ultralight, con un layer di embedding single-process funzionante. È il riferimento più vicino al progetto.
- **WebKitLegacy di Apple** (`Source/WebKitLegacy`): l'ultimo layer di embedding single-process ancora nel repository, solo per le piattaforme Apple. Mostra come si implementano i client di WebCore.
- **Port PlayStation e JSCOnly**: build CMake senza GLib, `RunLoopGeneric`, rete curl, Skia previsto.

### Port CMake

Il port si aggiunge come nuovo valore di `PORT` nel CMake di WebKit, con i file `Options<Port>.cmake` e `Platform<Port>.cmake` per WTF, JavaScriptCore e WebCore, sul modello di PlayStation. La stessa configurazione vale per Linux, Windows e macOS; le differenze per OS stanno solo nel layer di piattaforma.

### Grafica: Skia

Da WPE e WebKitGTK 2.46 [Skia ha sostituito Cairo come renderer 2D](https://wpewebkit.org/release/wpewebkit-2.46.0.html). Skia è già incluso nel repository WebKit e [linkato staticamente nella build CMake](https://lists.webkit.org/pipermail/webkit-dev/2024-February/032615.html), con licenza BSD 3-Clause. Il port riusa il codice grafico Skia di WebCore e aggiunge solo la creazione dei contesti GPU per ciascun OS.

### JavaScriptCore

- JIT attivo sulle piattaforme che lo consentono; interprete come fallback.
- La build CMake ha un'opzione `ENABLE_STATIC_JSC` per compilare JSC non condiviso, [introdotta per il port PlayStation](https://lists.webkit.org/pipermail/webkit-unassigned/2021-September/995296.html).
- L'API C di JSC è la base delle binding native verso l'host.

### Revisione upstream da seguire

- Il port segue **`main`**: i fix, compresi quelli di sicurezza, arrivano lì per primi.
- Non si segue `main` giorno per giorno: si fissa un commit che passa tutta la CI e lo si aggiorna a intervalli regolari (proposta: ogni 2–4 settimane), oppure subito in caso di advisory di sicurezza critico.
- Un job notturno compila il port contro l'ultimo `main`, così le rotture delle API interne di WebCore si vedono il giorno in cui arrivano, invece di accumularsi.
- Le modifiche ai file upstream vivono in `patches/`; il codice nuovo del port sta in directory proprie, così i conflitti restano limitati alle patch.

## Modello di processo

Il motore è **single-process**: WebCore, JavaScriptCore e l'applicazione condividono lo stesso processo e lo stesso thread principale. È la scelta che rende semplice il link statico (nessun sottoprocesso da lanciare, nessun IPC) e permette binding JS dirette.

### Thread

| Thread | Ruolo | Proprietario |
| --- | --- | --- |
| Principale | DOM, layout, JavaScript, eventi, callback dell'API | L'applicazione: è il thread che crea il runtime |
| Rete | Richieste curl in parallelo (curl multi) | Motore |
| JSC | Garbage collector concorrente, compilazione JIT | JavaScriptCore |
| Decodifica | Decodifica di immagini e font | Motore, pool di worker |
| Painting (opzionale) | Rasterizzazione Skia di tile in parallelo | Motore |

Il vincolo principale è quello di WebCore: tutto ciò che tocca DOM e JavaScript avviene su un solo thread. Per questo l'API pubblica è utilizzabile solo dal thread che ha creato il runtime.

### Integrazione con il loop dell'applicazione

L'applicazione fa avanzare il motore, come in Ultralight:

- **Modalità finestra**: `zg_runtime_run()` possiede il loop e gestisce eventi, timer e rendering.
- **Modalità gioco o loop proprio**: l'applicazione chiama `zg_runtime_update()` a ogni tick (timer, risposte di rete, task pendenti) e `zg_view_render()` quando vuole un frame.
- **Risveglio**: `zg_runtime_wakeup_handle()` restituisce un handle da attendere quando l'applicazione è idle, segnalato quando il motore ha lavoro da fare (risposta di rete, timer in scadenza).

### Conseguenze da accettare

- **Nessun isolamento dei crash**: un crash in WebCore o JSC termina l'applicazione.
- **Nessuna sandbox per il contenuto web**: il codice di WebCore gira con i privilegi dell'applicazione (vedi Sicurezza).
- **Memoria condivisa**: l'heap di JSC e le cache di WebCore contano nella memoria dell'applicazione. Il motore espone limiti configurabili per cache e heap.

* **Una sola VM JavaScript**: tutte le view condividono heap e garbage collector di JSC. I limiti di memoria si possono applicare solo all'intero runtime, non alla singola view.

## Convivenza con l'applicazione host

Un motore single-process vive dentro il processo di qualcun altro: un gioco, un'app con un proprio runtime (Go, .NET, Python, Node), un crash handler. WebKit invece è progettato per avere un processo tutto suo. Questa sezione elenca le risorse globali del processo che WebKit usa, e come il motore le rende compatibili con l'host.

### Allocatore di memoria

- WebKit usa un allocatore proprio (libpas e bmalloc) con Gigacage, che [riserva enormi quantità di spazio di indirizzamento](https://blogs.gnome.org/mcatanzaro/2018/11/02/on-webkit-build-options-also-how-to-accidentally-disable-important-security-features/) e richiede l'overcommit. È una fonte di conflitti con host che gestiscono la memoria per conto loro.
- **Decisione: malloc di sistema** (`USE_SYSTEM_MALLOC=ON`), per convivere con qualunque host senza riserve di memoria virtuale.
- **Conseguenza**: si perdono Gigacage, gli heap isolati e le altre protezioni dell'heap di WebKit. Senza sandbox, è una difesa in meno contro i bug di memoria; il contenuto esterno resta ancora più sconsigliato.
- **Mitigazione da valutare**: al posto del malloc della libc si può linkare un allocatore con hardening (per esempio mimalloc in modalità secure o Scudo), recuperando parte delle protezioni senza riserve di memoria enormi.
- L'API espone le statistiche di memoria del motore (heap JSC, cache di WebCore, superfici grafiche), così l'host può includerle nei propri budget.

### Segnali (Linux)

- JSC usa un segnale per sospendere i thread durante il GC concorrente: di default SIGUSR1, configurabile con la variabile `JSC_SIGNAL_FOR_GC`. WebKit [controlla che nessun altro gestisca quel segnale](https://lists.webkit.org/pipermail/webkit-unassigned/2021-March/969495.html), e un host che lo usa già (nel caso citato, Ruby) va in crash.
- Bun, che incorpora JSC, ha [spostato il default da SIGUSR1 a SIGPWR](https://github.com/oven-sh/WebKit/pull/235) perché molti pacchetti npm registrano handler su SIGUSR1. Nello stesso lavoro ha corretto un blocco del GC quando l'host installa uno stack alternativo per i segnali, come fanno ASan e molti crash handler.
- JSC gestisce anche segnali di trap della CPU (SIGSEGV, SIGBUS, SIGILL) per alcune funzioni, tra cui la memoria di WebAssembly. Gli handler vanno concatenati con quelli dell'host, non sostituiti.
- **Proposta**: il segnale per il GC si sceglie in `zg_runtime_config`, non con una variabile d'ambiente, con default su un segnale real-time libero. I test di CI girano anche con un crash handler e uno stack alternativo attivi.
- macOS usa le porte Mach e Windows `SuspendThread`, quindi il problema riguarda Linux.

### Simboli nel link statico

- Se l'host linka già zlib, libpng, curl o OpenSSL, il link statico produce simboli duplicati o, peggio, l'uso silenzioso della versione sbagliata. BoringSSL esporta gli stessi nomi di OpenSSL.
- **Proposta**: il motore si distribuisce come **un unico archivio** in cui tutte le dipendenze sono collegate in anticipo (link parziale) e tutti i simboli sono resi locali tranne l'API `zg_*`. BoringSSL si compila in più con il suo prefisso di simboli.
- Lo stesso vale per il runtime C++: se l'host è C++ con un'altra libreria standard, i simboli di libc++ del motore devono restare interni.

### Runtime e main thread

- WTF registra il "main thread" una sola volta per processo, e lo stato globale di WebKit non si smonta in modo affidabile.
- `zg_runtime` è quindi un **singleton di processo**: si crea una volta, e `zg_runtime_destroy` è valido solo alla chiusura dell'applicazione. Non si può ricreare il runtime.
- Le chiamate all'API da un thread diverso falliscono con un errore esplicito nelle build di debug.

### JIT e policy delle piattaforme

| Piattaforma | Vincolo | Gestione |
| --- | --- | --- |
| macOS | Con hardened runtime e notarizzazione il JIT richiede l'entitlement `com.apple.security.cs.allow-jit` e memoria `MAP_JIT` | Il `package` del framework aggiunge l'entitlement |
| Windows | Se l'host attiva la policy che vieta il codice dinamico (ACG), il JIT non può allocare memoria eseguibile | Rilevamento all'avvio e passaggio all'interprete |
| Linux | Policy SELinux come `execmem` negato | Rilevamento all'avvio e passaggio all'interprete |

`zg_runtime_config` ha un'opzione `jit` con tre valori: automatico (default), sempre attivo, disattivato.

### Thread del motore

- Ogni thread creato dal motore ha un nome riconoscibile (rete, GC, JIT, decodifica, worker), così compare in modo chiaro nei profiler e nei crash dump dell'host.
- Il numero di thread di GC, JIT e decodifica è configurabile, per host che hanno già un proprio pool e non vogliono competere per i core.
- I **Web Worker** girano su thread del motore, ciascuno con il proprio run loop, e terminano con la view che li ha creati. Vanno testati esplicitamente, perché nel single-process condividono heap e allocatore con l'host.

## Grafica e presentazione

Con un solo processo il problema del trasporto dei frame sparisce: **Skia renderizza direttamente su un contesto GPU dell'applicazione o creato dal motore nello stesso processo**. Non servono DMA-BUF, texture condivise tra processi o IOSurface.

### Modalità

- **Modalità finestra**: il motore crea una finestra nativa e una swapchain, e presenta da solo. È il caso del framework desktop.
- **Modalità texture**: l'applicazione fornisce il proprio device GPU (o accetta quello del motore) e riceve una texture con la view renderizzata. È il caso di UI di giochi e overlay.
- **Modalità CPU**: Skia rasterizza in un buffer di pixel in memoria. Serve per ambienti senza GPU (VM, CI, server) e per i test di rendering.

### Backend GPU per piattaforma

| Piattaforma | Backend Skia | Note |
| --- | --- | --- |
| Linux | OpenGL (EGL) o Vulkan | Driver caricati dal sistema con `dlopen` |
| Windows | OpenGL tramite ANGLE su D3D11 | Il percorso Skia più collaudato su Windows; condivisione diretta delle texture con i giochi D3D11; WebGL incluso |
| macOS | Metal | Il backend Metal di Skia è maturo |

### Compositing

WebCore separa le parti della pagina in layer (trasformazioni CSS, animazioni, video, canvas WebGL) e si aspetta un compositor che li unisca. È la parte più impegnativa del layer di piattaforma.

- **Versione 1**: compositing accelerato disattivato. WebCore disegna tutto in una sola superficie Skia, con aggiornamento delle sole regioni modificate. È più semplice e basta per la maggior parte delle UI.
- **Versione 2**: compositor proprio sopra Skia, oppure riuso di TextureMapper (il compositor dei port GTK e WPE). Serve per animazioni CSS fluide su pagine complesse e per WebGL.

### Ciclo di un frame

1. L'applicazione chiama `zg_view_render()`, oppure lo fa il loop del motore in modalità finestra.
2. Il motore esegue i task pendenti, aggiorna stile e layout, e ridisegna con Skia solo le regioni modificate.
3. Il risultato è nella swapchain (finestra), nella texture dell'applicazione (texture) o nel buffer di pixel (CPU).
4. La presentazione avvenuta fa avanzare `requestAnimationFrame`, così le animazioni seguono il vsync dell'applicazione.

### DPI e dimensioni

- La view ha dimensioni logiche e un fattore di scala; la superficie ha dimensioni fisiche (logiche × scala).
- Un cambio di monitor o di scala va propagato al motore, che rilancia layout e rendering.
- In modalità texture è l'applicazione a decidere dimensioni e scala, per esempio la risoluzione della texture di UI di un gioco.

### Render thread separato

Molti giochi renderizzano su un thread diverso da quello principale, mentre il motore disegna sul thread che possiede il runtime. In modalità texture serve quindi un passaggio esplicito tra i due thread:

1. Il motore renderizza in una delle sue texture, a doppio o triplo buffer, sul proprio thread.
2. Al termine segnala un fence GPU (fence D3D11, `VkSemaphore`, `GLsync` o evento Metal) e pubblica la texture come "pronta".
3. Il render thread dell'app acquisisce la texture più recente, attende il fence sulla GPU (senza bloccare la CPU) e la compone.
4. Al rilascio, la texture torna al motore per il frame successivo.

Se app e motore usano lo stesso device GPU basta il fence. Con device diversi, su Windows si condivide la texture con un handle NT (`IDXGIResource1::CreateSharedHandle`). L'API espone questo ciclo come `zg_surface_acquire` / `zg_surface_release`, usabili dal render thread.

## Testo e font

Per avere un rendering del testo identico ovunque, il motore usa lo stesso stack su tutte le piattaforme e permette di **incorporare i font nel binario**. I font di sistema restano disponibili come opzione.

### Stack

| Componente | Ruolo | Licenza |
| --- | --- | --- |
| FreeType | Rasterizzazione dei glifi | FTL o GPLv2, a scelta |
| HarfBuzz | Shaping del testo | MIT |
| Font manager di Skia | Font incorporati o letti da directory e fallback, al posto di Fontconfig | BSD 3-Clause (parte di Skia) |
| ICU | Unicode, segmentazione, bidi, collation | Licenza Unicode |

Su Windows e macOS va deciso se usare lo stesso stack FreeType per coerenza visiva, o il font manager nativo di Skia (DirectWrite, CoreText) per integrazione con il sistema. Il default proposto è FreeType ovunque, con il nativo come opzione.

### Font incorporati

- Un set minimo di default incluso nel binario: una sans, una serif, una monospace e un font di fallback ampio per gli script non latini.
- API per registrare font dall'applicazione (`zg_context_register_font` da memoria), così gli asset del frontend non dipendono dal sistema.
- `@font-face` continua a funzionare normalmente, anche con lo schema custom degli asset (vedi Rete, storage e risorse).

### ICU e dimensioni del binario

I dati ICU completi pesano decine di MB e dominano la dimensione di un binario statico. Il build system usa il filtro dei dati ICU per includere solo le lingue e le funzioni necessarie, con un profilo "minimo" e uno "completo" selezionabili a compile time.

## Rete, storage e risorse

Lo stack di rete è **unico su tutte le piattaforme: curl con BoringSSL**. curl è già integrato in WebCore (le classi di `platform/network/curl`, incluso il cookie jar su SQLite), si linka statico senza problemi e offre HTTP/2 e, in seguito, HTTP/3.

### Stack

| Componente | Ruolo | Note |
| --- | --- | --- |
| curl (interfaccia multi) | HTTP/1.1, HTTP/2, pool di connessioni | Gira sul thread di rete del motore |
| BoringSSL | TLS 1.3 | Veloce, usato da Chrome, adatto al link statico; alternativa OpenSSL 3 |
| nghttp2 | HTTP/2 con multiplexing | — |
| ngtcp2 e nghttp3 | HTTP/3 su QUIC | Opzionale, versione 2 |
| c-ares | DNS asincrono | Opzionale; il resolver a thread di curl basta per la versione 1 |
| zlib, brotli, zstd | Decompressione delle risposte | — |

### Prestazioni

In un'applicazione quasi tutte le risorse arrivano da `app://`, servite dalla memoria senza toccare la rete. Per le richieste esterne (API, immagini remote) contano soprattutto: riuso delle connessioni, multiplexing HTTP/2, resumption delle sessioni TLS e cache DNS. Sono tutte funzioni di curl da configurare, non da scrivere.

### Certificati

BoringSSL non usa da solo gli store di sistema. Il motore carica le CA radice all'avvio: dai percorsi standard su Linux, dallo store di sistema di Windows (CryptoAPI) e dal Keychain su macOS. In alternativa si può incorporare un bundle di CA nel binario, utile per ambienti embedded.

### Loader delle risorse

Il layer di embedding implementa il caricamento: `app://` e gli schemi custom rispondono direttamente sul thread principale; `http` e `https` passano a curl sul thread di rete, e le risposte tornano al thread principale a pezzi, in streaming.

### Cache HTTP

La cache su disco di WebKit vive nel network process di WebKit2, quindi non è disponibile. Nella versione 1 c'è solo la cache in memoria di WebCore; una cache su disco propria arriva nella versione 2, se serve.

### Schema custom `app://`

- Gli asset del frontend (HTML, JS, CSS, immagini, font) sono incorporati nel binario come archivio compresso, generato a compile time.
- Lo schema risponde dall'archivio con MIME type corretto e supporto alle range request per audio e video.
- Lo schema va registrato come **sicuro** e con CORS abilitato, altrimenti API come `crypto.subtle` e `fetch` cross-origin non funzionano.
- L'applicazione può registrare schemi aggiuntivi con un callback (`zg_context_register_scheme`), per esempio per servire file generati a runtime.

### Dev mode

In sviluppo la view carica direttamente il dev server del frontend (per esempio Vite su `http://localhost:5173`), e il passaggio a `app://` avviene solo nella build di release. L'hot reload passa da HTTP e **WebSocket**, e qui c'è lavoro da fare: senza il network process di WebKit2 il backend WebSocket va fornito dal layer di embedding, sopra curl (che supporta WebSocket nelle versioni recenti) o riusando il codice curl di WebCore se è ancora presente nella revisione scelta. Va verificato presto, perché senza WebSocket il dev mode perde l'hot reload.

### Storage

- Cookie con il cookie jar su SQLite di WebCore; localStorage e IndexedDB con implementazioni in-process nel layer di embedding, sul modello di WebKitLegacy.
- Directory dati per applicazione nel percorso standard dell'OS (`$XDG_DATA_HOME`, `%LOCALAPPDATA%`, `~/Library/Application Support`).
- Modalità effimera opzionale: nessun dato scritto su disco.

## Input, IME, clipboard, accessibilità

In modalità finestra il motore traduce gli eventi nativi da solo; in modalità texture è l'applicazione a iniettarli tramite API. In entrambi i casi gli eventi passano dallo stesso percorso interno, per evitare comportamenti diversi.

### Eventi

- Mouse (movimento, pulsanti, rotella con delta precisi), tastiera (keycode, testo, modificatori), touch e pen in una fase successiva.
- In modalità texture le coordinate arrivano in pixel logici della view; per un'UI di gioco su una superficie 3D è l'applicazione a proiettare il puntatore.
- Callback dal motore verso l'host: cambio di cursore, tooltip, richiesta di focus, apertura di link esterni.

### IME

Il supporto agli input method (cinese, giapponese, coreano, emoji) è obbligatorio per un motore usabile. In modalità finestra si integra con l'IME di sistema (IBus o Fcitx su Linux, TSF su Windows, NSTextInputClient su macOS). In modalità texture il motore espone posizione del cursore di testo ed eventi di composizione, e l'applicazione decide come mostrarli.

### Clipboard, drag and drop, dialog

- Clipboard di sistema in lettura e scrittura, con testo, HTML e immagini.
- Drag and drop di file verso la view nella versione 1; trascinamento fuori dalla view più avanti.
- Context menu, file chooser e dialog JS (`alert`, `confirm`, `prompt`) diventano callback verso l'host, con un'implementazione nativa di default in modalità finestra.

### Accessibilità

WebCore costruisce l'albero di accessibilità, ma il ponte verso lo screen reader è specifico di ogni OS: AT-SPI su Linux (che nei port upstream dipende da GLib e D-Bus), UI Automation su Windows, NSAccessibility su macOS. Nel port proprio questi ponti vanno scritti. Sono fuori dalla versione 1 e restano una questione aperta, insieme all'accessibilità in modalità texture.

## API pubblica C

L'API è C99 puro, con handle opachi, nessun tipo WebKit esposto, e il prefisso `zg_`. Il prefisso `WK` va evitato perché è già usato dall'API C di WebKit.

### Principi

- **Handle opachi** con creazione e distruzione esplicite (`zg_view_create` / `zg_view_destroy`). Nessuna struttura interna nell'header.
- **Un solo thread**: tutte le chiamate avvengono sul thread che ha creato il runtime, e i callback arrivano sullo stesso thread.
- **Stabilità ABI**: struct di configurazione con campo `size` iniziale, così si possono aggiungere campi senza rompere i client.
- **Errori espliciti**: le funzioni che possono fallire ritornano `zg_result` e un messaggio leggibile con `zg_last_error()`.
- **Stringhe UTF-8** con lunghezza esplicita dove serve.

### Oggetti

| Oggetto | Responsabilità |
| --- | --- |
| `zg_runtime` | Inizializzazione globale, loop, avanzamento del motore |
| `zg_context` | Sessione: directory dati, schemi custom, rete, font registrati |
| `zg_view` | Una pagina: caricamento, dimensioni, input, rendering, JavaScript |
| `zg_window` | Finestra nativa opzionale che ospita una view (modalità finestra) |
| `zg_surface` | Destinazione del rendering in modalità texture o CPU |
| `zg_value` | Valore JavaScript visto dall'host, con conversioni verso i tipi C |

### Loop

- `zg_runtime_run()`: il motore possiede il loop. È il caso del framework desktop.
- `zg_runtime_update()`: un passo di avanzamento (timer, risposte di rete, task pendenti), da chiamare a ogni tick del loop dell'applicazione.
- `zg_runtime_wakeup_handle()`: handle da attendere quando l'applicazione è idle, segnalato quando il motore ha lavoro da fare.

### Binding JavaScript dirette

Con un solo processo il JavaScript della pagina gira nello stesso processo e sullo stesso thread dell'applicazione. Le funzioni native si registrano **direttamente nel contesto JSC della pagina**, con l'API C di JavaScriptCore, senza IPC.

- **Chiamate sincrone**: il callback nativo gira durante la chiamata JS e il valore di ritorno arriva subito al JavaScript (`const r = App.greet("Daniele")`). È il caso di default.
- **Chiamate asincrone**: per lavoro lungo il callback restituisce una `Promise`, esegue il lavoro su un thread dell'applicazione e la risolve con `zg_promise_resolve`, che il motore esegue sul thread principale.
- **Installazione**: le binding si installano quando WebCore crea l'oggetto `window` di un frame (il callback di "window object cleared" del frame loader client), solo per le origini autorizzate.
- **Eventi verso il JS**: `zg_view_emit` crea e invia direttamente un `CustomEvent` sul `window` della pagina.

### Esempio d'uso

```c
#include <zerog.h>

static zg_value *on_greet(zg_call *call, void *user) {
    size_t len;
    const char *name = zg_call_arg_string(call, 0, &len);
    char reply[128];
    snprintf(reply, sizeof reply, "Ciao %.*s", (int)len, name);
    return zg_value_string(call, reply);
}

int main(void) {
    zg_runtime *rt = zg_runtime_create(&(zg_runtime_config){ .size = sizeof(zg_runtime_config) });
    zg_context *ctx = zg_context_create(rt, &(zg_context_config){
        .size = sizeof(zg_context_config),
        .app_id = "dev.example.demo",
    });
    zg_context_mount_assets(ctx, "app", embedded_assets, embedded_assets_len);

    zg_window *win = zg_window_create(ctx, &(zg_window_config){
        .size = sizeof(zg_window_config), .title = "Demo", .width = 1024, .height = 768,
    });
    zg_view *view = zg_window_view(win);
    zg_view_bind(view, "App.greet", on_greet, NULL);
    zg_view_load_url(view, "app://index.html");

    zg_runtime_run(rt);
    zg_runtime_destroy(rt);
    return 0;
}
```

Lato JS: `const msg = App.greet("Daniele")`, senza `await`. Il framework Zig genera sopra questa API le binding tipizzate.

### Diagnostica per lo sviluppatore

- **Console**: ogni `console.log`, `warn` ed `error` arriva a un callback dell'host con livello, messaggio, file e riga. Il framework lo stampa nel terminale in dev mode.
- **Errori JS non gestiti**: callback dedicato con messaggio e stack trace, anche per le `Promise` rifiutate senza handler.
- **Web Inspector**: nel profilo dev il motore avvia l'inspector remoto di WebKit su una porta locale, e il frontend dell'inspector (incluso nel binario solo nel profilo dev) si apre in una seconda finestra. Nei profili minimal e standard non c'è, per dimensione e superficie d'attacco.
- **Log interni del motore**: livelli configurabili, instradati allo stesso callback, così l'host decide dove scriverli.

## Build system e linking statico

"Singolo binario" ha un confine preciso: tutto è linkato statico **tranne la libc e i loader dei driver GPU**. I driver vanno caricati dal sistema con `dlopen` di `libEGL` o `libvulkan` su Linux, e lo stesso vale per le DLL di sistema su Windows e i framework su macOS. È lo stesso compromesso dei giochi.

Senza GLib, libsoup e GStreamer spariscono anche i moduli caricati a runtime, che erano l'ostacolo principale al link statico.

### Struttura

- **WebKit** (WTF, JavaScriptCore, WebCore) si compila con il CMake di WebKit e il `PORT` del progetto, con clang su tutte le piattaforme (clang-cl su Windows), producendo archivi statici.
- **Dipendenze** C e C++ compilate statiche in un sysroot per ogni target, da una pipeline riproducibile.
- **Layer di piattaforma, layer di embedding e API C** si compilano nello stesso CMake del port.
- **zig build** orchestra tutto: lancia CMake come step esterno, raccoglie gli archivi, genera l'archivio degli asset e fa il link finale dell'applicazione.
- **Runtime C++** statico, per non dipendere dalla versione installata nel sistema.

### Media

WebCore ha bisogno di un backend di riproduzione per `<video>`, `<audio>` e Web Audio, e nei port upstream è GStreamer o AVFoundation. Il port proprio ne deve fornire uno.

- **Versione 1**: media disattivati (`ENABLE_VIDEO`, `ENABLE_WEB_AUDIO`).
- **Dopo**: un backend unico basato su FFmpeg (LGPL, link statico con gli stessi obblighi di WebCore), oppure backend per OS (Media Foundation, AVFoundation). È una questione aperta.

### Profili di build

- **minimal**: niente media, niente WebGL, dati ICU ridotti. L'obiettivo è il binario più piccolo possibile per UI di applicazione.
- **standard**: aggiunge WebGL e, quando esiste, il backend media.
- **dev**: simboli completi, assert attive, Web Inspector abilitato. Per lavorare sul motore stesso.

### Tempi e cache

Una build completa di WebKit richiede ore. La CI usa una cache degli artefatti per revisione e configurazione; le modifiche ai soli layer del progetto ricompilano solo quelli e il link finale.

## Dipendenze: cosa togliere, tenere, sostituire

Il profilo minimal dipende da **16 librerie (17 su Windows, con ANGLE), tutte compilate statiche**, e a runtime da nient'altro che libc, driver GPU e componenti di sistema. Tutto ciò che i port Linux upstream portano con sé (GLib, libsoup, GStreamer, Fontconfig, librerie di sistema per sandbox e accessibilità) si toglie o si sostituisce.

### Criteri

Una dipendenza resta solo se soddisfa tutte e tre le condizioni:

1. serve a una funzione di WebCore che il profilo tiene accesa;
2. si compila statica con CMake o zig, senza patch invasive;
3. non carica plugin, moduli o file di dati a runtime fuori dal binario.

### Decisioni

| Dipendenza | Decisione | Uso | Come |
| --- | --- | --- | --- |
| ICU | Tenere | Unicode, segmentazione, normalizzazione, `Intl` di JSC: usato ovunque in WTF e WebCore | Statica con filtro dei dati e poche lingue, tenendo i dati dei fusi orari per Date e Intl. WebCore ha codec propri per molte codifiche (`TextCodecCJK`, `TextCodecSingleByte`), quindi i dati dei converter ICU si possono ridurre molto (da verificare) |
| HarfBuzz | Tenere | Shaping del testo | Senza GLib, ICU e Graphite; dati Unicode interni |
| FreeType | Tenere | Rasterizzazione dei glifi | Solo zlib e libpng come dipendenze; niente bzip2 e brotli |
| Skia | Tenere | Grafica 2D | Già nel repository; solo i backend GPU necessari, niente PDF, SVG e Skottie |
| curl | Tenere | HTTP/1.1 e HTTP/2 | Solo HTTP e HTTPS: niente FTP, SMTP, LDAP e altri protocolli |
| BoringSSL | Tenere | TLS e WebCrypto | Una sola libreria crittografica per rete e WebCrypto |
| nghttp2 | Tenere | HTTP/2 | — |
| libpsl | Tenere | Public suffix list per i domini dei cookie | Lista compilata dentro il binario |
| zlib | Tenere | Decompressione per HTTP, PNG e FreeType | zlib-ng in modalità compatibile per le prestazioni |
| brotli, woff2 | Tenere | Font WOFF2 e `Content-Encoding: br` | WOFF2 è il formato di web font più comune nei frontend |
| libpng, libjpeg-turbo | Tenere | Immagini PNG e JPEG, `toDataURL` | — |
| libwebp | Tenere | Immagini WebP | Escludibile se l'app non usa WebP |
| libxml2 | Tenere | Documenti XML e XHTML, SVG come documento, `responseXML` | Build minima: niente HTTP, FTP, iconv, LZMA, Python |
| SQLite | Tenere | Cookie jar, IndexedDB | Amalgamation in un solo file C |
| GLib, GObject, GIO | Sostituire | Main loop e tipi dei port Linux upstream | `RunLoopGeneric` di WTF |
| libsoup, glib-networking | Sostituire | Rete e TLS dei port GLib; il TLS è un modulo caricato a runtime | curl e BoringSSL |
| libgcrypt, libtasn1 | Sostituire | WebCrypto dei port GLib | [Backend OpenSSL di WebCore](https://webkit.googlesource.com/WebKit/+/7f131dee9ca99449765f131be8dfd81c800dce88/Source/WebCore/crypto/openssl), scritto da Sony per i suoi port, compilato contro BoringSSL |
| Fontconfig, expat | Sostituire | Ricerca e fallback dei font su Linux | Font manager di Skia per font incorporati o letti da directory, più una tabella di fallback propria |
| GTK, Wayland, X11 | Sostituire | Finestra e input | Core senza finestra; modalità finestra con SDL3 statico, che carica X11 e Wayland a runtime con `dlopen`, oppure layer finestra proprio |
| libepoxy | Sostituire | Caricamento delle funzioni OpenGL | Skia carica le funzioni GL e Vulkan con `eglGetProcAddress` e `vkGetInstanceProcAddr` |
| GStreamer | Togliere | Audio e video tramite plugin | Media disattivati nella versione 1 |
| libdrm, GBM | Togliere | DMA-BUF tra processi | Inutili con un solo processo |
| libxslt | Togliere | XSLT | `ENABLE_XSLT=OFF` |
| lcms2 | Togliere | Profili colore ICC | Skia gestisce gli spazi colore |
| libavif, dav1d, libjxl | Togliere | AVIF e JPEG XL | Riattivabili nel profilo standard |
| libhyphen | Togliere | `hyphens: auto` | — |
| enchant | Togliere | Controllo ortografico | — |
| libmanette | Togliere | Gamepad API | — |
| libsecret | Togliere | Archivio credenziali | — |
| ATK, AT-SPI | Togliere | Accessibilità su Linux | Ponti propri dopo la versione 1 |
| libsystemd, libseccomp, bubblewrap, xdg-dbus-proxy | Togliere | Logging e sandbox dei port multi-processo | — |
| libidn2 | Togliere | Domini internazionalizzati in curl | WebCore converte già gli host in punycode con ICU |
| c-ares | Togliere | DNS asincrono | Basta il resolver a thread di curl |
| ANGLE | Tenere | WebGL; OpenGL su Windows | Necessario su Windows come backend GPU di Skia (GL su D3D11); su Linux e macOS solo nel profilo standard, per WebGL |

### Funzioni da spegnere

| Funzione | Flag CMake | minimal | standard |
| --- | --- | --- | --- |
| Video e audio | `ENABLE_VIDEO` | No | Con backend media |
| Web Audio | `ENABLE_WEB_AUDIO` | No | Con backend media |
| WebGL | `ENABLE_WEBGL` | No | Sì |
| WebAssembly | `ENABLE_WEBASSEMBLY` | No | Sì |
| XSLT | `ENABLE_XSLT` | No | No |
| WebRTC e MediaStream | `ENABLE_WEB_RTC`, `ENABLE_MEDIA_STREAM` | No | No |
| Encrypted Media | `ENABLE_ENCRYPTED_MEDIA` | No | No |
| Gamepad | `ENABLE_GAMEPAD` | No | No |
| Sintesi vocale | `ENABLE_SPEECH_SYNTHESIS` | No | No |
| Service worker | `ENABLE_SERVICE_WORKER` | No | Da valutare |
| Web Inspector remoto | `ENABLE_REMOTE_INSPECTOR` | No | Solo nel profilo dev |
| AVIF e JPEG XL | `USE_AVIF`, `USE_JPEGXL` | No | Sì |
| WOFF2 | `USE_WOFF2` | Sì | Sì |

I nomi dei flag vanno verificati sulla revisione scelta: cambiano tra versioni, e alcuni sono `ENABLE_` mentre altri sono `USE_`.

### Cosa resta dinamico a runtime

| Piattaforma | Caricato dal sistema | Come |
| --- | --- | --- |
| Linux | glibc, driver GPU (`libEGL`, `libGL`, `libvulkan`) | Build contro una glibc di base vecchia (per esempio 2.28) con zig, così il binario parte anche su distro datate. musl statico è escluso: non può caricare i driver GPU con `dlopen` |
| Windows | Solo DLL di sistema (kernel32, user32, d3d11, dxgi, crypt32, ws2\_32) | CRT statica (`/MT`) |
| macOS | Solo framework di sistema (Metal, AppKit, CoreFoundation, Security) | — |

Perl, Python, Ruby, gperf e unifdef servono a generare codice durante la build di WebKit, ma non finiscono nel binario.

### Ordine di lavoro

1. Partire dalla configurazione del port JSCOnly, che [dipende solo da ICU](https://bugs.webkit.org/show_bug.cgi?id=155706): verifica subito la build statica di WTF e JSC.
2. Aggiungere WebCore con tutte le funzioni della tabella spente e solo Skia, HarfBuzz, FreeType, libxml2 e SQLite.
3. Aggiungere le altre librerie una per volta, quando arriva la funzione che le richiede: rete (curl, BoringSSL, nghttp2, libpsl), immagini (libpng, libjpeg-turbo, libwebp), font web (brotli, woff2).
4. Misurare la dimensione del binario a ogni aggiunta, in CI, così ogni dipendenza ha un costo visibile.

## Licenze e conformità LGPL

Il link statico è consentito anche per applicazioni proprietarie, ma con un obbligo preciso: l'utente finale deve poter **ricollegare l'applicazione con una versione modificata delle parti LGPL**. Il motore deve rendere questo obbligo facile da rispettare, altrimenti nessuno lo userà in prodotti commerciali.

Questa sezione descrive il design tecnico, non è una consulenza legale. Prima della prima release va fatta una revisione da un legale esperto di licenze open source.

### Licenze dei componenti

| Componente | Licenza |
| --- | --- |
| WebCore, JavaScriptCore | [LGPL](https://en.wikipedia.org/wiki/WebKit) |
| WTF e resto di WebKit | BSD 2-Clause |
| Skia, ANGLE | BSD 3-Clause |
| HarfBuzz, nghttp2, libpsl, libxml2, brotli, woff2 | MIT |
| FreeType | FTL (scelta al posto di GPLv2, richiede una nota nei crediti) |
| ICU | Licenza Unicode |
| curl | Licenza curl (stile MIT) |
| BoringSSL | Licenze permissive, da verificare sulla revisione usata |
| zlib | Licenza zlib |
| libpng | Licenza libpng |
| libjpeg-turbo | BSD e licenza IJG |
| libwebp | BSD 3-Clause |
| SQLite | Pubblico dominio |
| SDL3 (solo modalità finestra) | Licenza zlib |
| Codice del motore | Da decidere: proposta BSD 2-Clause, coerente con WebKit |

Il layer di embedding eredita la licenza del codice da cui eventualmente deriva: BSD se riprende parti di WebKitLegacy, LGPL se riprende parti del WebCore di Ultralight. Va tracciato file per file fin dall'inizio.

### Cosa fa il motore per la conformità

- **Relink kit**: un target `zig build relink-kit` che produce un archivio con i file oggetto dell'applicazione, lo script di link e le istruzioni. Con questo kit e il sorgente del motore, chiunque può ricollegare l'app con un WebKit modificato.
- **Profilo dinamico**: in alternativa, l'applicazione può linkare il motore come libreria dinamica, e l'obbligo si riduce a distribuire la libreria sostituibile.
- **Notice automatiche**: la build genera il file delle licenze di terze parti con tutti i componenti effettivamente inclusi, da allegare alla distribuzione.
- **Sorgente corrispondente**: ogni release del motore pubblica il sorgente esatto di WebKit e delle patch usate, con tag allineati alle release binarie.

### Applicazioni open source

Per applicazioni con licenza compatibile con la LGPL (GPL, LGPL, MPL e simili) gli obblighi sono già soddisfatti dalla pubblicazione del sorgente, e il relink kit non serve.

## Sicurezza

Con un solo processo **non c'è sandbox per il contenuto web**: un bug di memoria in WebCore o JSC sfruttato da una pagina diventa esecuzione di codice con i privilegi dell'applicazione. Il modello di fiducia quindi è netto: il motore è progettato per contenuto dell'applicazione, e il contenuto esterno è un'eccezione esplicita, documentata come rischio alto.

### Superficie d'attacco

| Superficie | Rischio | Mitigazione |
| --- | --- | --- |
| WebCore e JSC | Bug di memoria sfruttabili da contenuto malevolo, senza sandbox | Contenuto esterno disattivato di default, JIT disattivabile, funzioni web non necessarie escluse dalla build |
| Binding host | Chiamate native da origini non previste | Binding installate solo per le origini `app://` dichiarate, controllo dell'origine per ogni chiamata |
| Schema `app://` | Path traversal, MIME sniffing | Archivio in sola lettura, nessun accesso al filesystem, MIME espliciti |
| Navigazione | Pagina dell'app portata su un sito esterno | Navigazione fuori da `app://` bloccata di default; i link esterni si aprono nel browser di sistema |
| Contenuto esterno in iframe | Codice non fidato nello stesso processo dell'app | Disattivato di default; se abilitato, senza binding e con il JIT spento per tutto il runtime |

### Riduzione della superficie

Il profilo di build è anche uno strumento di sicurezza: ogni funzione esclusa (WebAssembly, WebGL, XSLT, media) è codice che non può essere attaccato. Il profilo minimal è quello consigliato per applicazioni che non caricano contenuto esterno.

### JIT

Il JIT è una scelta di processo, non di view: le opzioni di JSC sono globali e tutte le view condividono la stessa VM JavaScript. L'opzione `jit` di `zg_runtime_config` vale quindi per tutta l'applicazione, e chi carica contenuto non fidato dovrebbe disattivarla, a costo delle prestazioni JS.

### Politica di aggiornamento

- Il motore segue main, dove i fix di sicurezza arrivano per primi; a ogni advisory WebKit critico il commit fissato si aggiorna subito.
- Obiettivo: una release patch del motore entro pochi giorni da un advisory critico.
- Il framework espone la versione del motore e un controllo opzionale delle release, così le applicazioni sanno quando devono essere ricompilate.

### Fuzzing

Il layer di embedding, le binding (conversione dei valori tra JSC e host), gli schemi custom e il loader sono codice nuovo del progetto e vanno fuzzati fin dalla prima versione. WebCore e JSC sono già fuzzati estensivamente upstream.

## Framework Zig

Il framework è il primo utente del motore e la sua demo principale. L'obiettivo è la parità di funzionalità con Wails, con un vantaggio specifico: **binding generate a comptime**, senza reflection a runtime, e con tipi TypeScript esatti. Il target è Zig 0.16.

### Binding generate

L'applicazione definisce una struct con metodi `pub`. A comptime il framework la ispeziona con `@typeInfo` e genera:

- un dispatcher che decodifica gli argomenti, chiama il metodo e codifica il risultato, registrato sul motore come chiamata host;
- un file `bindings.d.ts` e un client `bindings.ts`, prodotti da uno step di `zig build` che esegue un piccolo generatore sullo stesso modulo.

```zig
pub const App = struct {
    pub fn greet(self: *App, name: []const u8) ![]const u8 { ... }
    pub fn listFiles(self: *App, dir: []const u8) ![]FileEntry { ... }
};
```

Nel frontend: `const files: FileEntry[] = await App.listFiles("/tmp")`.

| Tipo Zig | Tipo TypeScript |
| --- | --- |
| Interi, float | `number` (`bigint` per interi oltre 53 bit) |
| `bool` | `boolean` |
| `[]const u8` | `string` |
| Slice e array | `T[]` |
| Struct | `interface` |
| `enum` | union di string literal |
| `union(enum)` | discriminated union `{ tag: "...", value: ... }` |
| `?T` | `T \| null` |
| Error set | `Promise` rifiutata (chiamate asincrone) o eccezione JS (sincrone), con codice di errore tipizzato |

Ogni chiamata riceve un arena allocator liberato a fine chiamata, così i metodi non gestiscono la memoria dei risultati. Le binding sono sincrone di default, perché motore e applicazione condividono il thread; i metodi marcati come asincroni girano su un thread di lavoro e lato JS diventano `Promise`.

### Parità con Wails

| Funzionalità | Versione 1 | Dopo |
| --- | --- | --- |
| Binding tipizzate host → JS | Sì | — |
| Eventi nei due sensi | Sì | — |
| Finestra: titolo, dimensioni, fullscreen, frameless | Sì | Trasparenza ed effetti |
| Più finestre | — | Sì |
| Dialog nativi (apri, salva, messaggio) | Sì | — |
| Menu applicazione e context menu | Sì | — |
| System tray | — | Sì |
| Clipboard | Sì | — |
| Drag and drop di file | Sì | — |
| Istanza singola | Sì | — |
| Associazione file e URL scheme | — | Sì |
| Informazioni sugli schermi | Sì | — |
| Console JS nel terminale | Sì | — |
| Devtools (Web Inspector) | Nel profilo dev | — |

### CLI

- `init`: crea un progetto da template (vanilla, React, Svelte, Vue) con `build.zig` e configurazione del frontend.
- `dev`: avvia il dev server del frontend e l'applicazione con il profilo motore `dev`, rigenera le binding quando cambia il codice Zig.
- `build`: compila il frontend, incorpora gli asset, genera le binding e produce il binario statico di release.
- `package`: per macOS crea il bundle `.app` (obbligatorio sulla piattaforma); per Windows aggiunge icona, manifest e informazioni di versione all'eseguibile.

## Obiettivi misurabili

Ogni metrica si misura in CI a ogni commit, sulla stessa app di esempio, e si confronta con WebView2 e WebKitGTK. Gli obiettivi numerici si fissano dopo la fase 1, quando esiste una prima misura reale.

| Metrica | Come si misura | Obiettivo |
| --- | --- | --- |
| Dimensione del binario | Hello world nel profilo minimal, dopo lo strip dei simboli | Da fissare dopo la fase 1 |
| Memoria a riposo | RSS con una view vuota caricata | Da fissare dopo la fase 1 |
| Memoria con app di esempio | RSS con la demo React aperta | Da fissare dopo la fase 1 |
| Tempo di avvio | Dall'avvio del processo al primo frame mostrato | Da fissare dopo la fase 1 |
| Fluidità | Frame time di un'animazione CSS di riferimento | Da fissare dopo la fase 1 |
| Latenza delle binding | Chiamata vuota sincrona e asincrona, in microsecondi | Da fissare dopo la fase 2 |

Un aumento oltre una soglia rispetto alla release precedente fa fallire la CI, così le regressioni di dimensione e memoria si vedono subito.

## Roadmap

Lo sviluppo parte dal port stesso, su Linux e Windows insieme. Il punto di decisione critico è la fase 1: se il layer di embedding non riesce a far renderizzare WebCore senza WebKit2 con uno sforzo ragionevole, il piano va rivisto prima di investire nel resto.

&#91;embedded content: roadmap · 7 fasi, un gate per fase\]

Ogni fase si chiude solo quando il suo gate è superato. La fase 5 può iniziare appena l'API C della fase 2 è stabile, in parallelo con le fasi 3 e 4. Le durate non sono stimate qui: vanno fissate dopo la fase 0, quando si conoscono i tempi reali di build e di iterazione su WebKit.

## Testing, CI e upstream

Il test più importante è uno solo: **la stessa pagina renderizzata allo stesso modo su tutte le piattaforme**. Con un port unico è finalmente misurabile, e va misurato a ogni commit.

### Livelli di test

| Livello | Cosa verifica | Come |
| --- | --- | --- |
| API C | Creazione e distruzione, errori, callback | Test unitari in modalità CPU |
| Rendering | Output visivo di pagine di riferimento | Modalità CPU e texture, confronto con immagini di riferimento con tolleranza, uguali per tutti gli OS |
| Binding | Conversione dei valori tra JSC e host per ogni tipo supportato | Test generati dalla tabella dei tipi del framework |
| Layer di embedding | Navigazione, form, eventi, storage | Pagine di test dedicate, guidate da script |
| Memoria | Leak, use-after-free, cicli di creazione e distruzione delle view | Build con ASan e LSan nella CI notturna |
| Fuzzing | Conversione dei valori, schemi custom, loader | libFuzzer sul codice del progetto |
| Compatibilità web | Regressioni a ogni rebase di WebKit | Sottoinsieme di Web Platform Tests |

### CI

- Target iniziali: Linux x86\_64 e Windows x64, poi macOS arm64, poi Linux aarch64.
- Cache degli artefatti WebKit per revisione e configurazione.
- Build completa notturna per ogni profilo (minimal, standard, dev).
- Ogni release produce: binari di esempio, sorgente corrispondente, file delle licenze, relink kit di esempio.

### Simboli e crash

- Simboli separati dal binario (file di debug su Linux, PDB su Windows, dSYM su macOS), indicizzati per build ID e pubblicati per ogni release.
- Il motore non include un crash reporter: si integra con quello dell'host (Crashpad, Breakpad, Sentry Native), e i suoi thread hanno nomi riconoscibili nei dump.
- Su Linux i crash handler installano uno stack alternativo per i segnali, che interferisce con il GC di JSC (vedi Convivenza con l'applicazione host). La CI include un test con un crash handler attivo.
- Uno script di simbolizzazione trasforma un minidump in stack trace leggibili con i simboli della release.

### Strategia upstream

- Il layer di embedding single-process difficilmente sarà accettato upstream, dato che WebKit1 è stato rimosso per i port non-Apple. Resta nel repository del progetto.
- Vanno invece proposte upstream le patch generiche: portabilità di `RunLoopGeneric`, build statica, fix al codice Skia e curl di WebCore.
- Aggiornamento del commit di `main` ogni 2–4 settimane, con il job notturno che segnala in anticipo le rotture.
- La metrica di salute è la dimensione della serie di patch sui file upstream, distinta dal codice nuovo del progetto.

## Rischi e questioni aperte

Il rischio che può far fallire il progetto è il **costo di mantenimento del layer di embedding**: le API interne di WebCore cambiano a ogni revisione, e nessun port upstream condivide più quel lavoro. È il prezzo del single-process e del port proprio, ed è lo stesso che paga Ultralight.

### Rischi

| Rischio | Impatto | Mitigazione |
| --- | --- | --- |
| API interne di WebCore che cambiano a ogni rebase | Alto | Layer di embedding piccolo, rebase regolari, test del layer a ogni rebase |
| Contenuto web senza sandbox nel processo dell'app | Alto | Contenuto esterno disattivato di default, profilo minimal, JIT disattivabile |
| Applicazioni statiche con WebKit vulnerabile | Alto | Politica di aggiornamento, controllo di versione nel framework |
| Compositing accelerato da scrivere | Medio | Versione 1 senza compositing, compositor nella versione 2 |
| Backend media da scrivere | Medio | Media disattivati nella versione 1 |
| Backend GPU di Skia su Windows | Medio | Prototipo Windows presto, come gate della roadmap |
| Nessun isolamento dei crash | Medio | Fuzzing del codice del progetto, ASan nella CI |
| Dimensione del binario | Medio | Filtro dei dati ICU, profilo minimal; da misurare al primo milestone |
| Barriera di adozione per la LGPL | Medio | Relink kit e profilo dinamico |
| Concorrenza di Servo | Basso | Differenziarsi su compatibilità e API C |
| Conflitti con l'host: segnali, allocatore, simboli duplicati | Alto | Segnale del GC configurabile, archivio unico con simboli locali, statistiche di memoria esposte, test con runtime host e crash handler |
| JIT bloccato dalle policy della piattaforma | Medio | Rilevamento all'avvio e passaggio all'interprete, entitlement aggiunto dal package su macOS |

### Questioni aperte

- [ ] Nome: deciso, ZeroG (ricerca sui marchi in EUIPO da fare). Licenza del codice del motore ancora da decidere.
- [x] Ramo upstream: deciso, `main` con commit fissato e aggiornamenti regolari.
- [x] Windows: deciso, Skia su ANGLE (GL su D3D11).
- [ ] Compositing nella versione 2: compositor proprio o riuso di TextureMapper?
- [ ] Backend media: FFmpeg unico o backend nativi per OS?
- [ ] `ENABLE_STATIC_JSC` funziona con il port nuovo senza patch?
- [ ] Font: FreeType ovunque o font manager nativo su Windows e macOS?
- [ ] Accessibilità: ponti AT-SPI, UI Automation e NSAccessibility.

* [x] Allocatore: deciso, malloc di sistema. Resta da valutare un allocatore con hardening.
* [ ] Segnale di default per il GC di JSC su Linux, e test con i runtime host più comuni.
* [ ] Integrazione con il crash reporter dell'host: quali supportare ufficialmente?

- [ ] Backend WebSocket nel single-process: codice curl di WebCore ancora presente, o implementazione nel layer di embedding?

## Fonti

- [WPE WebKit's New WPEPlatform API: A Migration Guide](https://lucaberton.com/blog/wpe-webkit-wpeplatform-api-migration-embedded/)
- [WPE WebKit 2.46.0 released](https://wpewebkit.org/release/wpewebkit-2.46.0.html)
- [webkit-dev: Replacing Cairo in WebKit with Skia](https://lists.webkit.org/pipermail/webkit-dev/2024-February/032615.html)
- [webkit-changes: Rename WinCairo port to Win](https://lists.webkit.org/pipermail/webkit-changes/2024-July/304070.html)
- [WebKit Bug 230170: ENABLE\_STATIC\_JSC sul port PlayStation](https://lists.webkit.org/pipermail/webkit-unassigned/2021-September/995296.html)
- [Servo is now available on crates.io](https://servo.org/blog/2026/04/13/servo-0.1.0-release/)
- [Ultralight su GitHub](https://github.com/ultralight-ux/Ultralight)
- [Blitz su GitHub](https://github.com/DioxusLabs/blitz)
- [WebKit, Wikipedia (licenze)](https://en.wikipedia.org/wiki/WebKit)

* [WebKit Bug 254141: rimozione del ResourceHandle curl legacy dopo l'eliminazione di WinCairo WK1](https://lists.webkit.org/pipermail/webkit-unassigned/2023-March/1093883.html)
* [WebCore di Ultralight su GitHub](https://github.com/ultralight-ux/WebCore)
* [WebKit Bug 241736: RunLoopGeneric usato dai port JSCOnly e PlayStation](https://lists.webkit.org/pipermail/webkit-unassigned/2022-June/1042988.html)

- [WebKit Bug 155706: RunLoop per JSCOnly, che dipende solo da ICU](https://bugs.webkit.org/show_bug.cgi?id=155706)
- [WebCore, backend WebCrypto OpenSSL](https://webkit.googlesource.com/WebKit/+/7f131dee9ca99449765f131be8dfd81c800dce88/Source/WebCore/crypto/openssl)
- [WebCore PAL, codec di testo](https://webkit.googlesource.com/WebKit/+/67fdb04ff86da85ec1f6eab7caac0901ec227cfe/Source/WebCore/PAL/pal/text)

* [On WebKit Build Options, Michael Catanzaro](https://blogs.gnome.org/mcatanzaro/2018/11/02/on-webkit-build-options-also-how-to-accidentally-disable-important-security-features/)
* [WebKit Bug 223069: conflitto di JSC\_SIGNAL\_FOR\_GC con Ruby](https://lists.webkit.org/pipermail/webkit-unassigned/2021-March/969495.html)
* [oven-sh/WebKit PR 235: segnale del GC e stack alternativi in Bun](https://github.com/oven-sh/WebKit/pull/235)
