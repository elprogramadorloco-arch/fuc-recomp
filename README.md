# FateUnlimitedCodesRecomp

> **v0.0.4:** Test `update.bat`. The Configuration window now displays the
> installed version as `FateUnlimitedCodesRecomp - vx.x.x Configuration`.

> **v0.0.3 FFmpeg download fix / corrección de descarga FFmpeg / correção do
> download do FFmpeg:** the verified archive SHA-256 is now pinned directly and
> transient downloads are retried through `curl.exe` with a PowerShell fallback.
> The build no longer depends on a separate checksum-manifest request.

> **v0.0.2 standalone-build fix / corrección de compilación autónoma / correção
> da compilação autônoma:** the public package now includes the original PSP
> native host/HLE sources under `native/`, downloads a pinned SHA-256-verified
> FFmpeg LGPL dependency, and creates every generated folder beside `run.bat`.
> It never requires an unpublished parent repository. See
> `BUILD_ARCHITECTURE.md` and `THIRD_PARTY_NOTICES.md`.

[English](#english) · [Español](#español) · [Português](#português)

---

## English

### What is this project?

FateUnlimitedCodesRecomp is a hybrid native Windows version of
*Fate/unlimited codes*. It uses the PSP release as its static-recompilation and
gameplay base, with the project direction of combining it with the
higher-detail PS2 assets and PS2-style controls. The current public build
establishes the PSP base; PS2 asset replacement is in progress and PS2 controls
remain planned. It is not a PSP emulator and it does not include the game. The
user generates a private local build from a lawfully obtained copy.

The project uses a version of **PSPRecomp modified specifically for
FateUnlimitedCodesRecomp**. PPSSPP is used only as a user-supplied preparation
tool for its official decrypted-EBOOT dump function; it is not required to run
the finished native build.

### Requirements

- 64-bit Windows.
- CMake 3.22 or newer.
- A compatible MSVC x64 C++ toolchain from Visual Studio or Build Tools
  2019/2022. The complete Visual Studio IDE is not required, and Community,
  Professional, Enterprise and standalone Build Tools installations are
  accepted when the C++ x64 workload is installed.
- The European PSP release of *Fate/unlimited codes*:
  - required filename: `game.iso`;
  - game ID: `NPEH00007`;
  - disc version: `1.01`;
  - ISO size: `738,394,112` bytes;
  - ISO SHA-256: `BDF13BEFFBAD519FD51F1AB075D165D3C7BFA075FEEF1A5B1FD7775BAC0F830D`.
- `font.zip`, containing these 18 PSP PGF fonts:
  - `jpn0.pgf` and `kr0.pgf`;
  - `ltn0.pgf` through `ltn15.pgf`.
- An official PPSSPP Windows x64 portable ZIP, version **1.20.4 or newer**.
  Future versions are accepted while they continue to include
  `PPSSPPWindows64.exe`, the required assets and the compatible official EBOOT
  dump function.

### How to use it

1. Place these three files directly in `data`, using these exact filenames:
   `game.iso`, `font.zip` and `ppsspp.zip`. Other names are rejected.
2. Run `run.bat`.
3. The TUI validates the ISO, every required font, PPSSPP, CMake and the C++
   toolchain before creating anything.
4. PPSSPP is extracted from your `ppsspp.zip`, started with an isolated
   memstick and closed automatically as soon as the verified EBOOT dump is
   complete. No menu input or manual close is required.
5. Wait for the progress bar to reach 100%. The private native build is written
   to `out`.
6. Open `out\FateUnlimitedCodesRecomp.exe`.

Do not publish `data`, `out` or `tools\.build`. They may contain user inputs,
extracted game files or generated derivatives.

### Updates

When a newer GitHub Release is available, the launcher can show a **What's
New** window. Selecting **Update** downloads a source-only update ZIP, verifies
its SHA-256, closes the launcher and starts `update.bat`.

The update TUI validates the installed private data and build environment,
shows progress and builds only what the release manifest requires. Launcher-only
updates reuse the native runtime and AOT corpus. Runtime updates reuse cached
AOT units when compatible. Full AOT regeneration is reserved for incompatible
code-generator or ABI changes. `data` and `save` are preserved, and a failed
update restores the previous launcher/runtime.

### Roadmap

| Step | Status | Note |
| --- | --- | --- |
| Base PSP version | OK | May still contain errors; battle voice gain remains to be corrected. |
| Replacement with PS2 assets | In progress | Deferred work is integrated only when it does not break the verified PSP base. |
| PS2 controls and AI implementation | Planned | Design and verification are pending. |
| Online gameplay | Planned | Network architecture has not been implemented. |
| Mods: images and textures | Planned | A legal user-provided mod path is planned. |
| Mods: models and stages | Planned | Planned after the stable base and asset pipeline. |

### Legal notice

This project is an interoperability-oriented source project and is not legal
advice. It is not affiliated with or endorsed by Capcom, Type-Moon, Cavia,
Sony, or any other rightsholder. All trademarks and copyrighted game material
belong to their respective owners.

This repository and its Releases must not contain or redistribute game ISOs or
CSOs, encrypted or decrypted EBOOT files, extracted game files, game audio,
video, textures, models, fonts, save data, savestates, generated AOT source,
objects or executables incorporating game-derived code. Users must provide
their own lawfully obtained inputs and create private derivatives locally.
Owning a copy does not automatically grant redistribution rights.

The original launcher, preparation and update code in this directory is
licensed under the included MIT License. The included PSPRecomp snapshot is a
project-specific modified version distributed under its MIT License; its
license and notices must remain intact. PPSSPP is GPL-2.0-or-later and is not
bundled: users provide an official archive independently. Any redistribution
of PPSSPP must separately comply with its license and source obligations.

The generated `out` directory is private and must not be uploaded without a
separate rights review. See `LEGAL.md` for the canonical scope.

---

## Español

### ¿Qué es este proyecto?

FateUnlimitedCodesRecomp es una versión híbrida nativa para Windows de
*Fate/unlimited codes*. Usa la edición de PSP como base de recompilación
estática y jugabilidad, con la dirección de combinarla con los assets de mayor
detalle de PS2 y controles al estilo de PS2. La compilación pública actual
establece la base PSP; el cambio a assets de PS2 está en progreso y los
controles de PS2 permanecen en planeación. No es un emulador de PSP y no incluye
el juego. El usuario genera una compilación local privada desde una copia
obtenida legalmente.

El proyecto utiliza una versión de **PSPRecomp modificada específicamente para
FateUnlimitedCodesRecomp**. PPSSPP se usa únicamente como herramienta de
preparación aportada por el usuario para su función oficial de volcado del
EBOOT descifrado; no es necesario para ejecutar la compilación nativa terminada.

### Requisitos

- Windows de 64 bits.
- CMake 3.22 o posterior.
- Un toolchain C++ MSVC x64 compatible de Visual Studio o Build Tools
  2019/2022. No es necesario instalar el IDE completo. Se aceptan Community,
  Professional, Enterprise y Build Tools independientes cuando está instalada
  la carga de trabajo C++ x64.
- La edición europea para PSP de *Fate/unlimited codes*:
  - nombre de archivo obligatorio: `game.iso`;
  - ID del juego: `NPEH00007`;
  - versión del disco: `1.01`;
  - tamaño de la ISO: `738.394.112` bytes;
  - SHA-256 de la ISO: `BDF13BEFFBAD519FD51F1AB075D165D3C7BFA075FEEF1A5B1FD7775BAC0F830D`.
- `font.zip`, que debe contener estas 18 fuentes PGF de PSP:
  - `jpn0.pgf` y `kr0.pgf`;
  - desde `ltn0.pgf` hasta `ltn15.pgf`.
- Un ZIP portable oficial de PPSSPP para Windows x64, versión **1.20.4 o
  posterior**. Se aceptan versiones futuras mientras conserven
  `PPSSPPWindows64.exe`, los assets necesarios y la función oficial compatible
  de volcado del EBOOT.

### Cómo se usa

1. Coloca estos tres archivos directamente en `data`, usando exactamente estos
   nombres: `game.iso`, `font.zip` y `ppsspp.zip`. No se aceptan otros nombres.
2. Ejecuta `run.bat`.
3. La TUI valida la ISO, todas las fuentes, PPSSPP, CMake y el toolchain C++
   antes de crear archivos.
4. PPSSPP se extrae de tu `ppsspp.zip`, se inicia con un memstick aislado y se
   cierra automáticamente en cuanto termina el EBOOT verificado. No debes
   responder menús ni cerrar la ventana manualmente.
5. Espera a que la barra de progreso llegue al 100 %. La compilación nativa
   privada aparecerá en `out`.
6. Abre `out\FateUnlimitedCodesRecomp.exe`.

No publiques `data`, `out` ni `tools\.build`. Pueden contener entradas del
usuario, archivos extraídos del juego o derivados generados.

### Actualizaciones

Cuando exista una Release nueva en GitHub, el launcher puede mostrar una
ventana **What's New**. Al seleccionar **Update**, descarga un ZIP que contiene
solo las fuentes públicas, comprueba su SHA-256, cierra el launcher e inicia
`update.bat`.

La TUI de actualización valida los datos privados instalados y el entorno de
compilación, muestra el progreso y compila únicamente lo indicado por el
manifiesto. Una actualización del launcher reutiliza runtime y corpus AOT. Una
actualización del runtime reutiliza el AOT almacenado cuando es compatible. El
corpus completo solo se regenera ante cambios incompatibles del generador o
ABI. `data` y `save` se conservan, y si algo falla se restaura el launcher y
runtime anteriores.

### Mapa de ruta

| Paso | Estado | Nota |
| --- | --- | --- |
| Versión base PSP | OK | Puede tener algún error; falta corregir la ganancia de las voces en batalla. |
| Cambio por los assets PS2 | En progreso | Se integrará sin romper la base PSP verificada. |
| Implementación de controles y AI PS2 | En planeación | Falta diseño y verificación. |
| Implementación de juego en línea | En planeación | La arquitectura de red todavía no está implementada. |
| Implementación de mods: imágenes y texturas | En planeación | Se planifica una ruta legal con archivos aportados por el usuario. |
| Implementación de mods: modelos y escenarios | En planeación | Se realizará después de estabilizar la base y el pipeline de assets. |

### Aviso legal

Este es un proyecto de código fuente orientado a interoperabilidad y no es
asesoría legal. No está afiliado ni respaldado por Capcom, Type-Moon, Cavia,
Sony ni ningún otro titular. Todas las marcas y el material protegido del juego
pertenecen a sus respectivos propietarios.

El repositorio y sus Releases no deben contener ni redistribuir ISO/CSO del
juego, EBOOT cifrados o descifrados, archivos extraídos, audio, vídeo, texturas,
modelos, fuentes, partidas, savestates, fuentes AOT generadas, objetos o
ejecutables que incorporen código derivado del juego. Cada usuario debe aportar
sus propias entradas obtenidas legalmente y generar los derivados de forma
local y privada. Poseer una copia no concede automáticamente derechos de
redistribución.

El código original del launcher, preparación y actualización de esta carpeta
se distribuye bajo la licencia MIT incluida. El snapshot incluido de PSPRecomp
es una versión modificada específicamente para este proyecto y se distribuye
bajo su licencia MIT; deben conservarse sus licencias y avisos. PPSSPP usa
GPL-2.0-or-later y no se incluye: el usuario aporta por separado un archivo
oficial. Redistribuir PPSSPP exige cumplir independientemente sus obligaciones
de licencia y código fuente.

El directorio generado `out` es privado y no debe subirse sin una revisión de
derechos independiente. Consulta `LEGAL.md` para el alcance canónico.

---

## Português

### O que é este projeto?

FateUnlimitedCodesRecomp é uma versão híbrida nativa para Windows de
*Fate/unlimited codes*. Ela usa a edição de PSP como base de recompilação
estática e jogabilidade, com a direção de combiná-la com os assets de maior
detalhe do PS2 e controles no estilo do PS2. A compilação pública atual
estabelece a base PSP; a substituição por assets do PS2 está em andamento e os
controles do PS2 permanecem planejados. Não é um emulador de PSP e não inclui o
jogo. O usuário gera uma compilação local privada a partir de uma cópia obtida
legalmente.

O projeto utiliza uma versão do **PSPRecomp modificada especificamente para o
FateUnlimitedCodesRecomp**. O PPSSPP é usado somente como ferramenta de
preparação fornecida pelo usuário para a função oficial de dump do EBOOT
descriptografado; ele não é necessário para executar a compilação nativa final.

### Requisitos

- Windows de 64 bits.
- CMake 3.22 ou mais recente.
- Um toolchain C++ MSVC x64 compatível do Visual Studio ou Build Tools
  2019/2022. A IDE completa não é obrigatória. Community, Professional,
  Enterprise e Build Tools independentes são aceitos com a carga C++ x64.
- A edição europeia para PSP de *Fate/unlimited codes*:
  - nome de arquivo obrigatório: `game.iso`;
  - ID do jogo: `NPEH00007`;
  - versão do disco: `1.01`;
  - tamanho da ISO: `738.394.112` bytes;
  - SHA-256 da ISO: `BDF13BEFFBAD519FD51F1AB075D165D3C7BFA075FEEF1A5B1FD7775BAC0F830D`.
- `font.zip`, contendo estas 18 fontes PGF do PSP:
  - `jpn0.pgf` e `kr0.pgf`;
  - `ltn0.pgf` até `ltn15.pgf`.
- Um ZIP portátil oficial do PPSSPP para Windows x64, versão **1.20.4 ou mais
  recente**. Versões futuras são aceitas enquanto mantiverem
  `PPSSPPWindows64.exe`, os assets necessários e a função oficial compatível de
  dump do EBOOT.

### Como usar

1. Coloque estes três arquivos diretamente em `data`, usando exatamente estes
   nomes: `game.iso`, `font.zip` e `ppsspp.zip`. Outros nomes são rejeitados.
2. Execute `run.bat`.
3. A TUI valida a ISO, todas as fontes, o PPSSPP, o CMake e o toolchain C++ antes
   de criar qualquer arquivo.
4. O PPSSPP é extraído do seu `ppsspp.zip`, iniciado com um memstick isolado e
   fechado automaticamente assim que o dump verificado do EBOOT termina. Não é
   necessário responder menus nem fechar a janela manualmente.
5. Aguarde a barra de progresso chegar a 100%. A compilação nativa privada será
   criada em `out`.
6. Abra `out\FateUnlimitedCodesRecomp.exe`.

Não publique `data`, `out` nem `tools\.build`. Essas pastas podem conter
entradas do usuário, arquivos extraídos do jogo ou derivados gerados.

### Atualizações

Quando uma nova Release estiver disponível no GitHub, o launcher pode mostrar
uma janela **What's New**. Ao selecionar **Update**, ele baixa um ZIP contendo
somente as fontes públicas, verifica o SHA-256, fecha o launcher e inicia
`update.bat`.

A TUI de atualização valida os dados privados instalados e o ambiente de
compilação, mostra o progresso e compila somente o que o manifesto exigir.
Atualizações apenas do launcher reutilizam o runtime e o corpus AOT. Mudanças
de runtime reutilizam o AOT em cache quando compatível. O corpus completo só é
regenerado após mudanças incompatíveis de gerador ou ABI. `data` e `save` são
preservados, e uma falha restaura o launcher/runtime anterior.

### Roteiro

| Etapa | Estado | Nota |
| --- | --- | --- |
| Versão base PSP | OK | Pode conter erros; ainda falta corrigir o ganho das vozes durante as batalhas. |
| Substituição por assets do PS2 | Em progresso | Será integrada sem quebrar a base PSP verificada. |
| Implementação de controles e IA do PS2 | Planejada | Projeto e verificação ainda pendentes. |
| Implementação de jogo online | Planejada | A arquitetura de rede ainda não foi implementada. |
| Mods: imagens e texturas | Planejada | Está prevista uma rota legal com arquivos fornecidos pelo usuário. |
| Mods: modelos e cenários | Planejada | Planejada após a estabilização da base e do pipeline de assets. |

### Aviso legal

Este é um projeto de código-fonte voltado à interoperabilidade e não constitui
aconselhamento jurídico. Não é afiliado nem aprovado pela Capcom, Type-Moon,
Cavia, Sony ou qualquer outro titular. Todas as marcas e materiais protegidos
do jogo pertencem aos seus respectivos proprietários.

O repositório e suas Releases não devem conter nem redistribuir ISO/CSO do jogo,
EBOOT criptografado ou descriptografado, arquivos extraídos, áudio, vídeo,
texturas, modelos, fontes, saves, savestates, fontes AOT geradas, objetos ou
executáveis que incorporem código derivado do jogo. Cada usuário deve fornecer
suas próprias entradas obtidas legalmente e gerar derivados de modo local e
privado. Possuir uma cópia não concede automaticamente direitos de
redistribuição.

O código original do launcher, preparação e atualização desta pasta é
distribuído sob a licença MIT incluída. O snapshot incluído do PSPRecomp é uma
versão modificada especificamente para este projeto e permanece sob sua licença
MIT; as licenças e avisos devem ser preservados. O PPSSPP usa GPL-2.0-or-later
e não é incluído: o usuário fornece separadamente um arquivo oficial. Qualquer
redistribuição do PPSSPP deve cumprir de forma independente suas obrigações de
licença e código-fonte.

O diretório gerado `out` é privado e não deve ser enviado sem uma análise de
direitos separada. Consulte `LEGAL.md` para o escopo canônico.
