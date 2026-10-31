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
- **Decisione: mimalloc su tutte le piattaforme** (`USE_MIMALLOC=ON`), la copia inclusa in WebKit (`Source/bmalloc/mimalloc`). Stesso allocatore e stesso comportamento su Linux, Windows e macOS. Il malloc di sistema non è un'opzione su Windows: `BPlatform.h` lo rifiuta con un `#error`, perché la memoria allineata non si può liberare con `::free`.
- mimalloc non sostituisce il malloc dell'host (upstream lo compila con `MI_OVERRIDE=OFF`) e i suoi simboli restano privati nel bundle, quindi un host che usa a sua volta mimalloc non va in conflitto (verificato da `tests/link/host_conflicts.c`).
- **Conseguenza**: si perdono Gigacage, gli heap isolati e le altre protezioni dell'heap di WebKit (libpas). Senza sandbox, è una difesa in meno contro i bug di memoria; il contenuto esterno resta ancora più sconsigliato.
- **Hardening opzionale**: `ZEROG_MIMALLOC_SECURE` attiva la modalità secure di mimalloc (guard page, allocazione randomizzata, rilevamento di double free e corruzione delle free list), a costo di prestazioni. Default: spento.
- **Riserve di memoria virtuale**: non spariscono con nessun allocatore. JSC riserva sempre uno Structure heap da 4 GB sui 64 bit, il JIT un pool eseguibile da 1 GB su x86\_64, mimalloc arene da 1 GiB. Con `vm.overcommit_memory=2` su Linux contano nel limite di commit. Togliere libpas elimina solo Gigacage.
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
| Linux | OpenGL (EGL) | Driver caricati dal sistema con `dlopen`. Per gli host Vulkan il motore renderizza su GL in buffer dma-buf che l'host importa (`ZG_SURFACE_VULKAN`, `zg_surface_dmabuf`); GBM e il device DRM sono caricati a runtime solo per questo |
| Windows | OpenGL tramite ANGLE su D3D11 | Il percorso Skia più collaudato su Windows; condivisione diretta delle texture con i giochi D3D11; WebGL incluso |
| macOS | Metal | Il backend Metal di Skia è maturo |

### Compositing

WebCore separa le parti della pagina in layer (trasformazioni CSS, animazioni, video, canvas WebGL) e si aspetta un compositor che li unisca. È la parte più impegnativa del layer di piattaforma.

- **Viste CPU**: compositing accelerato disattivato. WebCore disegna tutto in una sola superficie Skia, con aggiornamento delle sole regioni modificate.
- **Viste GPU** (texture, finestra, Vulkan; deciso e fatto): il compositor Skia upstream dei port GTK e WPE (coordinated graphics, senza TextureMapper), guidato da un thread compositor proprio del motore (`graphics/Compositor`), l'equivalente single-process di `LayerTreeHost` e `ThreadedCompositor` di WebKit2. I tile si dipingono sui worker, le animazioni CSS di transform e opacity producono frame senza aggiornare il rendering della pagina, al ritmo dei frame dell'applicazione. WebGL, i canvas 2D (disegnati sulla GPU) e i video (frame come piani YUV convertiti dalla GPU) sono layer propri.

### Ciclo di un frame

1. L'applicazione chiama `zg_view_render()`, oppure lo fa il loop del motore in modalità finestra.
2. Il motore esegue i task pendenti, aggiorna stile e layout, e ridisegna con Skia solo le regioni modificate.
3. Il risultato è nella swapchain (finestra), nella texture dell'applicazione (texture) o nel buffer di pixel (CPU).
4. La presentazione avvenuta fa avanzare `requestAnimationFrame`, così le animazioni seguono il vsync. Chi dà il ritmo dipende dalla modalità:
   - **finestra**: il motore, con la propria sorgente di vsync (swapchain DXGI con attesa, CVDisplayLink, timing di swap EGL);
   - **texture e CPU**: l'applicazione, che chiama `zg_view_render(view, timestamp)` una volta per frame del proprio loop. Il motore non stima il ritmo da solo, così l'UI resta allineata al frame del gioco.

   Nel port, l'aggiornamento del rendering è guidato dal layer di embedding (un `DisplayRefreshMonitor` proprio), perché con `RunLoopGeneric` gli observer del run loop non vengono eseguiti.

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

BoringSSL non usa da solo gli store di sistema. Il motore usa le CA radice del sistema: il bundle dai percorsi standard su Linux, lo store di Windows tramite curl (`CURLSSLOPT_NATIVE_CA`), il Keychain su macOS (da fare con il port macOS). L'applicazione aggiunge CA proprie con `zg_runtime_add_ca_certificates` (PEM, anche un bundle incorporato nel binario per ambienti embedded); valgono anche su Windows e per `wss://` (patch 0022). Un certificato rifiutato dà un errore leggibile ("certificate not trusted").

### Loader delle risorse

Il layer di embedding implementa il caricamento: `app://` e gli schemi custom rispondono direttamente sul thread principale; `http` e `https` passano a curl sul thread di rete, e le risposte tornano al thread principale a pezzi, in streaming.

### Cache HTTP

La cache su disco di WebKit vive nel network process di WebKit2, quindi non è disponibile. Il motore ha una cache su disco propria (`loader/HTTPCache`) accanto a quella in memoria di WebCore: per sessione nella directory dati (nessuna nei contesti effimeri), con limite configurabile (`zg_context_config.http_cache_limit`), freschezza e validazione HTTP (max-age, ETag, Last-Modified, `Vary`), eviction dei meno usati.

### Cache API

Anche la Cache API (`caches`, usata dalle pagine e dai worker) in WebKit2 vive nel network process. Nei profili standard, dev e debug il motore ha uno storage proprio (`storage/CacheStorageZeroG`) sul thread principale: un file per origine in `<data_dir>/CacheStorage/`, riscritto per intero a ogni modifica su una WorkQueue, nessun file nei contesti effimeri. Il matching segue `DOMCacheEngine::queryCacheMatch` (con `Vary`), le risposte opache rispettano la Cross-Origin-Resource-Policy, la quota è di 512 MB per origine. Nel profilo minimal `caches` non esiste.

### Schema custom `app://`

- Gli asset del frontend (HTML, JS, CSS, immagini, font) sono incorporati nel binario come archivio compresso, generato a compile time.
- Lo schema risponde dall'archivio con MIME type corretto e supporto alle range request per audio e video.
- Lo schema va registrato come **sicuro** e con CORS abilitato, altrimenti API come `crypto.subtle` e `fetch` cross-origin non funzionano.
- L'applicazione può registrare schemi aggiuntivi con un callback (`zg_context_register_scheme`), per esempio per servire file generati a runtime.

### Dev mode

In sviluppo la view carica direttamente il dev server del frontend (per esempio Vite su `http://localhost:5173`), e il passaggio a `app://` avviene solo nella build di release. L'hot reload passa da HTTP e **WebSocket**: senza il network process di WebKit2 il backend WebSocket lo fornisce il layer di embedding, sopra curl (`loader/WebSocketChannelZeroG`), con subprotocol, cookie dell'handshake, messaggi di testo e binari e `wss://`. Il dev server è fidato solo nel profilo dev (`dev_server_url`).

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

- Clipboard di sistema in lettura e scrittura, con testo, HTML e immagini (`zg_runtime_set_clipboard`, oppure quella di SDL3 in modalità finestra); `navigator.clipboard` incluso.
- Drag and drop di file, testo e URL verso la view (`zg_view_drag`), e drag HTML5 dentro la pagina; trascinamento fuori dalla view più avanti.
- File chooser e dialog JS (`alert`, `confirm`, `prompt`) sono callback verso l'host, con un'implementazione nativa di default in modalità finestra. Il context menu non c'è ancora.

### Accessibilità

WebCore costruisce l'albero di accessibilità, ma il ponte verso lo screen reader è specifico di ogni OS, e nel port proprio va scritto. Fatti (modalità finestra): **UI Automation** su Windows, un provider scritto da zero (nomi, ruoli, stati, Invoke, Value, Toggle, Text, eventi); **AT-SPI** su Linux, un ponte proprio su libdbus caricata a runtime, senza GLib (Accessible, Application, Component, Action, Value, Text, eventi). Restano NSAccessibility con il port macOS e l'accessibilità in modalità texture, dove la finestra è dell'host.

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

- **Chiamate sincrone (default)**: il callback nativo gira durante la chiamata JS e il valore di ritorno arriva subito al JavaScript (`const r = App.greet("Daniele")`).
- **Chiamate asincrone (supportate, da dichiarare)**: per lavoro lungo il callback crea una `Promise` con `zg_call_promise`, la restituisce subito al JavaScript ed esegue il lavoro su un thread dell'applicazione. `zg_promise_resolve` / `zg_promise_reject` si possono chiamare da qualunque thread: ricevono dati C semplici (JSON o UTF-8) che il motore copia, e il motore risolve la `Promise` sul thread principale. I valori JSC (`zg_value`) non passano mai tra thread.
- **Installazione**: le binding si installano quando WebCore crea l'oggetto `window` di un frame (il callback di "window object cleared" del frame loader client), solo per le origini fidate, e solo nel frame principale salvo richiesta esplicita. L'origine si controlla anche a ogni chiamata, non solo all'installazione.
- **Origini fidate**, in tre livelli:
  1. **automatico (default)**: gli asset dell'applicazione (`app://` e gli schemi registrati con `zg_context_register_scheme`). Nessuna configurazione.
  2. **dev server**: un solo campo, `dev_server_url` in `zg_context_config`, impostato dal framework nel profilo dev. Il motore lo ignora nei profili di release, quindi non può finire in produzione per errore.
  3. **policy opzionale**: un callback dell'host che decide a runtime se un'origine può navigare o ricevere le binding, per i casi che i primi due livelli non coprono. Facoltativo.
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
- **Web Inspector**: nei profili dev e debug il motore avvia l'inspector remoto di WebKit su una porta locale, e il frontend dell'inspector (incluso nel binario solo in questi profili) si apre in una seconda finestra. Nei profili minimal e standard non c'è, per dimensione e superficie d'attacco.
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

- **Profilo minimal**: nessun backend media, quindi niente riproduzione. `ENABLE_VIDEO` resta acceso senza backend: nessun port upstream compila con video spento e WebCore compila il codice media senza guardie, quindi spegnerlo vorrebbe dire una serie di patch crescente. Gli elementi `<video>` e `<audio>` esistono e segnalano che non possono riprodurre. `ENABLE_WEB_AUDIO` e `ENABLE_MEDIA_SOURCE` sono spenti (combinazione già usata dal port PlayStation).
- **Profili standard, dev e debug** (fatto): nei profili standard, dev e debug un motore proprio (`port/embedding/media`) su librerie separate, tutte senza royalty e con licenze permissive, al posto di FFmpeg: libwebm per WebM, libvpx (VP8, VP9), dav1d (AV1), libopus, libogg/opusfile/libvorbis per Ogg, dr_libs per MP3, FLAC e WAV, libyuv per la conversione dei frame. Nessun codec brevettato nel binario: H.264 e AAC in MP4 passano dal sistema operativo dove c'è (Media Foundation su Windows, con i decoder del sistema; su Linux non si riproducono). I file MP4 con VP9, AV1 e Opus si riproducono ovunque con i decoder del motore (le sample table del moov, o i moof di un file frammentato). I controlli nativi sono i modern media controls di WebKit con le icone Adwaita compilate nel motore (patch 0033). Media Source Extensions per WebM e MP4 frammentato: MediaSource e SourceBuffer di WebCore tengono i frame, il port analizza il byte stream (WebM con il parser a callback di libwebm, MP4 con un parser di box proprio) e riproduce i frame accodati con gli stessi decoder; H.264 e AAC da MSE passano dai decoder di Media Foundation su Windows (patch 0031). Nelle viste composite il video è un layer suo: i frame vanno al compositor senza ridipingere la pagina (patch 0032), come piani YUV che la GPU converte in uno shader (nessuna conversione sulla CPU). Su Windows H.264 si decodifica sulla GPU (DXVA, con un device D3D11 condiviso dai decoder di Media Foundation) dove la GPU lo consente. Nelle viste GPU anche i canvas 2D disegnano sulla GPU (Ganesh) come layer propri. L'audio esce con SDL3 (WASAPI; ALSA, PulseAudio, PipeWire caricati a runtime). Web Audio usa lo stesso motore: la destinazione è uno stream SDL3, `decodeAudioData` passa dai decoder dei media, la FFT è pffft (patch 0030), le risposte HRTF di WebKit sono compilate nel motore. Il profilo minimal resta senza motore e senza Web Audio.

### Profili di build

- **minimal**: niente media, niente WebGL, dati ICU ridotti. L'obiettivo è il binario più piccolo possibile per UI di applicazione.
- **standard**: aggiunge WebGL, WebAssembly, il JIT FTL, il motore media, Web Audio e Media Source Extensions.
- **dev**: motore in release con il Web Inspector abilitato. È il profilo di `zerog dev`, per sviluppare l'applicazione.
- **debug**: simboli completi e assert attive (build Debug di WebKit). Per lavorare sul motore stesso; più lento e molto più grande.

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
| GTK, Wayland, X11 | Sostituire | Finestra e input | Core senza finestra; modalità finestra con SDL3 statico, che carica X11 e Wayland a runtime con `dlopen` (e nei profili con media l'audio: ALSA, PulseAudio, PipeWire, anch'essi a runtime) |
| libepoxy | Sostituire | Caricamento delle funzioni OpenGL | Skia carica le funzioni GL e Vulkan con `eglGetProcAddress` e `vkGetInstanceProcAddr` |
| GStreamer | Sostituire | Audio e video tramite plugin | Motore media proprio su librerie separate (vedi Media): libwebm, libvpx, dav1d, libopus, libogg, opusfile, libvorbis, dr_libs, libyuv, pffft per Web Audio; H.264 e AAC dai decoder del sistema su Windows |
| libdrm, GBM | Togliere | DMA-BUF tra processi | Inutili per il trasporto tra processi; per la superficie Vulkan su Linux GBM e le ioctl DRM si caricano a runtime, senza dipendenza di build |
| libxslt | Togliere | XSLT | `ENABLE_XSLT=OFF` |
| lcms2 | Togliere | Profili colore ICC | Skia gestisce gli spazi colore |
| libavif | Togliere | AVIF | Da riattivare nel profilo standard (dav1d c'è già, per i video AV1) |
| libjxl | Togliere | JPEG XL | Escluso: poco usato sul web (Chrome e Firefox non lo mostrano di default), e libjxl porta con sé highway e lcms2, circa 2 MB |
| libhyphen | Togliere | `hyphens: auto` | — |
| enchant | Togliere | Controllo ortografico | — |
| libmanette | Togliere | Gamepad API | — |
| libsecret | Togliere | Archivio credenziali | — |
| ATK, AT-SPI | Sostituire | Accessibilità su Linux | Ponte AT-SPI proprio su libdbus caricata a runtime, senza ATK né GLib |
| libsystemd, libseccomp, bubblewrap, xdg-dbus-proxy | Togliere | Logging e sandbox dei port multi-processo | — |
| libidn2 | Togliere | Domini internazionalizzati in curl | WebCore converte già gli host in punycode con ICU |
| c-ares | Togliere | DNS asincrono | Basta il resolver a thread di curl |
| ANGLE | Tenere | WebGL; OpenGL su Windows | Necessario su Windows come backend GPU di Skia (GL su D3D11); su Linux e macOS solo nel profilo standard, per WebGL |

### Funzioni da spegnere

| Funzione | Flag CMake | minimal | standard |
| --- | --- | --- | --- |
| Video e audio | `ENABLE_VIDEO` | Acceso, senza backend (nessuna riproduzione) | Con backend media |
| Web Audio | `ENABLE_WEB_AUDIO` | No | Con backend media |
| Media Source Extensions | `ENABLE_MEDIA_SOURCE` | No | Sì (WebM, MP4 frammentato) |
| WebGL | `ENABLE_WEBGL` | No | Sì |
| WebAssembly | `ENABLE_WEBASSEMBLY` | No | Sì |
| JIT FTL (quarto livello di JSC) | `ENABLE_FTL_JIT` | No: il suo backend B3 compila solo con WebAssembly; restano interprete, Baseline e DFG | Sì |
| XSLT | `ENABLE_XSLT` | No | No |
| WebRTC e MediaStream | `ENABLE_WEB_RTC`, `ENABLE_MEDIA_STREAM` | No | No |
| Encrypted Media | `ENABLE_ENCRYPTED_MEDIA` | No | No |
| Gamepad | `ENABLE_GAMEPAD` | No | No |
| Sintesi vocale | `ENABLE_SPEECH_SYNTHESIS` | No | No |
| Cache API | `CacheAPIEnabled` (impostazione) | No | Sì |
| Service worker | `ENABLE_SERVICE_WORKER` | No | Previsti (oggi spenti) |
| Web Inspector remoto | `ENABLE_REMOTE_INSPECTOR` | No | Solo nei profili dev e debug |
| AVIF | `USE_AVIF` | No | Previsto (oggi spento) |
| JPEG XL | `USE_JPEGXL` | No | No |
| WOFF2 | `USE_WOFF2` | Sì | Sì |

I nomi dei flag vanno verificati sulla revisione scelta: cambiano tra versioni, e alcuni sono `ENABLE_` mentre altri sono `USE_`.

### Cosa resta dinamico a runtime

| Piattaforma | Caricato dal sistema | Come |
| --- | --- | --- |
| Linux | glibc, driver GPU (`libEGL`, `libGL`, `libvulkan`) | Build contro una glibc di base vecchia (per esempio 2.28) con zig, così il binario parte anche su distro datate. musl statico è escluso: non può caricare i driver GPU con `dlopen` |
| Windows | Solo DLL di sistema (kernel32, user32, d3d11, dxgi, crypt32, ws2\_32) | CRT statica (`/MT`), unica variante distribuita: un host MSVC deve compilare anche lui con `/MT`, perché il linker rifiuta di mescolare runtime diversi (`/FAILIFMISMATCH`) |
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
| WebCore, JavaScriptCore | [LGPL](https://en.wikipedia.org/wiki/WebKit) (v2 "o successiva", circa 200 file in JSC) |
| WTF | Misto: in gran parte BSD 2-Clause, ma circa 100 file sono LGPL v2 "o successiva", anche centrali (`DateMath`, `dtoa`, `FastMalloc.h`, `Forward.h`). Ai fini dell'obbligo di relink va trattata come LGPL |
| bmalloc, resto di WebKit | BSD 2-Clause |
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
| SDL3 (modalità finestra, audio dei media) | Licenza zlib |
| Media (standard, dev, debug): libwebm, libvpx, libopus, libogg, opusfile, libvorbis, libyuv | BSD 3-Clause (libvpx anche ISC) |
| Media: dav1d | BSD 2-Clause |
| Media: dr_libs (dr_mp3, dr_flac, dr_wav) | Pubblico dominio (Unlicense) o MIT-0 |
| Web Audio (standard, dev, debug): pffft | Licenza FFTPACK (stile BSD) |
| Codice del motore | Da decidere: proposta BSD 2-Clause, coerente con WebKit |

Il layer di embedding eredita la licenza del codice da cui eventualmente deriva: BSD se riprende parti di WebKitLegacy, LGPL se riprende parti del WebCore di Ultralight. Va tracciato file per file fin dall'inizio (`docs/PROVENANCE.md`). In ogni caso si linka a WTF, JSC e WebCore, quindi l'obbligo di relink copre l'intero archivio del motore: il relink kit resta lo strumento giusto, con lo stesso design.

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
| Binding host | Chiamate native da origini non previste | Binding solo per le origini fidate (asset dell'app; dev server solo nel profilo dev; policy opzionale dell'host), solo nel frame principale di default, controllo dell'origine per ogni chiamata |
| Schema `app://` | Path traversal, MIME sniffing | Archivio in sola lettura, nessun accesso al filesystem, MIME espliciti |
| Navigazione | Pagina dell'app portata su un sito esterno | Navigazione fuori da `app://` bloccata di default; i link esterni si aprono nel browser di sistema |
| Contenuto esterno (navigazione e iframe) | Codice non fidato nello stesso processo dell'app | Disattivato di default. Si abilita solo con un flag esplicito dello sviluppatore (`allow_external_content` in `zg_context_config`), che se ne assume la responsabilità: il motore non aggiunge una sandbox e la sicurezza dipende da quella di WebKit upstream e dalla rapidità degli aggiornamenti. Le binding non vengono comunque installate per origini non fidate; il JIT segue la sua opzione e non viene spento automaticamente |

### Riduzione della superficie

Il profilo di build è anche uno strumento di sicurezza: ogni funzione esclusa (WebAssembly, WebGL, XSLT, media) è codice che non può essere attaccato. Il profilo minimal è quello consigliato per applicazioni che non caricano contenuto esterno.

### JIT

Il JIT è una scelta di processo, non di view: le opzioni di JSC sono globali e tutte le view condividono la stessa VM JavaScript. L'opzione `jit` di `zg_runtime_config` vale quindi per tutta l'applicazione ed è indipendente da `allow_external_content`. La documentazione raccomanda di disattivarla a chi carica contenuto non fidato (le parti ottimizzanti del JIT sono la fonte più frequente di bug sfruttabili in JSC, ed è ciò che spegne anche il Lockdown Mode di Apple), a costo delle prestazioni JS, ma la scelta resta allo sviluppatore.

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
    // Metodi che girano su un thread di lavoro e lato JS restituiscono una Promise.
    pub const async_methods = .{"listFiles"};

    pub fn greet(self: *App, name: []const u8) ![]const u8 { ... }
    pub fn listFiles(self: *App, dir: []const u8) ![]FileEntry { ... }
};
```

Nel frontend: `const msg: string = App.greet("Daniele")` (sincrona, senza `await`) e `const files: FileEntry[] = await App.listFiles("/tmp")` (asincrona). Il generatore emette `greet(name: string): string` e `listFiles(dir: string): Promise<FileEntry[]>` in `bindings.d.ts`; un nome in `async_methods` che non corrisponde a un metodo `pub` è un errore di compilazione.

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

Ogni chiamata riceve un arena allocator, così i metodi non gestiscono la memoria dei risultati: per le chiamate sincrone si libera al ritorno del callback, per quelle asincrone quando la `Promise` è stata risolta o rifiutata. Le binding sono sincrone di default, perché motore e applicazione condividono il thread; i metodi elencati in `async_methods` girano su un thread di lavoro, il risultato viene serializzato e lato JS diventano `Promise`.

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
| Compositing accelerato da mantenere | Medio | Compositor Skia upstream (coordinated graphics) con un thread compositor proprio piccolo; viste CPU senza compositing |
| Motore media da mantenere | Medio | Librerie separate e stabili, decoder condivisi tra file e MSE, test con fixture per ogni formato e un sito reale (`ZeroGSite`) |
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
- [x] Compositing: deciso e fatto, il compositor Skia upstream (coordinated graphics) con un thread compositor proprio; niente TextureMapper.
- [x] Backend media: deciso e fatto, librerie separate senza royalty al posto di FFmpeg, più i decoder del sistema per H.264 e AAC (Media Foundation su Windows).
- [x] JSC statico: WTF, JavaScriptCore e PAL si compilano come archivi statici (`*_LIBRARY_TYPE STATIC` nel port), senza patch.
- [x] Font: per ora FreeType ovunque, con il font manager custom di Skia; il font manager nativo resta un'opzione non fatta.
- [ ] Accessibilità: AT-SPI e UI Automation fatti (modalità finestra); restano NSAccessibility e la modalità texture.

* [x] Allocatore: deciso, mimalloc su tutte le piattaforme; modalità secure disponibile (`ZEROG_MIMALLOC_SECURE`), spenta di default.
* [x] CRT su Windows: deciso, solo `/MT`.
* [x] Binding: decise, sincrone di default, asincrone supportate se dichiarate (`async_methods`).
* [x] Licenze: WTF va trattata come LGPL; l'obbligo di relink copre l'intero archivio, il design del relink kit resta invariato.
* [ ] Segnale per il GC di JSC su Linux: configurabile (`zg_runtime_config.gc_signal`, default un segnale real-time libero); mancano i test con i runtime host più comuni e con un crash handler con stack alternativo.
* [ ] Integrazione con il crash reporter dell'host: quali supportare ufficialmente?

- [x] Backend WebSocket nel single-process: deciso e fatto, implementazione nel layer di embedding sopra curl.
- [x] Cache API: decisa e fatta nei profili standard, dev e debug, con uno storage proprio nel layer di embedding; spenta in minimal.
- [ ] Service worker: decisi per i profili standard, dev e debug (spenti in minimal), da fare.
- [ ] AVIF nel profilo standard, dev e debug: da fare (libavif sopra dav1d).
- [x] JPEG XL: deciso, escluso da tutti i profili (poco diffuso, dipendenze pesanti).
- [ ] Context menu: callback verso l'host e menu nativo in modalità finestra.

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
